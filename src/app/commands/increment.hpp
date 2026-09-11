#pragma once

// A tick or lot size as it arrives on the command line: decimal text plus the
// symbol's scale. Shared because `backtest` and `serve` both take one and both
// have to fail the same way - a bad increment is a usage error, so it is logged
// and refused rather than thrown across a command boundary.

#include "core/logging.hpp"             // IWYU pragma: keep - spdlog::error
#include "order_book/reject_reason.hpp" // describe
#include "symbol/symbol_spec.hpp"       // parse_exact_decimal
#include <spdlog/spdlog.h>

#include <cstdint>
#include <optional>
#include <string_view>

namespace exchange::app {

/// @brief Parse a tick or lot size from decimal text onto @p scale.
/// @return The scaled increment, or nothing - the reason is logged.
[[nodiscard]] inline std::optional<std::int64_t>
increment(std::string_view text, int scale, std::string_view what) {
	const auto scaled = engine::parse_exact_decimal(text, scale);
	if (!scaled) {
		spdlog::error("--{} '{}': {}",
					  what,
					  text,
					  engine::describe(scaled.error()));
		return std::nullopt;
	}
	if (*scaled <= 0) {
		spdlog::error("--{} must be positive (got '{}')", what, text);
		return std::nullopt;
	}
	return *scaled;
}

} // namespace exchange::app
