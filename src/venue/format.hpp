#pragma once
// fmt formatters for the venue module's composite value types.
//
// An opt-in sidecar, like market_data/format.hpp and event/format.hpp: nothing
// in the module needs fmt to be useful, so a caller that only builds endpoints
// does not pay for the formatting machinery. The enums are not here - they get
// format_as for free from the EXCHANGE_ENUM_* macros, and a type must never
// have both.

#include "venue/endpoint.hpp"

#include <fmt/format.h>

#include <string_view>

/// @brief A WebSocket endpoint as the @c wss:// URL it denotes - paste-able
///        straight into a client when a capture misbehaves.
template <>
struct fmt::formatter<exchange::venue::stream_endpoint>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::venue::stream_endpoint &endpoint,
				format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			return fmt::format_to(out,
								  "wss://{}:{}{}",
								  endpoint.host,
								  endpoint.port,
								  endpoint.target);
		});
	}
};

/// @brief A REST endpoint as the @c https:// URL it denotes.
///
/// @warning Prints the target verbatim, query string included. A signed request
///          carries its API signature there, so this is for endpoints as built,
///          never for one that has been signed.
template <>
struct fmt::formatter<exchange::venue::http_endpoint>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::venue::http_endpoint &endpoint,
				format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			return fmt::format_to(out,
								  "https://{}{}",
								  endpoint.host,
								  endpoint.target);
		});
	}
};
