#pragma once
// Commands and the records of a session, as text.
// Opt-in, like fmt's own fmt/std.h and fmt/ranges.h: only translation units that
// actually print one of these pay for <fmt/format.h>, so the domain headers stay
// free of it. Include this wherever you format one; a missing include is a
// compile error, never a silently different rendering.
//
// Every formatter here derives from fmt::nested_formatter<std::string_view>: each
// type renders as text, so standard fill/align/width apply to the whole record -
// `{:>32}` right-aligns one in a 32-column log field.
// @see https://fmt.dev/12.0/api/#formatting-user-defined-types
//
// One sidecar per module, which is what CLAUDE.md asks for: a cross-cutting
// facility is an opt-in header *inside* a module, never a central one, because a
// central one would point an edge back up the graph. These four used to be a
// single trading-engine/format.hpp, which was correct while the engine was a
// single library.

#include "command.hpp"
#include "engine_event.hpp"
#include "lifecycle/lifecycle.hpp"

#include "trading-engine/order_book/format.hpp" // engine_event prints a trade/outcome
#include "trading-engine/orders/format.hpp"     // a PLACE prints its order

#include <fmt/format.h>

#include <string_view>


/**
 * @brief A command as @c "cmd[PLACE sym=7 order[...]]" - the tag, the listing,
 *        and whichever payload the tag says is live.
 *
 * The tag is printed as a word by a switch here rather than by @c format_as,
 * because @c command::Type is nested inside @c command and the
 * @c EXCHANGE_ENUM_* machinery generates free functions that a nested enum
 * cannot reach. A switch costs the same and the compiler still enforces
 * coverage, which is the property that matters: adding a command type without
 * deciding how it prints is a warning, and a warning here is an error.
 *
 * @note The union is read only through the accessors, and only on the arm the
 *       tag names - which is what makes this safe to write at all. @see command
 */
template <>
struct fmt::formatter<exchange::engine::event::command>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::engine::event::command &cmd,
				format_context &ctx) const -> format_context::iterator {
		using Type = exchange::engine::event::command::Type;
		return write_padded(ctx, [&](auto out) {
			const std::string_view tag = [&] {
				switch (cmd.type) {
				case Type::PLACE: return "PLACE";
				case Type::CANCEL: return "CANCEL";
				case Type::ADD: return "ADD";
				case Type::REDUCE: return "REDUCE";
				}
				return "?";
			}();
			out = fmt::format_to(out, "cmd[{} sym={} ", tag, cmd.symbol);
			switch (cmd.type) {
			case Type::PLACE:
				out = fmt::format_to(out, "{}", cmd.as_place());
				break;
			case Type::CANCEL:
				out = fmt::format_to(out, "id={}", cmd.as_cancel());
				break;
			case Type::ADD:
			case Type::REDUCE: {
				const auto &level = cmd.as_level();
				out               = fmt::format_to(out,
												   "{} @{} x {}",
												   level.side,
												   level.price,
												   level.volume);
				break;
			}
			}
			return fmt::format_to(out, "]");
		});
	}
};


/**
 * @brief A published event as @c "event[sym=7 trade[aggressor=1 hit=2 @100 x
 * 4]]".
 *
 * The kind is not printed as a word: the payload's own rendering already begins
 * with @c "trade[" or @c "outcome[", so naming the tag as well would say it
 * twice. What the wrapper adds is the one thing neither payload carries and the
 * whole record exists for - the listing.
 */
template <>
struct fmt::formatter<exchange::engine::event::engine_event>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::engine::event::engine_event &event,
				format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			out = fmt::format_to(out, "event[sym={} ", event.symbol);
			switch (event.kind) {
			case exchange::engine::event::EventKind::TRADE:
				out = fmt::format_to(out, "{}", event.as_trade());
				break;
			case exchange::engine::event::EventKind::OUTCOME:
				out = fmt::format_to(out, "{}", event.as_outcome());
				break;
			}
			return fmt::format_to(out, "]");
		});
	}
};


/**
 * @brief A session opening, as @c "startup[session=7 COLD
 * at=1700000000000000000]".
 *
 * The three lifecycle formatters print the timestamp raw rather than as a date.
 * Rendering it needs a time zone and a calendar, and this header formats
 * records
 * - a value that means "1.7e18 nanoseconds after the UNIX epoch" prints as that
 * number, and whatever displays it to a human owns the locale question. It also
 * keeps a log line diffable against the bytes the journal actually holds, which
 * is the reason these records exist.
 */
template <>
struct fmt::formatter<exchange::engine::event::lifecycle::startup>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::engine::event::lifecycle::startup &startup,
				format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			return fmt::format_to(out,
								  "startup[session={} {} at={}]",
								  startup.session,
								  startup.mode,
								  startup.timestamp.time_since_epoch().count());
		});
	}
};


/// @brief A session closing, as @c "shutdown[session=7 CLEAN at=… cmds=120
///        events=310]". The two counts always print, including at zero: a
///        session that applied nothing is news, not an omission.
template <>
struct fmt::formatter<exchange::engine::event::lifecycle::shutdown>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::engine::event::lifecycle::shutdown &shutdown,
				format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			return fmt::format_to(out,
								  "shutdown[session={} {} at={} cmds={} "
								  "events={}]",
								  shutdown.session,
								  shutdown.reason,
								  shutdown.timestamp.time_since_epoch().count(),
								  shutdown.commands_applied,
								  shutdown.events_published);
		});
	}
};


/**
 * @brief A set of recovery sources, as @c "SNAPSHOT|JOURNAL".
 *
 * A set and not a bit, so it cannot reuse the enum's generated @c format_as -
 * that one answers for a single @c recovery_mode, and a set of two has no
 * single name. Pipe-separated in list order, so @c "SNAPSHOT|JOURNAL" reads the
 * way the recovery ran, and @c "none" for the empty set rather than an empty
 * field: an empty set is the malformed record @c is_well_formed
 * rejects, and a log line is exactly where you want to see it said out loud.
 */
template <>
struct fmt::formatter<exchange::engine::event::lifecycle::recovery_modes>
	: fmt::nested_formatter<std::string_view> {
	auto format(exchange::engine::event::lifecycle::recovery_modes sources,
				format_context &ctx) const -> format_context::iterator {
		using exchange::engine::event::lifecycle::recovery_mode;
		return write_padded(ctx, [&](auto out) {
			if (sources.is_empty()) return fmt::format_to(out, "none");
			bool written = false;
			for (const recovery_mode bit :
				 {recovery_mode::SNAPSHOT, recovery_mode::JOURNAL}) {
				if (!sources.test(bit)) continue;
				out     = fmt::format_to(out, "{}{}", written ? "|" : "", bit);
				written = true;
			}
			return out;
		});
	}
};


/// @brief A rebuild, as @c "recovery[session=8 from=7 SNAPSHOT|JOURNAL at=…
///        replayed=95 orders=12]". The session it continues prints beside its
///        own, because a recovery record read without that edge names a history
///        it does not identify.
template <>
struct fmt::formatter<exchange::engine::event::lifecycle::recovery>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::engine::event::lifecycle::recovery &recovery,
				format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			return fmt::format_to(out,
								  "recovery[session={} from={} {} at={} "
								  "replayed={} orders={}]",
								  recovery.session,
								  recovery.recovered_from,
								  recovery.source,
								  recovery.timestamp.time_since_epoch().count(),
								  recovery.entries_replayed,
								  recovery.orders_restored);
		});
	}
};
