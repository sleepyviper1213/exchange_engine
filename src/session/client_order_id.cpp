#include "client_order_id.hpp"

#include <charconv>
#include <system_error>

namespace exchange::session {
std::string client_order_id(order_id_t id) {
	return std::string(CLIENT_ORDER_PREFIX) + std::to_string(id);
}

std::optional<order_id_t> engine_order_id(std::string_view text) noexcept {
	if (!text.starts_with(CLIENT_ORDER_PREFIX)) return std::nullopt;
	text.remove_prefix(CLIENT_ORDER_PREFIX.size());
	// A cancel names the order it cancels, so it decodes to the same id. Not
	// stripping this would make `is_ours` false for our own cancel requests,
	// and reconciliation would then classify them as somebody else's orders.
	if (text.ends_with(CANCEL_SUFFIX)) text.remove_suffix(CANCEL_SUFFIX.size());
	if (text.empty()) return std::nullopt;
	// from_chars on an unsigned type already refuses a sign, which is the
	// behaviour wanted: "ex--1" is not one of ours.
	order_id_t id         = 0;
	const char *const end = text.data() + text.size();
	const auto [stop, ec] = std::from_chars(text.data(), end, id);
	if (ec != std::errc{} || stop != end) return std::nullopt;
	return id;
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