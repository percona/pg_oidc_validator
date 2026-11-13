#include <jwt-cpp/jwt.h>

#include <iterator>
#include <ranges>
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
#include "nodes/pg_list.h"
#include "utils/guc.h"
#include "utils/varlena.h"

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

// When non-empty, the validator uses this URL (instead of pg_hba's `issuer=`)
static char* discovery_url_override = nullptr;

// When non-empty, a comma separated list of audiences; the JWT `aud` claim has to name one of them
static char* audience = nullptr;

// Splits the audience GUC into its elements, or, when `audiences` is null, only checks that the value is a well
// formed list. Returns nullptr on success, otherwise a message describing why it is not.
static const char* split_audience_list(const char* value, audiences_t* audiences) {
  if (value == nullptr || *value == '\0') {
    return nullptr;
  }

  // SplitGUCList() carves the list up in place and returns pointers into rawstring, so it has to stay alive until
  // the elements have been copied out.
  char* rawstring = pstrdup(value);
  List* elements = NIL;
  const char* error = nullptr;

  if (SplitGUCList(rawstring, ',', &elements)) {
    const int count = list_length(elements);

    for (int i = 0; i < count; i++) {
      const auto* element = static_cast<const char*>(list_nth(elements, i));

      if (*element == '\0') {
        error = "audience must not be empty";
        break;
      }

      if (audiences != nullptr) {
        audiences->insert(element);
      }
    }
  } else {
    error = "list syntax is invalid";
  }

  list_free(elements);
  pfree(rawstring);

  return error;
}

// Reports a malformed list as soon as it is configured, but deliberately accepts it. Returning false would leave
// the GUC at its previous value --- its empty default, at postmaster startup, where core downgrades a rejected
// value to a WARNING and carries on --- and an empty audience means "do not validate `aud`", so a typo would
// silently switch the check off. The value has to reach validate_token(), which refuses the login instead.
static bool check_audience(char** newval, void**, GucSource) {
  const char* error = split_audience_list(*newval, nullptr);

  if (error != nullptr) {
    ereport(WARNING, (errmsg("invalid value for parameter \"pg_oidc_validator.audience\": \"%s\"", *newval),
                      errdetail("%s", error), errhint("OAuth logins are refused until this is corrected.")));
  }

  return true;
}

extern "C" void _PG_init() {
  DefineCustomStringVariable("pg_oidc_validator.authn_field",
                             gettext_noop("OAuth field used for matching PostgreSQL users"), nullptr, &authn_field,
                             "sub", PGC_SIGHUP, 0, nullptr, nullptr, nullptr);
  DefineCustomStringVariable(
      "pg_oidc_validator.discovery_url_override",
      gettext_noop("If set, fetch OIDC discovery and JWKS from this URL instead of the pg_hba issuer."),
      gettext_noop("The JWT `iss` claim is still validated against the pg_hba issuer."), &discovery_url_override, "",
      PGC_SIGHUP, 0, nullptr, nullptr, nullptr);
  DefineCustomStringVariable(
      "pg_oidc_validator.audience", gettext_noop("Comma separated list of audiences accepted in the JWT `aud` claim."),
      gettext_noop("A token is accepted if its `aud` claim names any of the listed audiences. If left empty, the "
                   "`aud` claim is not validated, and an access token the issuer minted for another service is "
                   "accepted as a login."),
      // No GUC_LIST_QUOTE: init_custom_variable() rejects that flag with elog(FATAL) because pg_dump cannot
      // re-quote a list belonging to an extension that is not loaded. SplitGUCList() honours double quoted
      // elements regardless of the flag, so only re-quoting on output is lost.
      &audience, "", PGC_SIGHUP, GUC_LIST_INPUT, check_audience, nullptr, nullptr);
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
  audiences_t accepted_audiences;
  const char* audience_error = pg::pg_try([&]() { return split_audience_list(audience, &accepted_audiences); });

  if (audience_error != nullptr) {
    // check_audience() only warns about a malformed value, so it reaches us intact. Refuse the login rather than
    // fall back to no audience check at all.
    elog(WARNING, "OAuth failed: pg_oidc_validator.audience is invalid: %s", audience_error);
    return false;
  }

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
  const auto verifier = configure_verifier_with_jwks(issuer, accepted_audiences, jwks_info, jwt_kid);
  verifier.verify(decoded_token);
  auto received_scopes = parse_jwt_scopes(decoded_token.get_payload_json()["scp"]);
  const auto json_scope = parse_jwt_scopes(decoded_token.get_payload_json()["scope"]);
  received_scopes.insert(json_scope.begin(), json_scope.end());

  if (required_scopes.empty()) {
    // An explicitly empty scope opts out of scope validation.
    // While this isn't recommended, it gives a workaround for some OIDC providers.
    elog(DEBUG1, "Configured scope is empty; skipping scope validation");
  } else if (received_scopes.empty()) {
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

  if (required_scopes.empty()) {
    res->authorized = true;
  } else if (issuer_is_azure(issuer)) {
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

void validator_shutdown(ValidatorModuleState*) {
  // Detach cache manually, otherwise the destructor will try to do it after shmem_exit already completed
  pg::http_cache::get_instance().detach();
}
