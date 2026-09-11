#pragma once
// Why a Binance trade payload could not be read.
//
// Split from binance_trade.hpp for the reason depth_error.hpp is split from
// binance_depth.hpp: a caller that only reports the reason should not also
// compile the parser. The X-macro list generating the enumerator names and
// their messages is part of the enum and travels with it.
//
// --- why this is not depth_error -------------------------------------------
//
// All three of these are spelled identically in depth_error, and folding both
// into one "binance market-data parse failure" would be defensible - but only
// by renaming depth_error, which is named in twelve files including the
// enum-conversion and formatter tests that pin its labels. That is churn spent
// against the tree's own rule about not renaming opportunistically, and it buys
// one enum instead of two.
//
// The sets are not the same either, which is the substantive reason rather than
// the bookkeeping one: depth_error's `malformed_level` cannot occur on a tape,
// because a print has no levels. Sharing the enum would mean every trade parse
// carrying a category that is unreachable by construction, and a switch over it
// having a case no test can reach.

#include "core/util/enum_string.hpp"
#include "fwd.hpp"

#include <cstdint>

namespace exchange::market_data::binance {

#define TRADE_ERROR_LIST(X)                                                    \
	X(invalid_json, "invalid JSON")                                            \
	X(missing_field, "missing or mistyped field")                              \
	X(bad_number, "invalid number")

enum class trade_error : std::uint8_t {
	EXCHANGE_ENUM_VALUES(TRADE_ERROR_LIST)
};

/// @brief The category message for a @c trade_error (empty view if out of
///        range).
EXCHANGE_ENUM_LABEL(trade_error, message, TRADE_ERROR_LIST)

#undef TRADE_ERROR_LIST
} // namespace exchange::market_data::binance
