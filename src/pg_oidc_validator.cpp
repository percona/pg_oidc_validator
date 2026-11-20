#include <jwt-cpp/jwt.h>

#include <iterator>
#include <sstream>
#include <string>

#include "http_cache.hpp"
#include "http_client.hpp"
#include "jwk.hpp"

static const char* ModuleName = "pg_oidc_validator";
static const char* ModuleVersion = "1.1.0";

extern "C" {
#include "postgres.h"
//
#include "fmgr.h"
#include "libpq/libpq-be.h"
#include "libpq/oauth.h"
#include "miscadmin.h"
#include "utils/guc.h"

PG_MODULE_MAGIC_EXT(.name = ModuleName, .version = ModuleVersion);
}

void validator_shutdown(ValidatorModuleState*);
bool validate_token(const ValidatorModuleState* state, const char* token, const char* role,
                    ValidatorModuleResult* result);

static const OAuthValidatorCallbacks validator_callbacks = {PG_OAUTH_VALIDATOR_MAGIC, nullptr, validator_shutdown,
                                                            validate_token};

extern "C" {
const OAuthValidatorCallbacks* _PG_oauth_validator_module_init(void) { return &validator_callbacks; }
}

static char* authn_field = nullptr;
static char* audience = nullptr;

// When non-empty, the validator uses this URL (instead of pg_hba's `issuer=`)
static char* discovery_url_override = nullptr;

extern "C" void _PG_init() {
  DefineCustomStringVariable("pg_oidc_validator.audience",
                             gettext_noop("Expected audience of access tokens for this PostgreSQL server."), nullptr,
                             &audience, "", PGC_SIGHUP, 0, nullptr, nullptr, nullptr);
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

  if (audience == nullptr || *audience == '\0') {
    elog(WARNING, "OAuth requires pg_oidc_validator.audience to be configured");
    return false;
  }
  if (MyProcPort->hba->oauth_skip_usermap) {
    elog(WARNING, "pg_oidc_validator requires PostgreSQL identity mapping");
    return false;
  }

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
  const auto verifier = configure_verifier_with_jwks(issuer, jwks_info, jwt_kid, audience);
  verifier.verify(decoded_token);

  const auto payload = decoded_token.get_payload_json();
  if (!token_scopes_match(issuer, required_scopes, payload)) {
    elog(LOG, "OAuth access token does not grant the configured scopes");
    return true;
  }

  const auto identity = payload.find(authn_field);
  if (identity == payload.end() || !identity->second.is<std::string>()) {
    elog(WARNING, "OAuth identity claim '%s' must be a string", authn_field);
    return false;
  }
  const auto& name = identity->second.get<std::string>();
  if (name.empty() || name.find('\0') != std::string::npos) {
    elog(WARNING, "OAuth identity claim '%s' must be a non-empty PostgreSQL identity", authn_field);
    return false;
  }

  res->authn_id = pg::pg_try([&]() { return pstrdup(name.c_str()); });
  res->authorized = true;
  return true;
} catch (const std::exception& ex) {
  elog(WARNING, "OAuth validation failed with exception: %s", ex.what());
  return false;
} catch (...) {
  elog(WARNING, "OAuth validation failed with unknown internal error");
  return false;
}

void validator_shutdown(ValidatorModuleState*) {
  // Detach cache manually, otherwise the destructor will try to do it after shmem_exit already completed
  pg::http_cache::get_instance().detach();
}
