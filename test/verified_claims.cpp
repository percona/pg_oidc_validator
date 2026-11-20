#include <openssl/core_names.h>
#include <openssl/rsa.h>

#include <chrono>
#include <iostream>
#include <memory>

#include "../src/jwk.hpp"

using namespace std::string_literals;
using picojson::array;
using picojson::object;
using picojson::value;

namespace {

std::string public_parameter(EVP_PKEY* key, const char* parameter) {
  BIGNUM* raw = nullptr;
  if (EVP_PKEY_get_bn_param(key, parameter, &raw) != 1) throw std::runtime_error("Cannot read test public key");
  const std::unique_ptr<BIGNUM, decltype(&BN_free)> number(raw, BN_free);
  const auto encoded = jwt::base::encode<jwt::alphabet::base64url>(jwt::helper::bn2raw(number.get()));
  return jwt::base::trim<jwt::alphabet::base64url>(encoded);
}

}  // namespace

int main() try {
  // Only this process trusts the ephemeral signing key. Fixtures have no
  // user identity or delegated permissions and never leave this process.
  jwt::helper::evp_pkey_handle key(EVP_RSA_gen(2048));
  if (!key) throw std::runtime_error("Cannot generate test key");
  const jwt::algorithm::rsa signer(key, EVP_sha256, "RS256");
  const std::string tenant = "00000000-0000-4000-8000-000000000001";
  const std::string issuer = "https://login.microsoftonline.com/" + tenant + "/v2.0";
  const std::string audience = "claims-unit-test";
  object jwk{{"kid", value("fixture"s)},
             {"kty", value("RSA"s)},
             {"use", value("sig"s)},
             {"n", value(public_parameter(key.get(), OSSL_PKEY_PARAM_RSA_N))},
             {"e", value(public_parameter(key.get(), OSSL_PKEY_PARAM_RSA_E))},
             {"issuer", value("https://login.microsoftonline.com/{tenantid}/v2.0"s)}};
  auto make_verifier = [&] {
    return configure_verifier_with_jwks(issuer, value(object{{"keys", value(array{value(jwk)})}}), "fixture", audience);
  };
  auto verifier = make_verifier();
  const auto now = std::chrono::system_clock::now();
  const auto epoch = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
  const object valid{{"iss", value(issuer)},
                     {"aud", value(audience)},
                     {"tid", value(tenant)},
                     {"ver", value("2.0"s)},
                     {"exp", value(int64_t(epoch + 300))}};

  auto check = [&](const char* name, const object& claims, bool expect_success) {
    auto fixture = jwt::create().set_key_id("fixture");
    for (const auto& [claim, val] : claims) fixture.set_payload_claim(claim, val);
    const auto decoded = jwt::decode(fixture.sign(signer));
    std::error_code ec;
    verifier.verify(decoded, ec);
    if ((!ec) != expect_success) throw std::runtime_error(name);
  };
  auto changed = [&](const char* claim, value replacement) {
    auto claims = valid;
    claims[claim] = std::move(replacement);
    return claims;
  };
  auto missing = [&](const char* claim) {
    auto claims = valid;
    claims.erase(claim);
    return claims;
  };

  check("valid", valid, true);
  check("missing expiry", missing("exp"), false);
  check("expired", changed("exp", value(int64_t(epoch - 60))), false);
  check("non-numeric expiry", changed("exp", value("later"s)), false);
  check("fractional expiry", changed("exp", value(static_cast<double>(epoch) + 300.5)), true);
  check("missing audience", missing("aud"), false);
  check("different audience", changed("aud", value("unrelated"s)), false);
  check("array with audience", changed("aud", value(array{value("other"s), value(audience)})), true);
  check("array without audience", changed("aud", value(array{value("other"s)})), false);
  check("non-string audience", changed("aud", value(true)), false);
  check("missing tenant", missing("tid"), false);
  check("different tenant", changed("tid", value("00000000-0000-4000-8000-000000000002"s)), false);
  check("different issuer", changed("iss", value("https://issuer.invalid"s)), false);
  check("different token version", changed("ver", value("1.0"s)), false);

  jwk["issuer"] = value("https://login.microsoftonline.com/00000000-0000-4000-8000-000000000002/v2.0"s);
  verifier = make_verifier();
  check("key restricted to another tenant", valid, false);
  std::cout << "Verified claim checks passed\n";
} catch (const std::exception& error) {
  std::cerr << "Claim check failed: " << error.what() << '\n';
  return 1;
}
