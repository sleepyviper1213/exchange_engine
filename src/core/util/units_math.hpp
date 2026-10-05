#pragma once

#include <mp-units/framework.h>

#include <concepts>
#include <numeric>

namespace exchange::core::util {
template <auto R1, auto Origin, typename Rep1, auto R2, typename Rep2>
	requires requires(Rep1 a, Rep2 b) {
		mp_units::get_common_reference(R1, R2);
		requires requires { std::midpoint(a, b); };
	}
[[nodiscard]] constexpr mp_units::QuantityPointOf<
	get_quantity_spec(get_common_reference(R1, R2))> auto
midpoint(const mp_units::quantity_point<R1, Origin, Rep1> &a,
		 const mp_units::quantity_point<R2, Origin, Rep2> &b) noexcept {
	constexpr auto ref  = mp_units::get_common_reference(R1, R2);
	constexpr auto unit = mp_units::get_unit(ref);

	return Origin +
		   mp_units::quantity{
			   std::midpoint(
				   a.quantity_ref_from(Origin).numerical_value_in(unit),
				   b.quantity_ref_from(Origin).numerical_value_in(unit)),
			   ref};
}

/**
 * @brief @p q clamped to [@p low, @p high] and converted to @c To.
 *
 * @c To is the result type, and by default the bounds are its whole range, so
 * @c clamp<quantity_t>(volume) is the saturating narrowing: a value that does
 * not fit is cut to the nearest one that does rather than wrapped. Given
 * explicit bounds, @c To is deduced from them.
 *
 * The comparison happens before the conversion, in @p q's own representation,
 * which is the only order that cannot wrap. The conversion is then the explicit
 * one - a @c quantity_cast to @c To's spec and a @c value_cast to its rep -
 * because mp-units does not guard narrowing, so an implicit one would compile
 * and truncate.
 */
template <mp_units::Quantity To, auto R, typename Rep>
	requires std::totally_ordered_with<mp_units::quantity<R, Rep>, To>
[[nodiscard]] constexpr To clamp(const mp_units::quantity<R, Rep> &q,
								 const To &low  = To::min(),
								 const To &high = To::max()) noexcept {
	if (q < low) return low;
	if (q > high) return high;
	return mp_units::value_cast<typename To::rep>(
		mp_units::quantity_cast<To::quantity_spec>(q).in(To::unit));
}
} // namespace exchange::core::util