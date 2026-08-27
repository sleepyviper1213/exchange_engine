#pragma once

#include "order_manager.hpp"

#include <fmt/format.h>

#include <string_view>


/**
 * @brief One managed order, as
 *        @c "OrderRecord[id=42 sym=7 acct=99 ask @1250 4/30 PARTIALLY_FILLED]".
 *
 * The record store's answer to "what happened to order 42", so it leads with
 * the id and ends with the fate. In between is what identifies the order to a
 * human reading a log: its listing, its owner, and where it sat.
 *
 * @c traded/quantity rather than a remaining count, because the two numbers
 * together say how far through the order is and either alone does not - and
 * because that is the pairing @c OrderStatus is derived from, so a line whose
 * status looks wrong can be checked against the quantities on the same line.
 *
 * Fields that carry no information are omitted, on the same reasoning as
 * @c order's compact form: @c reason only when the order ended with one - a
 * cancel needs no excuse, so NONE is the common case and printing it is noise -
 * @c acct only when the order is attributed, and @c ts only when stamped.
 */
template <>
struct fmt::formatter<exchange::engine::execution::order_record>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::engine::execution::order_record &record,
				format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			out = fmt::format_to(out,
								 "OrderRecord[id={} sym={}",
								 record.id,
								 record.symbol);
			// Zero is "unattributed", which is what anonymous depth carries and
			// what every order carries until the gateway sets an account.
			if (record.account != 0)
				out = fmt::format_to(out, " acct={}", record.account);
			out = fmt::format_to(out,
								 " {} @{} {}/{} {}",
								 record.side, // format_as -> "bid" / "ask"
								 record.price,
								 record.state.traded(),
								 record.state.quantity(),
								 status(record)); // format_as -> enumerator
			if (record.reason != exchange::engine::reject_reason::NONE)
				out = fmt::format_to(out, " {}", record.reason);
			if (record.timestamp != 0)
				out = fmt::format_to(out, " ts={}", record.timestamp);
			return fmt::format_to(out, "]");
		});
	}
};


/**
 * @brief A handle, as @c "order_handle[slot=3 gen=1]" or @c
 * "order_handle[none]".
 *
 * Prints the generation, which is the whole reason the type is not a bare
 * index: two handles naming the same slot at different generations are
 * different orders, and a log that showed only the slot would render them
 * identically at exactly the moment the difference mattered.
 */
template <>
struct fmt::formatter<exchange::engine::execution::order_handle>
	: fmt::nested_formatter<std::string_view> {
	auto format(exchange::engine::execution::order_handle handle,
				format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			if (!is_valid(handle))
				return fmt::format_to(out, "order_handle[none]");
			return fmt::format_to(out,
								  "order_handle[slot={} gen={}]",
								  handle.slot,
								  handle.generation);
		});
	}
};


/**
 * @brief A record store's occupancy, as
 *        @c "order_manager[live=3 retained=120 peak=57/32768]".
 *
 * The capacity-planning line. @c peak against @c capacity is the reading that
 * matters - a store sized right reports a peak comfortably below its capacity,
 * and one that reached it has refused orders. @c live and @c retained say how
 * that capacity is being spent right now: orders the book can still act on,
 * versus history kept so a late cancel can be told what became of its order.
 *
 * @c evicted prints only when it is non-zero, and it is the one number here
 * that reports a loss: terminal records dropped to make room, each of which is
 * an order the store can no longer answer questions about. Zero is the healthy
 * case and the common one, so it stays off the line.
 *
 * @note Deliberately not the records themselves. The store holds up to its
 *       capacity of them and a formatter that walked all 32k would be a way to
 *       hang a log; print an @c order_record when you have one in hand.
 */
template <>
struct fmt::formatter<exchange::engine::execution::order_manager>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::engine::execution::order_manager &orders,
				format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			out = fmt::format_to(out,
								 "order_manager[live={} retained={} peak={}/{}",
								 orders.live(),
								 orders.retained(),
								 orders.high_water(),
								 orders.capacity());
			if (orders.evicted() != 0)
				out = fmt::format_to(out, " evicted={}", orders.evicted());
			return fmt::format_to(out, "]");
		});
	}
};
