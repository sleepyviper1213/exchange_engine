#pragma once
// Commands and the records of a session, as text.
// Opt-in, like fmt's own fmt/std.h and fmt/ranges.h: only translation units
// that actually print one of these pay for <fmt/format.h>, so the domain
// headers stay free of it. Include this wherever you format one; a missing
// include is a compile error, never a silently different rendering.
//
// Every formatter here derives from fmt::nested_formatter<std::string_view>:
// each type renders as text, so standard fill/align/width apply to the whole
// record -
// `{:>32}` right-aligns one in a 32-column log field.
// @see https://fmt.dev/12.0/api/#formatting-user-defined-types

#include "command.hpp"
#include "engine_event.hpp"
#include "lifecycle/lifecycle.hpp"
#include "order_book/format.hpp" // engine_event prints a trade/outcome
#include "orders/format.hpp"     // a PLACE prints its order

#include <fmt/format.h>

#include <string_view>

/**
 * @brief A command as @c "cmd[PLACE sym=7 Order[...]]" - the tag, the listing,
 *        and whichever payload the tag says is live.
 *
 * The tag prints itself: @c command::Type is an @c EXCHANGE_ENUM_NAME enum like
 * every other one here, so @c format_as carries it and this formatter only has
 * to choose the payload. It used to be a hand-written switch on the belief that
 * the X-macro machinery could not reach an enum nested inside a class - it can,
 * and the generated free function at namespace scope takes @c command::Type
 * without complaint. One list, one spelling, and adding a command type no
 * longer means remembering to name it in a second place.
 *
 * @par What that gave up, and why it is the right trade
 * The switch printed @c "?" for a value outside the enum; @c to_string returns
 * empty, so a corrupt tag now renders as a gap rather than a question mark.
 * Worse in isolation, and right in context: *every* enum in this project
 * answers out-of-range with an empty view, and @c journal_record::decode reads
 * exactly that to refuse a record whose tag this build does not know. A
 * formatter that invented its own sentinel would be the one thing here that
 * disagreed.
 *
 * @par The coverage guarantee is stronger than it was, not weaker
 * Two separate things now, and both hold. The *tag* cannot drift at all: the
 * enum body is generated from @c COMMAND_TYPE_LIST, so an enumerator without a
 * name is not a thing that can be written. The *payload* switch below is still
 * a switch over the same enum, so adding a fifth command type leaves it with an
 * unhandled case - a warning, and a warning here is an error. What used to be
 * one hand-maintained switch guarding both is now a construction that cannot be
 * wrong about the tag and a compiler error if it is wrong about the payload.
 *
 * @note The union is read only through the accessors, and only on the arm the
 *       tag names - which is what makes this safe to write at all. @see command
 */
template <>
struct fmt::formatter<exchange::engine::event::command>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::engine::event::command &cmd,
				format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			out = fmt::format_to(out, "cmd[{} sym={} ", cmd.type, cmd.symbol);
			switch (cmd.type) {
				using enum exchange::engine::event::command::Type;
			case PLACE: out = fmt::format_to(out, "{}", cmd.as_place()); break;
			case CANCEL:
				out = fmt::format_to(out, "id={}", cmd.as_cancel());
				break;
			case ADD:
			case REDUCE: {
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
 * at_ns=1700000000000000000]".
 *
 * The three lifecycle formatters print the timestamp raw rather than as a date.
 * Rendering it needs a time zone and a calendar, and this header formats
 * records
 * - a value that means "1.7e18 nanoseconds after the UNIX epoch" prints as that
 * number, and whatever displays it to a human owns the locale question. It also
 * keeps a log line diffable against the bytes the journal actually holds, which
 * is the reason these records exist.
 *
 * @par Why the field carries its unit in its name
 * Because two timestamps reach one log line and they are not the same one. This
 * is the *record's* stamp - when the session opened, which for a recovery
 * record replayed out of a journal is an instant in an earlier run. The
 * structured envelope adds its own, @c ts_ms, for when the line was emitted.
 * While this field was spelled @c at= a line read
 * @c {"ts_ms":1700000000000,...,"msg":"startup[... at=1700000000000000000]"} -
 * the same instant twice, in two units, with nothing saying which was which,
 * and a parser had only the magnitude to go on. @c at_ns says it, in the
 * envelope's own idiom. @see core/logging/lifecycle.cpp for @c ts_ms
 */
template <>
struct fmt::formatter<exchange::engine::event::lifecycle::startup>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::engine::event::lifecycle::startup &startup,
				format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			return fmt::format_to(
				out,
				"startup[session={} {} at_ns={}]",
				startup.session,
				startup.mode,
				exchange::engine::event::lifecycle::epoch_nanos(
					startup.timestamp));
		});
	}
};

/// @brief A session closing, as @c "shutdown[session=7 CLEAN at_ns=… cmds=120
///        events=310]". The two counts always print, including at zero: a
///        session that applied nothing is news, not an omission.
template <>
struct fmt::formatter<exchange::engine::event::lifecycle::shutdown>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::engine::event::lifecycle::shutdown &shutdown,
				format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			return fmt::format_to(
				out,
				"shutdown[session={} {} at_ns={} cmds={} "
				"events={}]",
				shutdown.session,
				shutdown.reason,
				exchange::engine::event::lifecycle::epoch_nanos(
					shutdown.timestamp),
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

/// @brief A rebuild, as @c "recovery[session=8 from=7 SNAPSHOT|JOURNAL at_ns=…
///        replayed=95 orders=12]". The session it continues prints beside its
///        own, because a recovery record read without that edge names a history
///        it does not identify.
template <>
struct fmt::formatter<exchange::engine::event::lifecycle::recovery>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::engine::event::lifecycle::recovery &recovery,
				format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			return fmt::format_to(
				out,
				"recovery[session={} from={} {} at_ns={} "
				"replayed={} orders={}]",
				recovery.session,
				recovery.recovered_from,
				recovery.source,
				exchange::engine::event::lifecycle::epoch_nanos(
					recovery.timestamp),
				recovery.entries_replayed,
				recovery.orders_restored);
		});
	}
};
