#pragma once
// The uniformity claim EXCHANGE_ENUM_NAME / EXCHANGE_ENUM_LABEL make: every
// enum declared through them gets the same fmt hook from the same macro, so
// these hold for all of them by construction rather than by hand-written copies
// agreeing with each other.

#include "core/util/enum_string.hpp"
#include "market-data/binance/binance_depth.hpp"
#include "market-data/binance/endpoints.hpp"
#include "market-data/parser/fixed_point.hpp"
#include "trading-engine/orders/order_type.hpp"
#include "trading-engine/orders/time_in_force_instruction.hpp"
#include "trading-engine/orders/types.hpp"

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>


using exchange::side_t;
using exchange::core::util::formattable_enum;
using exchange::engine::orders::order_type;
using exchange::engine::orders::time_in_force_instruction;
using exchange::market_data::parser::parse_error;
namespace binance = exchange::market_data::binance;

static_assert(formattable_enum<side_t>);
static_assert(formattable_enum<order_type>);
static_assert(formattable_enum<time_in_force_instruction>);
static_assert(formattable_enum<binance::depth_error>);
static_assert(formattable_enum<binance::depth_speed>);
static_assert(formattable_enum<parse_error>);

// The concept must actually discriminate — an enum with no hook must not match,
// or the assertions above prove nothing.
enum class unhooked_enum : std::uint8_t { a, b };
static_assert(!formattable_enum<unhooked_enum>);
static_assert(!formattable_enum<int>);

/// @brief Round-trip one project enum through every uniform conversion.
template <formattable_enum E>
void expect_uniform(E value, std::string_view expected) {
	// The accessor is the view, fmt::to_string is the owned copy, and "{}" is
	// the in-place render — three spellings, one text.
	EXPECT_EQ(format_as(value), expected);
	EXPECT_EQ(fmt::to_string(value), expected);
	EXPECT_EQ(fmt::format("{}", value), expected);
	// std::string conversion goes through fmt, not a per-enum helper.
	static_assert(std::is_same_v<decltype(fmt::to_string(value)), std::string>);
}

