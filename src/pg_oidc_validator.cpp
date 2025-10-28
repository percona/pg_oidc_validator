#include <jwt-cpp/jwt.h>

#include <ranges>
#include <string>

#include "http_client.hpp"
#include "jwk.hpp"

extern "C" {
#include "postgres.h"
//
#include "fmgr.h"
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

extern "C" void _PG_init() {
  DefineCustomStringVariable("pg_oidc_validator.authn_field",
                             gettext_noop("OAuth field used for matching PostgreSQL users"), nullptr, &authn_field,
                             "sub", PGC_SIGHUP, 0, nullptr, nullptr, nullptr);
}

bool validate_token(const ValidatorModuleState* state, const char* token, const char* role,
                    ValidatorModuleResult* res) try {
  // initialize return values to deny
  res->authn_id = nullptr;
  res->authorized = false;

  auto required_scopes_range = std::string(MyProcPort->hba->oauth_scope) | std::views::split(' ') |
                               std::views::transform([](auto r) { return std::string(r.data(), r.size()); });

  const scopes_t required_scopes(required_scopes_range.begin(), required_scopes_range.end());
  const std::string issuer = MyProcPort->hba->oauth_issuer;

  http_client http;
  const auto issuer_info = http.get_json(issuer_info_url(issuer));

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
      const auto available_claims = payload | std::views::keys | std::views::join_with(std::string(", "));
      const std::string claims_str(available_claims.begin(), available_claims.end());
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
    const auto req = required_scopes | std::views::join_with(std::string(", "));
    const std::string req_str(req.begin(), req.end());
    const auto rec = received_scopes | std::views::join_with(std::string(", "));
    const std::string rec_str(rec.begin(), rec.end());
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
