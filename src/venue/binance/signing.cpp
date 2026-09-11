#include "venue/binance/signing.hpp"

#include <fmt/format.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <array>
#include <cstddef>
#include <string>

namespace exchange::venue::binance {
namespace {

/// Bytes in a SHA-256 digest, which is @c SIGNATURE_CHARS / 2.
constexpr unsigned SHA256_BYTES = 32;

constexpr std::string_view HEX_DIGITS = "0123456789abcdef";

} // namespace

std::string sign(std::string_view payload, std::string_view secret) {
	std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
	unsigned length = 0;

	// The one-shot HMAC rather than the HMAC_CTX dance: there is nothing to
	// stream here - a Binance query is a few hundred bytes and is complete
	// before signing starts.
	const unsigned char *const out =
		HMAC(EVP_sha256(),
			 secret.data(),
			 static_cast<int>(secret.size()),
			 reinterpret_cast<const unsigned char *>(payload.data()),
			 payload.size(),
			 digest.data(),
			 &length);
	if (out == nullptr || length != SHA256_BYTES) return {};

	std::string hex;
	hex.reserve(SIGNATURE_CHARS);
	for (unsigned i = 0; i < length; ++i) {
		hex.push_back(HEX_DIGITS[digest[i] >> 4U]);
		hex.push_back(HEX_DIGITS[digest[i] & 0x0FU]);
	}
	return hex;
}

std::string sign_query(std::string_view query, std::string_view secret) {
	// An empty secret is a configuration mistake, and appending a signature
	// computed over nothing would send a request that looks signed and is not.
	// Returning the query unsigned makes the venue refuse it by name.
	if (secret.empty()) return std::string(query);
	return fmt::format("{}&signature={}", query, sign(query, secret));
}

} // namespace exchange::venue::binance
