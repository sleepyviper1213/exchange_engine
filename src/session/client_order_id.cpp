#include "client_order_id.hpp"

#include <charconv>
#include <system_error>

namespace exchange::session {
namespace {

/// One unsigned decimal filling the whole of @p text, or nothing.
template <class Unsigned>
[[nodiscard]] std::optional<Unsigned>
whole_number(std::string_view text) noexcept {
	if (text.empty()) return std::nullopt;
	// from_chars on an unsigned type already refuses a sign, which is the
	// behaviour wanted: "ex--1" is not one of ours.
	Unsigned value        = 0;
	const char *const end = text.data() + text.size();
	const auto [stop, ec] = std::from_chars(text.data(), end, value);
	if (ec != std::errc{} || stop != end) return std::nullopt;
	return value;
}

struct decoded_id {
	order_id_t id   = 0;
	venue_leg_t leg = 0;
};

[[nodiscard]] std::optional<decoded_id> decode(std::string_view text) noexcept {
	if (!text.starts_with(CLIENT_ORDER_PREFIX)) return std::nullopt;
	text.remove_prefix(CLIENT_ORDER_PREFIX.size());
	// A cancel names the order it cancels, so it decodes to the same id. Not
	// stripping this would make `is_ours` false for our own cancel requests,
	// and reconciliation would then classify them as somebody else's orders.
	if (text.ends_with(CANCEL_SUFFIX)) text.remove_suffix(CANCEL_SUFFIX.size());

	venue_leg_t leg = 0;
	if (const auto split = text.find(LEG_SEPARATOR);
		split != std::string_view::npos) {
		// "ex-42_" and "ex-42_0" are refused: leg zero is spelled without a
		// suffix, so either would be a second spelling of one venue order.
		const auto parsed = whole_number<venue_leg_t>(text.substr(split + 1));
		if (!parsed || *parsed == 0) return std::nullopt;
		leg  = *parsed;
		text = text.substr(0, split);
	}

	const auto id = whole_number<order_id_t>(text);
	if (!id) return std::nullopt;
	return decoded_id{.id = *id, .leg = leg};
}

} // namespace

std::string client_order_id(order_id_t id, venue_leg_t leg) {
	std::string text = std::string(CLIENT_ORDER_PREFIX) + std::to_string(id);
	if (leg != 0) {
		text += LEG_SEPARATOR;
		text += std::to_string(leg);
	}
	return text;
}

std::optional<order_id_t> engine_order_id(std::string_view text) noexcept {
	const auto decoded = decode(text);
	if (!decoded) return std::nullopt;
	return decoded->id;
}

std::optional<venue_leg_t> venue_leg(std::string_view text) noexcept {
	const auto decoded = decode(text);
	if (!decoded) return std::nullopt;
	return decoded->leg;
}

bool is_ours(std::string_view text) noexcept {
	return engine_order_id(text).has_value();
}

std::string cancel_order_id(order_id_t id) {
	return client_order_id(id) + std::string(CANCEL_SUFFIX);
}

bool is_cancel_id(std::string_view text) noexcept {
	return text.starts_with(CLIENT_ORDER_PREFIX) &&
		   text.ends_with(CANCEL_SUFFIX);
}


} // namespace exchange::session
