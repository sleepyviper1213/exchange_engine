#pragma once
// fmt formatters for the risk module's composite value types.
//
// An opt-in sidecar, like `market-data/format.hpp` and
// `trading-engine/format.hpp` and for the same reason: printing is a
// cross-cutting concern, and a module that needed a central one would have an
// edge pointing back up the dependency graph. It is deliberately *not* in
// `risk_management.hpp` - a translation unit that screens orders has no reason
// to compile fmt's machinery to do it.
//
// What is not here: the enums. `breach`, `trading_state` and `trip_cause` each
// get `format_as` for free from the `EXCHANGE_ENUM_*` X-macros in their own
// headers, and giving one of them a `fmt::formatter` as well would be two
// answers to one question. This file is for the types built *out* of those -
// today that is the breach set, which is a mask rather than an enumerator.

#include "risk_management/hooks/breach.hpp"

#include <fmt/format.h>

#include <string_view>

/**
 * @brief A set of broken rules as @c "PRICE_BAND|POSITION_LIMIT", or @c "none".
 *
 * @par Why this is not a loop at every call site
 * Because it was, twice - once in the log line that names a refused command and
 * once in the report that counts them - and each copy carried its own list of
 * rule names to test against. A mask is a composite value, so how it reads is
 * the module's business and not its callers'; @c ALL_BREACHES is the list, and
 * it is generated from the same X-macro the enum is.
 *
 * @par Worst-first, matching what a client is told
 * The rules print in @c RISK_BREACH_LIST's order, which is the order that
 * decides
 * @c first_reason - so the leftmost name in this string is the one reason the
 * client received, and everything after it is what the log knows and the client
 * does not. That correspondence is the point of printing the whole set.
 *
 * @note @c none rather than an empty string, because a formatter that can
 * render nothing produces a log line with a hole in it - "refused cmd[...] - "
 *       reads as a truncated message rather than as a fact.
 */
template <>
struct fmt::formatter<exchange::risk::hooks::breach_set>
	: fmt::nested_formatter<std::string_view> {
	auto format(exchange::risk::hooks::breach_set reasons,
				format_context &ctx) const -> format_context::iterator {
		namespace hooks = exchange::risk::hooks;
		return write_padded(ctx, [&](auto out) {
			if (reasons.is_empty()) return fmt::format_to(out, "none");

			bool first = true;
			for (const hooks::breach rule : hooks::ALL_BREACHES) {
				// NONE is the "no rule" enumerator and is a member of every
				// mask under a `test` that compares zero bits, so it is skipped
				// explicitly rather than filtered by the set. @see ALL_BREACHES
				if (rule == hooks::breach::NONE) continue;
				if (!reasons.test(rule)) continue;
				// The separator is written on its own rather than chosen with
				// a ternary over two format strings: fmt requires a
				// compile-time format string, and a runtime choice between two
				// literals is not one.
				if (!first) out = fmt::format_to(out, "|");
				out   = fmt::format_to(out, "{}", rule);
				first = false;
			}
			return out;
		});
	}
};
