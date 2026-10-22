#pragma once
// What a venue's account stream has to supply, so the pipeline that keeps it
// open - connect, subscribe, read, reconnect, report a gap - is written once
// rather than once per venue.
//
// A concept for the same reason `order_encoder` is one: the pipeline is a
// template over its venue, so each step is a direct call and a second venue is
// a second model rather than an edit to the pipeline. What crosses the seam is
// already venue-neutral - credentials, an environment, and the
// `execution_report` every venue's events are translated into.

#include "venue/credentials.hpp"
#include "venue/environment.hpp"
#include "venue/execution_report.hpp"

#include <concepts>
#include <cstdint>
#include <expected>
#include <string_view>

namespace exchange::venue {

/**
 * @brief A venue's account stream: where it is, how to ask it for our events,
 *        and how to read what it sends.
 *
 * @c A::endpoint returns something with @c host, @c port and @c target.
 * @c subscribe_request and @c parse_subscribe_reply return @c std::expected
 * over @c A::error_type, which @c A::describe turns into words.
 */
template <class A>
concept account_stream =
	requires(environment env, const credentials &creds, std::int64_t ms,
			 std::string_view text, int scale, typename A::error_type error) {
		{ A::endpoint(env).host };
		{ A::endpoint(env).port };
		{ A::endpoint(env).target };
		{ *A::subscribe_request(creds, ms, text) }
			-> std::convertible_to<std::string_view>;
		{ A::is_event(text) } -> std::convertible_to<bool>;
		{ *A::parse_subscribe_reply(text) } -> std::convertible_to<std::int64_t>;
		{ A::parse_report(text, scale, scale) }
			-> std::same_as<std::expected<execution_report, typename A::error_type>>;
		{ A::is_other_event(error) } -> std::convertible_to<bool>;
		{ A::describe(error) } -> std::convertible_to<std::string_view>;
	};

} // namespace exchange::venue
