#include "venue/credentials.hpp"

#include <cstddef>
#include <string>

namespace exchange::venue {

std::string credentials::fingerprint() const {
	/// How many leading characters of a key a log line may show. Enough to tell
	/// two configured keys apart, far short of enough to use one.
	constexpr std::size_t FINGERPRINT_CHARS = 4;

	if (key.empty()) return "<unset>";
	if (key.size() <= FINGERPRINT_CHARS) return "<too short to be a key>";
	return key.substr(0, FINGERPRINT_CHARS) + "...";
}

} // namespace exchange::venue
