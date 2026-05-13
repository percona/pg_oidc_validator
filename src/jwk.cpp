#include "jwk.hpp"

#include <ranges>
#include <regex>

extern "C" {
#include "postgres.h"
}

namespace {

std::string extract_azure_tenant_id(const std::string& issuer) {
  std::regex tenant_regex(R"([a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12})");
  std::smatch match;

  if (std::regex_search(issuer, match, tenant_regex)) {
    return match[0];
  }

  throw std::runtime_error("Couldn't extract tenant_id from azure issuer");
}

std::string convert_azure_issuer_to_jwt_format(const std::string& config_issuer) {
  std::string tenant_id = extract_azure_tenant_id(config_issuer);

  // Convert login.microsoftonline.com format to sts.windows.net format
  if (config_issuer.find("login.microsoftonline.com") != std::string::npos) {
    return "https://sts.windows.net/" + tenant_id + "/";
  }

  return config_issuer;
}

}  // namespace

void configure_rsa_key(const picojson::object& keyObject, const std::string& kid, const std::string& alg,
                       jwt_verifier& verifier) {
  const auto n_it = keyObject.find("n");
  const auto e_it = keyObject.find("e");

  if (n_it == keyObject.end() || e_it == keyObject.end()) {
    throw std::runtime_error(std::format("RSA key missing required 'n'/'e' components (kid: {})", kid));
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
    throw std::runtime_error(std::format("Unsupported RSA algorithm: {} (kid: {})", alg, kid));
  }
}

void configure_ec_key(const picojson::object& keyObject, const std::string& kid, const std::string& alg,
                      jwt_verifier& verifier) {
  const auto x_it = keyObject.find("x");
  const auto y_it = keyObject.find("y");
  const auto crv_it = keyObject.find("crv");

  if (x_it == keyObject.end() || y_it == keyObject.end() || crv_it == keyObject.end()) {
    throw std::runtime_error(std::format("EC key missing required 'x'/'y'/'crv' components (kid: {})", kid));
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
    throw std::runtime_error(std::format("Unsupported EC algorithm/curve combination: {}/{} (kid: {})", alg, crv, kid));
  }
}

void configure_hmac_key(const picojson::object& keyObject, const std::string& kid, const std::string& alg,
                        jwt_verifier& verifier) {
  const auto k_it = keyObject.find("k");

  if (k_it == keyObject.end()) {
    throw std::runtime_error(std::format("HMAC key missing required 'k' component (kid: {})", kid));
  }

  const std::string k = keyObject.at("k").to_str();

  if (alg == "HS256") {
    verifier = verifier.allow_algorithm(jwt::algorithm::hs256{k});
  } else if (alg == "HS384") {
    verifier = verifier.allow_algorithm(jwt::algorithm::hs384{k});
  } else if (alg == "HS512") {
    verifier = verifier.allow_algorithm(jwt::algorithm::hs512{k});
  } else {
    throw std::runtime_error(std::format("Unsupported HMAC algorithm: {} (kid: {})", alg, kid));
  }
}

std::string get_required_parameter(picojson::object const& key_object, std::string const& name) {
  if (!key_object.contains(name)) {
    throw std::runtime_error(std::format("Required parameter '{}' is missing", name));
  }
  return key_object.at(name).to_str();
}

jwt_verifier configure_verifier_with_jwks(const std::string& issuer, const picojson::value& jwksInfo,
                                          const std::string& required_kid) {
  std::string expected_issuer = issuer;

  auto verifier = jwt::verify().with_issuer(expected_issuer);

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

    const std::string kty = get_required_parameter(key_object, "kty");
    const std::string alg = key_object.contains("alg") ? key_object.at("alg").to_str() : "";

    if (kty == "RSA") {
      configure_rsa_key(key_object, kid, alg, verifier);
    } else if (kty == "EC") {
      configure_ec_key(key_object, kid, alg, verifier);
    } else if (kty == "oct") {
      configure_hmac_key(key_object, kid, alg, verifier);
    } else {
      throw std::runtime_error(std::format("Unsupported key type: {} (kid: {})", kty, kid));
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

bool issuer_is_azure(const std::string& issuer) {
  return issuer.contains("login.microsoftonline.com") || issuer.contains("sts.windows.net");
}

scopes_t parse_jwt_scopes(const picojson::value& jsonScopes) {
  if (jsonScopes.is<picojson::array>()) {
    const auto scope_range =
        jsonScopes.get<picojson::array>() | std::ranges::views::transform(&picojson::value::to_str);
    return {scope_range.begin(), scope_range.end()};
  }
  if (jsonScopes.is<std::string>()) {
    auto scope_range = jsonScopes.get<std::string>() | std::views::split(' ') |
                       std::views::transform([](auto r) { return std::string(r.data(), r.size()); });
    return {scope_range.begin(), scope_range.end()};
  }

  return {};
}

bool azure_scopes_match(const scopes_t& required_scopes, const scopes_t& received_scopes) {
  for (const auto& required_scope : required_scopes) {  // NOLINT: readability-use-anyofallof
    if (required_scope == "openid") {
      // Azure never returns openid in the output :(
      continue;
    }

    if (received_scopes.contains(required_scope)) {
      continue;
    }

    // For Azure, check if required scope ends with "/<scope-name>"
    // and received scope matches the part after the last "/"
    auto last_slash = required_scope.find_last_of('/');
    if (last_slash != std::string::npos && last_slash < required_scope.length() - 1) {
      std::string scope_suffix = required_scope.substr(last_slash + 1);
      if (!received_scopes.contains(scope_suffix)) {
        return false;
      }
    }
  }

  return true;
}
