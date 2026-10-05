#pragma once
// Prices and sizes, printed as the bare number they always printed as.
//
// A partial specialisation rather than `format_as`: fmt looks `format_as` up
// by ADL, and a quantity's unit is a non-type template argument, which brings
// no namespace into the lookup. Every spec the representation's own formatter
// accepts - width, fill, `L` - passes through, so `{:>8}` on a price is what it
// was on a `uint32_t`. The unit is deliberately not printed: the log lines and
// the CLI output that predate the strong types stay byte-identical.

#include "units.hpp"

#include <fmt/format.h>

template <auto R, typename Rep>
struct fmt::formatter<mp_units::quantity<R, Rep>> : fmt::formatter<Rep> {
	auto format(const mp_units::quantity<R, Rep> &q, format_context &ctx) const
		-> format_context::iterator {
		return fmt::formatter<Rep>::format(q.numerical_value_ref_in(q.unit),
										   ctx);
	}
};

template <auto R, auto PO, typename Rep>
struct fmt::formatter<mp_units::quantity_point<R, PO, Rep>>
	: fmt::formatter<Rep> {
	auto format(const mp_units::quantity_point<R, PO, Rep> &p,
				format_context &ctx) const -> format_context::iterator {
		const auto &from_origin = p.quantity_ref_from(PO);
		return fmt::formatter<Rep>::format(
			from_origin.numerical_value_ref_in(from_origin.unit), ctx);
	}
};
