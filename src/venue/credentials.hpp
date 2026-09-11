#pragma once
// An API key and secret, as a value - and nothing about where they came from.
//
// Reading configuration is `app/`'s job, and this module deliberately does not
// do it: `venue/` is protocol knowledge, and "which environment variable holds
// the secret" is a deployment convention rather than anything Binance defines.
// The composition root reads it and hands the result down. @see
// app/credentials_option.hpp

#include "venue_export.hpp" // VENUE_EXPORT (generated)

#include <string>
#include <string_view>

namespace exchange::venue {

/**
 * @brief A venue API credential.
 *
 * @warning Neither field has a formatter and neither should ever get one. The
 *          secret is the signing key: printed once into a log that is shipped
 *          somewhere, it is compromised and the compromise is silent. Use
 *          @c fingerprint when a run needs to say *which* key it is using.
 */
struct credentials {
	/// @brief The public identifier, sent as the @c X-MBX-APIKEY header.
	std::string key{};

	/// @brief The HMAC signing key. Never transmitted, never logged.
	std::string secret{};

	/// @brief Whether both halves are present. A credential missing either is
	///        not a weaker credential, it is no credential.
	[[nodiscard]] bool is_complete() const noexcept {
		return !key.empty() && !secret.empty();
	}

	/**
	 * @brief The first few characters of @c key, for a log line.
	 *
	 * Enough to tell two configured keys apart when a run authenticates as the
	 * wrong account, which is the only question a log has to answer about a
	 * credential. Never touches @c secret.
	 */
	[[nodiscard]] VENUE_EXPORT std::string fingerprint() const;
};

/**
 * @brief The environment variable conventionally holding the API key.
 *
 * Named here rather than in the CLI because the name is part of this type's
 * contract with whoever deploys the process - the reader is elsewhere, but
 * there should be exactly one spelling of what it reads.
 */
inline constexpr std::string_view API_KEY_VAR = "BINANCE_API_KEY";

/// @brief The environment variable holding the API secret. @see API_KEY_VAR
inline constexpr std::string_view API_SECRET_VAR = "BINANCE_API_SECRET";

} // namespace exchange::venue
