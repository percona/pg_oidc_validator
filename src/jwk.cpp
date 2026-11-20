#include "jwk.hpp"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <ranges>
#include <regex>
#include <sstream>
#include <string_view>

namespace {

constexpr std::string_view entra_v2_host = "https://login.microsoftonline.com/";
constexpr std::string_view entra_v1_host = "https://sts.windows.net/";

bool issuer_is_azure(const std::string& issuer) {
  return issuer.starts_with(entra_v2_host) || issuer.starts_with(entra_v1_host);
}

std::string entra_tenant_id(const std::string& issuer) {
  const bool v2 = issuer.starts_with(entra_v2_host);
  const auto path = std::string_view(issuer).substr((v2 ? entra_v2_host : entra_v1_host).size());
  const auto tenant = path.substr(0, path.find('/'));
  const auto suffix = path.substr(tenant.size());
  static const std::regex guid(R"([[:xdigit:]]{8}-[[:xdigit:]]{4}-[[:xdigit:]]{4}-[[:xdigit:]]{4}-[[:xdigit:]]{12})");
  if (!std::regex_match(tenant.begin(), tenant.end(), guid) || suffix != (v2 ? "/v2.0" : "/")) {
    throw std::runtime_error("Entra issuer must identify a single tenant by GUID");
  }
  return std::string(tenant);
}

// The default jwt-cpp check makes exp optional. Require it and compare the
// NumericDate in seconds, including fractional values permitted by RFC 7519.
void require_expiration(const jwt::verify_ops::verify_context<jwt::traits::kazuho_picojson>& ctx, std::error_code& ec) {
  const auto claim = ctx.get_claim(ec);
  if (ec) return;
  const auto type = claim.get_type();
  if (type != jwt::json::type::integer && type != jwt::json::type::number) {
    ec = jwt::error::token_verification_error::claim_type_missmatch;
    return;
  }
  const double expiration =
      type == jwt::json::type::integer ? static_cast<double>(claim.as_integer()) : claim.as_number();
  const std::chrono::duration<double> now = ctx.current_time.time_since_epoch();
  if (!(expiration > now.count())) ec = jwt::error::token_verification_error::token_expired;
}

bool oidc_scope(const std::string& scope) {
  return scope == "openid" || scope == "profile" || scope == "email" || scope == "offline_access";
}

}  // namespace

void configure_rsa_key(const picojson::object& keyObject, const std::string& kid, const std::string& alg,
                       jwt_verifier& verifier) {
  const auto n_it = keyObject.find("n");
  const auto e_it = keyObject.find("e");

  if (n_it == keyObject.end() || e_it == keyObject.end()) {
    throw std::runtime_error("RSA key missing required 'n'/'e' components (kid: " + kid + ")");
  }

  const std::string n = n_it->second.to_str();
  const std::string e = e_it->second.to_str();

  const std::string pem_key = jwt::helper::create_public_key_from_rsa_components(n, e);

  if (alg == "RS256" || alg.empty()) {
    verifier = verifier.allow_algorithm(jwt::algorithm::rs256(pem_key));
  } else if (alg == "RS384") {
    verifier = verifier.allow_algorithm(jwt::algorithm::rs384(pem_key));
  } else if (alg == "RS512") {
    verifier = verifier.allow_algorithm(jwt::algorithm::rs512(pem_key));
  } else if (alg == "PS256") {
    verifier = verifier.allow_algorithm(jwt::algorithm::ps256(pem_key));
  } else if (alg == "PS384") {
    verifier = verifier.allow_algorithm(jwt::algorithm::ps384(pem_key));
  } else if (alg == "PS512") {
    verifier = verifier.allow_algorithm(jwt::algorithm::ps512(pem_key));
  } else {
    throw std::runtime_error("Unsupported RSA algorithm: " + alg + " (kid: " + kid + ")");
  }
}

void configure_ec_key(const picojson::object& keyObject, const std::string& kid, const std::string& alg,
                      jwt_verifier& verifier) {
  const auto x_it = keyObject.find("x");
  const auto y_it = keyObject.find("y");
  const auto crv_it = keyObject.find("crv");

  if (x_it == keyObject.end() || y_it == keyObject.end() || crv_it == keyObject.end()) {
    throw std::runtime_error("EC key missing required 'x'/'y'/'crv' components (kid: " + kid + ")");
  }

  const std::string x = x_it->second.to_str();
  const std::string y = y_it->second.to_str();
  const std::string crv = crv_it->second.to_str();

  const std::string pem_key = jwt::helper::create_public_key_from_ec_components(crv, x, y);

  if (alg == "ES256" && crv == "P-256") {
    verifier = verifier.allow_algorithm(jwt::algorithm::es256(pem_key));
  } else if (alg == "ES384" && crv == "P-384") {
    verifier = verifier.allow_algorithm(jwt::algorithm::es384(pem_key));
  } else if (alg == "ES512" && crv == "P-521") {
    verifier = verifier.allow_algorithm(jwt::algorithm::es512(pem_key));
  } else {
    throw std::runtime_error("Unsupported EC algorithm/curve combination: " + alg + "/" + crv + " (kid: " + kid + ")");
  }
}

void configure_hmac_key(const picojson::object& keyObject, const std::string& kid, const std::string& alg,
                        jwt_verifier& verifier) {
  const auto k_it = keyObject.find("k");

  if (k_it == keyObject.end()) {
    throw std::runtime_error("HMAC key missing required 'k' component (kid: " + kid + ")");
  }

  const std::string k = keyObject.at("k").to_str();

  if (alg == "HS256") {
    verifier = verifier.allow_algorithm(jwt::algorithm::hs256{k});
  } else if (alg == "HS384") {
    verifier = verifier.allow_algorithm(jwt::algorithm::hs384{k});
  } else if (alg == "HS512") {
    verifier = verifier.allow_algorithm(jwt::algorithm::hs512{k});
  } else {
    throw std::runtime_error("Unsupported HMAC algorithm: " + alg + " (kid: " + kid + ")");
  }
}

std::string get_required_parameter(picojson::object const& key_object, std::string const& name) {
  if (!key_object.contains(name)) {
    throw std::runtime_error("Required parameter '" + name + "' is missing");
  }
  return key_object.at(name).to_str();
}

jwt_verifier configure_verifier_with_jwks(const std::string& issuer, const picojson::value& jwksInfo,
                                          const std::string& required_kid, const std::string& audience) {
  if (audience.empty()) {
    throw std::runtime_error("pg_oidc_validator.audience is required");
  }

  auto verifier = jwt::verify().with_issuer(issuer).with_audience(audience).with_claim("exp", require_expiration);
  if (issuer_is_azure(issuer)) {
    const auto tenant = entra_tenant_id(issuer);
    verifier.with_claim("tid", jwt::claim(tenant));
    verifier.with_claim("ver", jwt::claim(std::string(issuer.starts_with(entra_v2_host) ? "2.0" : "1.0")));
  }

  if (!jwksInfo.is<picojson::object>()) {
    throw std::runtime_error("JWKS info is not a JSON object");
  }

  const auto& jwks_object = jwksInfo.get<picojson::object>();
  const auto keys = jwks_object.at("keys").get<picojson::array>();

  for (const auto& key_value : keys) {
    if (!key_value.is<picojson::object>()) {
      throw std::runtime_error("Invalid JWKS format: not a json object");
    }

    const auto& key_object = key_value.get<picojson::object>();

    const std::string use = get_required_parameter(key_object, "use");

    if (use != "sig") {
      continue;
    }

    const std::string kid = get_required_parameter(key_object, "kid");

    if (kid != required_kid) {
      // Skip keys that don't match the required key ID
      continue;
    }

    // Entra may publish tenant-specific keys alongside shared signing keys.
    if (issuer_is_azure(issuer) && key_object.contains("issuer")) {
      auto key_issuer = key_object.at("issuer").to_str();
      const auto placeholder = key_issuer.find("{tenantid}");
      if (placeholder != std::string::npos) {
        key_issuer.replace(placeholder, sizeof("{tenantid}") - 1, entra_tenant_id(issuer));
      }
      if (key_issuer != issuer) {
        continue;
      }
    }

    const std::string kty = get_required_parameter(key_object, "kty");
    const std::string alg = key_object.contains("alg") ? key_object.at("alg").to_str() : "";

    if (kty == "RSA") {
      configure_rsa_key(key_object, kid, alg, verifier);
    } else if (kty == "EC") {
      configure_ec_key(key_object, kid, alg, verifier);
    } else if (kty == "oct") {
      configure_hmac_key(key_object, kid, alg, verifier);
    } else {
      throw std::runtime_error("Unsupported key type: " + kty + " (kid: " + kid + ")");
    }

    break;
  }

  return verifier;
}

std::string issuer_info_url(std::string const& issuer_url) {
  static const auto* const info_end = "/.well-known/openid-configuration";

  if (issuer_url.ends_with(info_end)) {
    return issuer_url;
  }

  return issuer_url + info_end;
}

scopes_t parse_jwt_scopes(const picojson::value& jsonScopes) {
  if (jsonScopes.is<picojson::array>()) {
    scopes_t scopes;
    for (const auto& scope : jsonScopes.get<picojson::array>()) {
      if (!scope.is<std::string>()) return {};
      scopes.insert(scope.get<std::string>());
    }
    return scopes;
  }
  if (jsonScopes.is<std::string>()) {
    std::istringstream iss(jsonScopes.get<std::string>());
    return scopes_t(std::istream_iterator<std::string>(iss), std::istream_iterator<std::string>{});
  }

  return {};
}

bool azure_scopes_match(const scopes_t& required_scopes, const scopes_t& received_scopes) {
  bool has_resource_scope = false;
  for (const auto& scope : required_scopes) {
    if (oidc_scope(scope)) continue;
    // Entra returns delegated permissions as short names, even when the client
    // requested a resource-qualified scope.
    const auto slash = scope.rfind('/');
    const auto name = slash == std::string::npos ? scope : scope.substr(slash + 1);
    if (name.empty() || !received_scopes.contains(name)) return false;
    has_resource_scope = true;
  }
  return has_resource_scope;
}

bool token_scopes_match(const std::string& issuer, const scopes_t& required_scopes, const picojson::object& payload) {
  const auto scp = payload.find("scp");
  if (issuer_is_azure(issuer)) {
    // Only delegated access tokens carry the Entra scp string. An ID token
    // or an application-only token must not authorize a database user.
    return scp != payload.end() && scp->second.is<std::string>() &&
           azure_scopes_match(required_scopes, parse_jwt_scopes(scp->second));
  }
  if (required_scopes.empty()) return true;
  scopes_t received;
  if (scp != payload.end()) received = parse_jwt_scopes(scp->second);
  if (const auto scope = payload.find("scope"); scope != payload.end()) {
    const auto values = parse_jwt_scopes(scope->second);
    received.insert(values.begin(), values.end());
  }
  return std::ranges::includes(received, required_scopes);
}
