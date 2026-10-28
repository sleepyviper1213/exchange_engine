#pragma once
// Binance spot's WebSocket API as a venue::account_stream: the endpoint, the
// HMAC-signed subscribe request, and the execution-report decoder. The wire
// format only - the pipeline that keeps the socket open is the session's.

#include "venue/account_stream.hpp"
#include "venue/binance/user_data.hpp"

#include <magic_enum/magic_enum.hpp>

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace exchange::venue::binance {

struct account_stream {
	using error_type = user_data_error;

	[[nodiscard]] static stream_endpoint endpoint(environment env) {
		return ws_api_endpoint(env);
	}

	[[nodiscard]] static std::expected<std::string, error_type>
	subscribe_request(const credentials &creds, std::int64_t timestamp_ms,
					  std::string_view request_id) {
		return binance::subscribe_request(creds, timestamp_ms, request_id);
	}

	/// An event push, as against the reply to a request on the same socket.
	[[nodiscard]] static bool is_event(std::string_view frame) {
		return is_user_data_event(frame);
	}

	[[nodiscard]] static std::expected<std::int64_t, error_type>
	parse_subscribe_reply(std::string_view frame) {
		return binance::parse_subscribe_reply(frame);
	}

	/// One level down first: the WebSocket API wraps a push as
	/// {"subscriptionId":N,"event":{...}}, and the unwrap is permissive so a
	/// bare event from the old listen-key stream still reads. @see unwrap_event
	[[nodiscard]] static std::expected<execution_report, error_type>
	parse_report(std::string_view frame, int price_decimals, int qty_decimals) {
		return parse_execution_report(unwrap_event(frame),
									  price_decimals,
									  qty_decimals);
	}

	/// A valid event that is not an execution report - a balance update.
	[[nodiscard]] static bool is_other_event(error_type error) noexcept {
		return error == user_data_error::not_an_execution_report;
	}

	[[nodiscard]] static std::string_view describe(error_type error) noexcept {
		return magic_enum::enum_name(error);
	}
};

static_assert(venue::account_stream<account_stream>);

} // namespace exchange::venue::binance
