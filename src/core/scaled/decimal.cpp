#include "decimal.hpp"

#include <fmt/format.h>

namespace exchange::core::scaled {

std::string to_decimal(std::int64_t scaled, int scale) {
	if (scale <= 0) return fmt::format("{}", scaled);

	// Split on the absolute value and put the sign back by hand: integer
	// division truncates towards zero in C++, so -5 / 100 is 0 and -5 % 100 is
	// -5, which would render as "0.-05" if fed straight to the formatter.
	const bool negative      = scaled < 0;
	const std::uint64_t bare = negative
								   ? 0U - static_cast<std::uint64_t>(scaled)
								   : static_cast<std::uint64_t>(scaled);

	std::uint64_t divisor = 1;
	for (int i = 0; i < scale; ++i) divisor *= 10U;

	return fmt::format("{}{}.{:0{}}",
					   negative ? "-" : "",
					   bare / divisor,
					   bare % divisor,
					   scale);
}

} // namespace exchange::core::scaled
