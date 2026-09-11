#include "api_error.hpp"

#include <fmt/format.h>

#include <simdjson.h>
#include <string>

namespace exchange::venue::binance {

std::optional<api_error> parse_api_error(std::string_view body) noexcept try {
	if (body.empty()) return std::nullopt;

	simdjson::dom::parser parser;
	// padded_string copies, which is what makes this safe on a view the caller
	// owns; an error body is a few dozen bytes and this is not a hot path.
	simdjson::dom::element doc;
	if (parser.parse(simdjson::padded_string(body)).get(doc))
		return std::nullopt;

	std::int64_t code = 0;
	if (doc["code"].get_int64().get(code)) return std::nullopt;

	std::string_view msg;
	// A code with no message is still an error envelope - the code is the part
	// that identifies it - so an absent `msg` is tolerated rather than fatal.
	if (doc["msg"].get_string().get(msg)) msg = {};

	return api_error{.code = static_cast<int>(code), .msg = std::string(msg)};
} catch (...) {
	// simdjson's DOM parser allocates, so the only thing that can throw here is
	// exhaustion. A body we could not decode is reported as "not an envelope",
	// which is exactly what the caller's fallback path is for.
	return std::nullopt;
}

std::string describe_api_error(std::string_view body,
							   std::string_view fallback) {
	const std::optional<api_error> parsed = parse_api_error(body);
	if (!parsed) return std::string(fallback);
	if (parsed->msg.empty()) return fmt::format("venue error {}", parsed->code);
	return fmt::format("{} (code {})", parsed->msg, parsed->code);
}

} // namespace exchange::venue::binance
