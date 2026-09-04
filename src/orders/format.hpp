#pragma once

#include "order.hpp"

#include <fmt/format.h>

#include <string_view>


/**
 * @brief An Order, in one of two renderings.
 *
 * | spec        | rendering |
 * | ----------- | --------- |
 * | @c "{}", @c "{:c}" | compact - @c "Order[id=1 bid 100 x 10 LIMIT
 * GOOD_TILL_CANCELLED]" | | @c "{:v}"          | verbose - every field, named,
 * including the empty ones |
 *
 * @par Why compact is the default
 * An order is printed by the line, not by the page: it appears in every ack,
 * every reject and every trace, and a field that reads @c "stop_price=0" on
 * every one of them is noise that makes the fields that *did* change harder to
 * see. So the compact form omits what carries no information - @c stop_price
 * when the order is not a stop, @c timestamp when it was never stamped - and
 * keeps what always does.
 *
 * Both instruction fields always print, even at their defaults. They are the
 * two halves of what the book is about to do with the order, and a LIMIT that
 * rests and a LIMIT that is withdrawn unfilled differ only in the second word.
 *
 * @par Why verbose exists anyway
 * Omitting a field and a field being zero are indistinguishable in the compact
 * form, which is exactly wrong when the question is "what did we actually
 * receive?". @c "{:v}" answers that: fixed shape, named fields, nothing hidden,
 * so two dumps can be diffed against each other.
 *
 * @par Combining the mode with fill, align and width
 * The mode letter comes first, and everything after it is an ordinary spec:
 * @c "{:c >60}" is compact, space-padded, right-aligned in 60.
 *
 * The space is not optional there, and the reason is fmt's grammar rather than
 * this formatter: a leading character followed by @c < @c > or @c ^ is a *fill*
 * character, so @c "{:c>60}" already means "pad with @c c" and still does. A
 * mode letter is only a mode when no alignment follows it. Writing the fill
 * explicitly disambiguates, which is the same thing you would do to right-align
 * anything whose fill happens to look like a type.
 */
template <>
struct fmt::formatter<exchange::engine::orders::order>
	: fmt::nested_formatter<std::string_view> {
	bool verbose = false;

	constexpr auto parse(format_parse_context &ctx)
		-> format_parse_context::iterator {
		const auto *it        = ctx.begin();
		const auto *const end = ctx.end();
		// The mode letter leads. It cannot trail: fmt's grammar puts the
		// presentation type last, so it rejects an unknown letter there at
		// compile time before this ever sees it. Leading is only ambiguous with
		// a fill character, and a fill is a fill only when an alignment follows
		// - the same rule fmt itself uses to tell the two apart. So "{:v}" is
		// verbose while "{:v<10}" still means "pad with v", exactly as it did.
		if (it != end && (*it == 'v' || *it == 'c')) {
			const auto *const next = it + 1;
			const bool is_fill =
				next != end && (*next == '<' || *next == '>' || *next == '^');
			if (!is_fill) {
				verbose = *it == 'v';
				ctx.advance_to(++it);
			}
		}
		// Whatever is left is an ordinary spec, so fill/align/width behave here
		// exactly as they do for every other record in this header.
		return fmt::nested_formatter<std::string_view>::parse(ctx);
	}

	auto format(const exchange::engine::orders::order &order,
				format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			if (verbose)
				return fmt::format_to(
					out,
					"Order[id={} side={} price={} stop_price={}"
					" qty={} type={} tif={} timestamp={}]",
					order.id,
					order.side, // format_as -> "bid" / "ask"
					order.price,
					order.stop_price,
					order.qty,
					order.type, // format_as -> enumerator name
					order.tif,
					order.timestamp);

			out = fmt::format_to(out,
								 "Order[id={} {} {}",
								 order.id,
								 order.side,
								 order.price);
			// Zero is the "not a stop" sentinel, so printing it would be
			// printing the absence of a trigger.
			if (order.stop_price != 0)
				out = fmt::format_to(out, " stop={}", order.stop_price);
			out = fmt::format_to(out,
								 " x {} {} {}",
								 order.qty,
								 order.type,
								 order.tif);
			if (order.timestamp != 0)
				out = fmt::format_to(out, " ts={}", order.timestamp);
			return fmt::format_to(out, "]");
		});
	}
};
