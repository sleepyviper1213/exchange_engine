#include "binance_trade.fixture.hpp"
#include "core/chrono/ingress.hpp"
#include "market_data/binance/binance_trade.hpp"
#include "market_data/binance/normalise.hpp"
#include "market_data/normalised.hpp"

#include <gtest/gtest.h>

#include <chrono>

using exchange::side_t;
using exchange::market_data::trade_print;
using exchange::market_data::binance::normalise;
using exchange::market_data::binance::parse_binance_trade;
using exchange::market_data::binance::trade_message;

// normalise(trade_message) - the venue-to-neutral seam for the tape, and the
// one place the maker flag becomes an aggressor side.

namespace {

/// A decoded frame with only the fields a given assertion cares about set.
trade_message normalise_trade_message(bool buyer_is_maker) {
	trade_message trade;
	trade.trade_id       = 42;
	trade.event_time     = 2000;
	trade.trade_time     = 1000;
	trade.price          = 15345;
	trade.qty            = 1000;
	trade.buyer_is_maker = buyer_is_maker;
	return trade;
}

// --------------------------------------------------------------------------
// The sign convention. Getting this backwards produces a tape whose every
// print is on the wrong side - no type catches it and no total reveals it, so
// it is pinned from both directions.
// --------------------------------------------------------------------------

TEST(NormaliseTrade, BuyerAsMakerMeansTheTakerSold) {
	// m == true: the resting order was a bid, so the aggressor was a seller,
	// and a sell order is one that would have rested on the ask.
	const trade_print print = normalise(normalise_trade_message(true));
	EXPECT_EQ(print.aggressor, side_t::ask);
}

TEST(NormaliseTrade, SellerAsMakerMeansTheTakerBought) {
	const trade_print print = normalise(normalise_trade_message(false));
	EXPECT_EQ(print.aggressor, side_t::bid);
}

TEST(NormaliseTrade, TheFixtureFrameDecodesAsASellIntoTheBid) {
	// The two halves joined up: a real wire frame, through the real decoder,
	// landing on the side the flag in it says.
	const auto decoded = parse_binance_trade(BINANCE_TRADE_JSON,
											 BINANCE_TRADE_PRICE_DECIMALS,
											 BINANCE_TRADE_QTY_DECIMALS);
	ASSERT_TRUE(decoded.has_value());
	EXPECT_EQ(normalise(*decoded).aggressor, side_t::ask);
}

// --------------------------------------------------------------------------
// Units and identity
// --------------------------------------------------------------------------

TEST(NormaliseTrade, ConvertsMillisecondsToNanoseconds) {
	const trade_print print = normalise(normalise_trade_message(true));
	EXPECT_EQ(print.event_time, std::chrono::nanoseconds{1'000'000'000});
}

TEST(NormaliseTrade, StampsTheExecutionTimeRatherThanTheSendTime) {
	// T, not E. The difference between them is the venue's own internal delay,
	// which is not a fact about this process and must not drive its clock.
	const trade_print print = normalise(normalise_trade_message(true));
	EXPECT_EQ(print.event_time,
			  std::chrono::duration_cast<std::chrono::nanoseconds>(
				  std::chrono::milliseconds{1000}));
}

TEST(NormaliseTrade, CarriesTheTradeIdAcrossAsASequence) {
	EXPECT_EQ(normalise(normalise_trade_message(true)).id, 42);
}

TEST(NormaliseTrade, CopiesTheScaledPriceAndSizeUnchanged) {
	// Scaling happened in the decoder; normalising must not scale again.
	const trade_print print = normalise(normalise_trade_message(true));
	EXPECT_EQ(print.price, 15345);
	EXPECT_EQ(print.qty, 1000);
}

TEST(NormaliseTrade, LeavesTheIngressStampUnset) {
	// Arrival time is not venue knowledge. Whoever received the bytes owns it
	// and sets it afterwards; normalise() inventing one would report the
	// decoder's own speed as a reaction time.
	const trade_print print = normalise(normalise_trade_message(true));
	EXPECT_FALSE(exchange::core::chrono::has_ingress(print.ingress));
}

} // namespace
