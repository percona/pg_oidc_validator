#include "jwk.hpp"

#include <cstdlib>
#include <iostream>

namespace {
const std::string tenant = "11111111-2222-3333-4444-555555555555";
const std::string issuer = "https://login.microsoftonline.com/" + tenant + "/v2.0";

void check(bool condition, const char* description) {
  if (!condition) {
    std::cerr << description << '\n';
    std::exit(EXIT_FAILURE);
  }
}
}  // namespace

int main() {
  picojson::object claims{{"scp", picojson::value(std::string("connect reporting"))},
                          {"tid", picojson::value(tenant)},
                          {"ver", picojson::value(std::string("2.0"))}};
  check(token_scopes_match(issuer, {"openid", "api://database/connect", "reporting"}, claims),
        "A delegated grant must satisfy every configured resource permission");
  check(!token_scopes_match(issuer, {"connect", "maintenance"}, claims), "Reject a missing unqualified permission");
  check(!token_scopes_match(issuer, {"api://database/maintenance"}, claims), "Reject a missing qualified permission");
  check(!token_scopes_match(issuer, {"openid", "profile", "email"}, claims), "Identity scopes grant no database access");
  check(!token_scopes_match(issuer, {}, claims), "Entra requires a resource permission");
  claims.erase("scp");
  claims["scope"] = picojson::value(std::string("connect"));
  check(!token_scopes_match(issuer, {"connect"}, claims), "Entra requires a delegated scp claim");
  check(token_scopes_match("https://issuer.example", {"connect"}, claims), "Other providers can use scope");
  check(token_scopes_match("https://issuer.example", {}, {}), "Other providers can configure an empty scope");
  return EXIT_SUCCESS;
}
