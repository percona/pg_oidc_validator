#include <jwt-cpp/jwt.h>

#include <iterator>
#include <ranges>
#include <sstream>
#include <string>

#include "http_cache.hpp"
#include "http_client.hpp"
#include "jwk.hpp"

extern "C" {
#include "postgres.h"
//
#include "fmgr.h"
#include "libpq/libpq-be.h"
#include "libpq/oauth.h"
#include "miscadmin.h"
#include "utils/guc.h"

PG_MODULE_MAGIC;
}

bool validate_token(const ValidatorModuleState* state, const char* token, const char* role,
                    ValidatorModuleResult* result);

static const OAuthValidatorCallbacks validator_callbacks = {PG_OAUTH_VALIDATOR_MAGIC, nullptr, nullptr, validate_token};

extern "C" {
const OAuthValidatorCallbacks* _PG_oauth_validator_module_init(void) { return &validator_callbacks; }
}

static char* authn_field = nullptr;

// When non-empty, the validator uses this URL (instead of pg_hba's `issuer=`)
static char* discovery_url_override = nullptr;

extern "C" void _PG_init() {
  DefineCustomStringVariable("pg_oidc_validator.authn_field",
                             gettext_noop("OAuth field used for matching PostgreSQL users"), nullptr, &authn_field,
                             "sub", PGC_SIGHUP, 0, nullptr, nullptr, nullptr);
  DefineCustomStringVariable(
      "pg_oidc_validator.discovery_url_override",
      gettext_noop("If set, fetch OIDC discovery and JWKS from this URL instead of the pg_hba issuer."),
      gettext_noop("The JWT `iss` claim is still validated against the pg_hba issuer."), &discovery_url_override, "",
      PGC_SIGHUP, 0, nullptr, nullptr, nullptr);
}

bool validate_token(const ValidatorModuleState* state, const char* token, const char* role,
                    ValidatorModuleResult* res) try {
  // initialize return values to deny
  res->authn_id = nullptr;
  res->authorized = false;

  try {
    pg::pg_try([&]() { pg::http_cache::get_instance().attach(); });
  } catch (const pg::postgres_exception& ex) {
    elog(WARNING, "Failed to attach to HTTP cache: %s", ex.what());
  }

  std::istringstream required_iss(std::string(MyProcPort->hba->oauth_scope));
  const scopes_t required_scopes(std::istream_iterator<std::string>(required_iss),
                                 std::istream_iterator<std::string>{});
  const std::string issuer = MyProcPort->hba->oauth_issuer;
  const std::string discovery_url =
      (discovery_url_override != nullptr && *discovery_url_override != '\0') ? discovery_url_override : issuer;

  http_client http;
  const auto issuer_info = http.get_json(issuer_info_url(discovery_url));

  if (!issuer_info.is<picojson::object>()) {
    elog(WARNING, "OpenID configuration from issuer is not a JSON object");
    return false;
  }

  const auto& issuer_object = issuer_info.get<picojson::object>();

  if (!issuer_object.contains("jwks_uri")) {
    elog(WARNING, "jwks_uri not present in issuer info. Is this an OIDC provider?");
    return false;
  }

  const auto jwks_uri = issuer_object.at("jwks_uri").to_str();

  if (jwks_uri.empty()) {
    elog(WARNING, "Could not parse JWKS URI from issuer configuration");
    return false;
  }

  const auto jwks_info = http.get_json(jwks_uri);
  const auto decoded_token = jwt::decode(token);
  const std::string jwt_kid = decoded_token.get_header_claim("kid").as_string();
  const auto verifier = configure_verifier_with_jwks(issuer, jwks_info, jwt_kid);
  verifier.verify(decoded_token);
  auto received_scopes = parse_jwt_scopes(decoded_token.get_payload_json()["scp"]);
  const auto json_scope = parse_jwt_scopes(decoded_token.get_payload_json()["scope"]);
  received_scopes.insert(json_scope.begin(), json_scope.end());

  if (received_scopes.empty()) {
    elog(WARNING, "Access token contains no scopes");
  }

  const auto payload = decoded_token.get_payload_json();

  PG_TRY();
  {
    if (!payload.contains(authn_field)) {
      std::string claims_str;
      for (const auto& kv : payload) {
        if (!claims_str.empty()) claims_str += ", ";
        claims_str += kv.first;
      }
      elog(WARNING, "OAuth failed: claim '%s' (authn_field) is missing. Available claims: %s", authn_field,
           claims_str.c_str());
      return false;
    }
    res->authn_id = pstrdup(payload.at(authn_field).to_str().c_str());
  }
  PG_CATCH();
  {
    elog(WARNING, "OAuth failed: out of memory");
    return false;
  }
  PG_END_TRY();

  if (issuer_is_azure(issuer)) {
    if (strcmp(authn_field, "sub") == 0) {
      elog(WARNING,
           "sub field is not guaranteed to be unique with Entra ID, consider using a different field for user "
           "matching.");
    }
    // Azure is broken: it expects us to provide full tenant-id
    // qualified scopes for the request, but then it returns the simple name
    // in the JWT instead. This requires a custom matching code.
    res->authorized = azure_scopes_match(required_scopes, received_scopes);
  } else {
    res->authorized = std::ranges::includes(received_scopes, required_scopes);
  }

  if (!res->authorized) {
    std::string req_str;
    for (const auto& s : required_scopes) {
      if (!req_str.empty()) req_str += ", ";
      req_str += s;
    }
    std::string rec_str;
    for (const auto& s : received_scopes) {
      if (!rec_str.empty()) rec_str += ", ";
      rec_str += s;
    }
    elog(LOG, "Authorization failed because of scope mismatch. Required scopes: %s. Received scopes: %s",
         req_str.c_str(), rec_str.c_str());
  } else {
    elog(DEBUG1, "OIDC validator authorizing user as '%s'", res->authn_id);
  }

  return true;
} catch (const std::exception& ex) {
  elog(WARNING, "OAuth validation failed with exception: %s", ex.what());
  return false;
} catch (...) {
  elog(WARNING, "OAuth validation failed with unknown internal error");
  return false;
}
