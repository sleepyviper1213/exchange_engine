/// @file
/// @brief AFL++ harness for the Binance depth parsers.
///
/// Feeds the raw fuzzer bytes straight into every parser entry point. We are
/// hunting for crashes, out-of-bounds reads and sanitizer trips inside the
/// parser; a returned @c std::unexpected error is a valid, non-crashing outcome
/// and is intentionally discarded.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "afl_harness.hpp"
#include "io/binance_depth.hpp"

namespace {

// SOLUSDT-like precision; the exact values do not matter to the parser's memory
// safety, which is what we fuzz.
inline constexpr int kPriceDecimals = 2;
inline constexpr int kQtyDecimals = 2;

void run(const std::uint8_t *data, std::size_t size) {
    const std::string_view json{reinterpret_cast<const char *>(data), size};

    (void)binance::parse_binance_depth(json, kPriceDecimals, kQtyDecimals);
    (void)binance::parse_binance_depth_update(json, kPriceDecimals, kQtyDecimals);
    (void)binance::parse_binance_depth_updates(json, kPriceDecimals, kQtyDecimals);
}

} // namespace

FUZZ_MAIN(run)
