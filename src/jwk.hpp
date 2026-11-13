#pragma once

#include <jwt-cpp/jwt.h>

#include <string>

#if __has_include(<flat_set>)
#include <flat_set>
using scopes_t = std::flat_set<std::string>;
using audiences_t = std::flat_set<std::string>;
#else
#include <set>
using scopes_t = std::set<std::string>;
using audiences_t = std::set<std::string>;
#endif

using jwt_verifier = jwt::verifier<jwt::default_clock, jwt::traits::kazuho_picojson>;

jwt_verifier configure_verifier_with_jwks(const std::string& issuer, const audiences_t& accepted_audiences,
                                          const picojson::value& jwksInfo, const std::string& required_kid);

std::string issuer_info_url(std::string const& issuer_url);

bool issuer_is_azure(const std::string& issuer);
scopes_t parse_jwt_scopes(const picojson::value& jsonScopes);
bool azure_scopes_match(const scopes_t& required_scopes, const scopes_t& received_scopes);
