#include "session/venue_fill_book.hpp"

#include "venue/binance/order.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>

// What the venue says traded, booked once and valued on the listing's grid.
//
// Every refusal here has a counter in live_session_report behind it, and each
// is a way the position used to drift: a fill delivered twice, a recovered
// trade attributed to the wrong order, a fill booked on a guessed side.

using namespace exchange;
using namespace exchange::session;
using exchange::engine::symbol_spec;
using exchange::venue::execution_kind;
using exchange::venue::execution_report;

namespace {

/// SOLUSDT's real grid: tick 0.01 at scale 2, lot 0.001 at scale 3. One
/// tick-lot is therefore 0.00001 USDT - 1000 at the commission's 1e-8 scale.
[[nodiscard]] symbol_spec fill_book_listing() {
	return symbol_spec{/*id=*/7,
					   /*symbol=*/"SOLUSDT",
					   /*price_scale=*/2,
					   /*qty_scale=*/3,
					   /*tick_scaled=*/1 * units::scaled_price,
					   /*lot_scaled=*/1 * units::scaled_size,
					   /*reference_scaled=*/at_scaled(15345),
					   /*collar_bps=*/symbol_spec::NO_COLLAR};
}

/// One fill of 0.002 SOL at 153.45, ours on the bid, resting.
[[nodiscard]] execution_report fill_book_report() {
	execution_report report;
	report.kind              = execution_kind::trade;
	report.side              = side_t::bid;
	report.is_maker          = true;
	report.last_qty_scaled   = 2;     // 0.002
	report.last_price_scaled = 15345; // 153.45
	report.transaction_time_ms = 5;
	return report;
}

} // namespace

TEST(VenueFillBook, ATradeIdIsBookedOnce) {
	venue_fill_book book;
	EXPECT_FALSE(book.is_repeated_trade(9001));
	EXPECT_TRUE(book.is_repeated_trade(9001));
	EXPECT_EQ(book.newest_trade_id(), 9001);
}

TEST(VenueFillBook, AReportNamingNoTradeIsNeverARepeat) {
	venue_fill_book book;
	EXPECT_FALSE(book.is_repeated_trade(-1));
	EXPECT_FALSE(book.is_repeated_trade(-1));
	EXPECT_EQ(book.newest_trade_id(), -1);
}

TEST(VenueFillBook, AVenueOrderIsAttributedToTheOrderItWasLearnedFor) {
	venue_fill_book book;
	book.remember(777, 42);
	book.remember(777, 43); // the first answer stands
	EXPECT_EQ(book.engine_id_of(777), 42U);
	EXPECT_EQ(book.engine_id_of(555), 0U);
}

TEST(VenueFillBook, AFillIsValuedOnTheListingsGrid) {
	const auto fill = venue_fill_book::value_fill(fill_book_report(),
												  fill_book_listing(),
												  std::nullopt);
	ASSERT_TRUE(fill.has_value());
	EXPECT_EQ(fill->side, side_t::bid);
	EXPECT_EQ(fill->price, at_tick(15345));
	EXPECT_EQ(fill->lots, 2 * units::lot);
	EXPECT_EQ(fill->at, 5'000'000U);
	EXPECT_EQ(fill->aggressor_side, side_t::ask)
		<< "we rested, so the print's direction is the other side's";
}

TEST(VenueFillBook, TheVenuesSideBeatsTheQuotersView) {
	const auto fill = venue_fill_book::value_fill(fill_book_report(),
												  fill_book_listing(),
												  side_t::ask);
	ASSERT_TRUE(fill.has_value());
	EXPECT_EQ(fill->side, side_t::bid);
}

TEST(VenueFillBook, TheQuotersViewIsOnlyAFallback) {
	execution_report report = fill_book_report();
	report.side.reset();
	const auto fill = venue_fill_book::value_fill(report,
												  fill_book_listing(),
												  side_t::ask);
	ASSERT_TRUE(fill.has_value());
	EXPECT_EQ(fill->side, side_t::ask);
}

TEST(VenueFillBook, AFillOnNoKnownSideIsRefusedNotGuessed) {
	execution_report report = fill_book_report();
	report.side.reset();
	const auto fill = venue_fill_book::value_fill(report,
												  fill_book_listing(),
												  std::nullopt);
	ASSERT_FALSE(fill.has_value());
	EXPECT_EQ(fill.error(), unbooked_fill::SIDE_UNKNOWN);
}

TEST(VenueFillBook, AFillWithNoQuantityIsRefused) {
	execution_report report = fill_book_report();
	report.last_qty_scaled  = 0;
	const auto fill = venue_fill_book::value_fill(report,
												  fill_book_listing(),
												  std::nullopt);
	ASSERT_FALSE(fill.has_value());
	EXPECT_EQ(fill.error(), unbooked_fill::NO_QUANTITY);
}

TEST(VenueFillBook, NoCommissionChargesNothing) {
	const auto fee = venue_fill_book::value_fee(fill_book_report(),
												fill_book_listing(),
												"USDT",
												"SOL");
	ASSERT_TRUE(fee.has_value());
	EXPECT_EQ(fee->charge, notional_t{});
	EXPECT_EQ(fee->base_scaled, 0);
}

TEST(VenueFillBook, AQuoteAssetFeeRoundsUpToTheNextTickLot) {
	execution_report report  = fill_book_report();
	report.commission_scaled = 1500; // 0.000015 USDT, 1.5 tick-lots
	report.commission_asset  = "USDT";
	const auto fee = venue_fill_book::value_fee(report,
												fill_book_listing(),
												"USDT",
												"SOL");
	ASSERT_TRUE(fee.has_value());
	EXPECT_EQ(fee->charge, 2 * (units::tick * units::lot));
}

TEST(VenueFillBook, ABaseAssetFeeIsWorthItsSizeAtTheFillPrice) {
	execution_report report  = fill_book_report();
	report.commission_scaled = 100'000; // 0.001 SOL
	report.commission_asset  = "SOL";
	const auto fee = venue_fill_book::value_fee(report,
												fill_book_listing(),
												"USDT",
												"SOL");
	ASSERT_TRUE(fee.has_value());
	// 0.001 SOL at 153.45 is 0.15345 USDT: 15345 tick-lots exactly.
	EXPECT_EQ(fee->charge, 15'345 * (units::tick * units::lot));
	EXPECT_EQ(fee->base_scaled, 100'000);
}

TEST(VenueFillBook, AFeeInAThirdAssetIsUnvalued) {
	execution_report report  = fill_book_report();
	report.commission_scaled = 100;
	report.commission_asset  = "BNB";
	EXPECT_FALSE(venue_fill_book::value_fee(report,
											fill_book_listing(),
											"USDT",
											"SOL")
					 .has_value());
}

TEST(VenueFillBook, AHistoryTradeTranslatesToTheReportTheStreamWouldSend) {
	const execution_report report = venue::binance::to_execution_report(
		{.id               = 9001,
		 .order_id         = 777,
		 .is_buyer         = false,
		 .is_maker         = false,
		 .price            = "153.45",
		 .qty              = "0.002",
		 .commission       = "0.00001500",
		 .commission_asset = "USDT",
		 .time_ms          = 5},
		2,
		3);
	EXPECT_EQ(report.side, side_t::ask);
	EXPECT_EQ(report.venue_trade_id, 9001);
	EXPECT_EQ(report.venue_order_id, 777);
	EXPECT_EQ(report.last_price_scaled, 15345);
	EXPECT_EQ(report.last_qty_scaled, 2);
	EXPECT_EQ(report.commission_scaled, 1500);
	EXPECT_TRUE(report.has_fill());
}
