#pragma once

#include <jwt-cpp/jwt.h>

#include <string>

#if __has_include(<flat_set>)
#include <flat_set>
using scopes_t = std::flat_set<std::string>;
#else
#include <set>
using scopes_t = std::set<std::string>;
#endif

using jwt_verifier = jwt::verifier<jwt::default_clock, jwt::traits::kazuho_picojson>;

jwt_verifier configure_verifier_with_jwks(const std::string& issuer, const picojson::value& jwksInfo,
                                          const std::string& required_kid, const std::string& audience);

std::string issuer_info_url(std::string const& issuer_url);

scopes_t parse_jwt_scopes(const picojson::value& jsonScopes);
bool azure_scopes_match(const scopes_t& required_scopes, const scopes_t& received_scopes);
bool token_scopes_match(const std::string& issuer, const scopes_t& required_scopes, const picojson::object& payload);
