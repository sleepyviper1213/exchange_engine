#pragma once
// Where a TLS client finds the roots it verifies against.
//
// `ssl::context::set_default_verify_paths()` asks OpenSSL for its compiled-in
// OPENSSLDIR, which is where a Unix distribution puts a CA bundle and where
// Windows has nothing at all. A vcpkg-built OpenSSL on Windows therefore starts
// with an *empty* trust store, and every `verify_peer` handshake fails with
// "certificate verify failed" no matter how valid the venue's chain is - which
// is exactly the failure that made the WebSocket reader hardcode verify_none in
// the first place, and the reason turning verification on needed this file
// rather than just a flag.
//
// Windows keeps its roots in the system certificate stores instead, reachable
// through CryptoAPI. This bridges the two: enumerate the stores, hand each DER
// certificate to OpenSSL, and the handshake has something to check against.
//
// Linked against crypt32, which transport/CMakeLists already carries for
// Boost.Asio's own Windows bits.

#include <openssl/ssl.h> // IWYU pragma: export - SSL_CTX is the parameter

#include <cstddef>

namespace exchange::transport::detail {

/// @brief How the trust store for @c ctx was populated.
struct trust_store_result {
	/// @brief Certificates added to the context's store. Zero means nothing was
	///        loaded and every @c verify_peer handshake will fail.
	std::size_t certificates = 0;
	/// @brief Where they came from, for a log line - @c "Windows ROOT/CA
	///        stores", @c "OpenSSL default paths", or why neither worked.
	const char *source = "";
};

/**
 * @brief Give @p ctx the platform's trusted roots.
 *
 * On Windows this reads the @c ROOT and @c CA system stores through CryptoAPI;
 * everywhere else it is @c SSL_CTX_set_default_verify_paths, which is the
 * correct answer on a platform that installs a CA bundle.
 *
 * @param ctx The context to populate. Must be non-null.
 * @return What was loaded and from where. **A caller enabling @c verify_peer
 *         must check @c certificates and say something if it is zero**: an
 *         empty store is not a handshake that fails safe in any useful sense,
 *         it is one that cannot succeed, and the resulting "certificate verify
 *         failed" names the venue rather than the real cause.
 *
 * @note Both Windows stores are read and failures on individual certificates
 *       are skipped rather than fatal. A store holds expired and malformed
 *       entries in normal operation, and one OpenSSL declines to parse is not a
 *       reason to abandon the rest.
 */
[[nodiscard]] trust_store_result load_platform_trust_store(SSL_CTX *ctx);

} // namespace exchange::transport::detail
