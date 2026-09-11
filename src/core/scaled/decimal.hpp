#pragma once
// Scaled integer out to decimal text - the last mile before a venue sees a
// number.
//
// The inverse of what `core/scaled/fixed_point.hpp` does on the way in,
// and deliberately not in that module: `market_data` depends on `venue`, so the
// edge cannot run the other way. It is a dozen lines and it is exact, which is
// the only property that matters here - a price rendered even slightly wrong is
// an order at a price nobody asked for.

#include "core_export.hpp"

#include <cstdint>
#include <string>

namespace exchange::core::scaled {

/**
 * @brief @p scaled rendered as decimal text with exactly @p scale digits after
 *        the point.
 *
 * @param scaled The value in @c 10^-scale units - what @c symbol_spec's
 *        @c price_to_scaled and @c quantity_to_scaled hand back.
 * @param scale Fractional digits. Zero renders an integer with no point.
 * @return The decimal, e.g. @c to_decimal(15345, 2) == @c "153.45".
 *
 * @note Never trims trailing zeros. @c to_decimal(100, 3) is @c "0.100", not
 *       @c "0.1" - both are the same number to the venue, and padding to the
 *       listing's own scale is the form that visibly matches the grid the
 *       venue published. Trimming would also make the output depend on the
 *       value rather than only on the scale, which is a worse thing to reason
 *       about at 3am.
 * @note Integer arithmetic throughout. No @c double appears anywhere on the
 *       order path, here least of all.
 */
[[nodiscard]] CORE_EXPORT std::string to_decimal(std::int64_t scaled,
												 int scale);

} // namespace exchange::core::scaled
