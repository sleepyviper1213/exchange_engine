// What an execution does to the account. Each case is a way the position used
// to drift from the venue's trade history, and the counter that now shows it.

#include "risk_management/hooks/pre_trade/fill_booking.hpp"

#include "orders/outcome.hpp"
#include "orders/trade.hpp"
#include "orders/types.hpp"

#include <gtest/gtest.h>

namespace {

using namespace exchange;

using exchange::engine::order_outcome;
using exchange::engine::reject_reason;
using exchange::engine::trade;
using exchange::risk::hooks::pre_trade::fill_booking;
using exchange::risk::hooks::pre_trade::position_book;
using exchange::risk::hooks::pre_trade::working_ledger;

constexpr symbol_id_t FILL_BOOKING_SYMBOL = 3;

/// A ledger, a position book and a booking over both, with order 42 working
/// as a 10-lot bid at 100 - entered the way the gate's commit enters it.
struct fill_booking_desk {
	position_book positions{16};
	working_ledger ledger{8};
	fill_booking booking{positions, ledger, FILL_BOOKING_SYMBOL};

	fill_booking_desk() {
		EXPECT_TRUE(
			ledger.insert(42, side_t::bid, at_tick(100), 10 * units::lot));
		positions.add_working(FILL_BOOKING_SYMBOL, side_t::bid, 10 * units::lot);
	}

	[[nodiscard]] auto snapshot() const {
		return positions.snapshot(FILL_BOOKING_SYMBOL);
	}
};

[[nodiscard]] trade fill_booking_print(order_id_t aggressor, order_id_t resting,
									   quantity_t lots, trade_id_t id = 1) {
	return trade{.aggressor      = aggressor,
				 .resting        = resting,
				 .price          = at_tick(100),
				 .volume         = lots,
				 .id             = id,
				 .sequence       = 0,
				 .timestamp      = 0,
				 .aggressor_side = side_t::ask};
}

TEST(RiskFillBooking, AnEnginePrintAgainstOurOrderMovesThePosition) {
	fill_booking_desk desk;
	desk.booking.book_engine(fill_booking_print(0, 42, 4 * units::lot));
	EXPECT_EQ(desk.snapshot().net_lots, 4 * units::lot);
	EXPECT_EQ(desk.snapshot().working_bid_lots, 6 * units::lot);
}

TEST(RiskFillBooking, SomebodyElsesPrintMovesNothing) {
	fill_booking_desk desk;
	desk.booking.book_engine(fill_booking_print(7, 8, 4 * units::lot));
	EXPECT_EQ(desk.snapshot().net_lots, volume_t{});
}

TEST(RiskFillBooking, AnEnginePrintIsCountedNotBookedOnceTheVenueIsTheMarket) {
	fill_booking_desk desk;
	desk.booking.set_books_engine_fills(false);
	desk.booking.book_engine(fill_booking_print(0, 42, 4 * units::lot));
	EXPECT_EQ(desk.snapshot().net_lots, volume_t{});
	EXPECT_EQ(desk.booking.ignored_engine_fills(), 1U);
}

TEST(RiskFillBooking, AVenueFillTheLedgerAlreadyLetGoIsStillBooked) {
	fill_booking_desk desk;
	(void)desk.ledger.retire(42);
	desk.booking.book_venue(42, side_t::bid, at_tick(100), 3 * units::lot);
	EXPECT_EQ(desk.snapshot().net_lots, 3 * units::lot);
	EXPECT_EQ(desk.booking.unledgered_fills(), 1U);
}

TEST(RiskFillBooking, AVenueFillLargerThanWhatIsWorkingBooksTheExcessToo) {
	fill_booking_desk desk;
	desk.booking.book_venue(42, side_t::bid, at_tick(100), 12 * units::lot);
	EXPECT_EQ(desk.snapshot().net_lots, 12 * units::lot);
	EXPECT_EQ(desk.snapshot().working_bid_lots, volume_t{});
	EXPECT_EQ(desk.booking.unledgered_fills(), 1U);
}

TEST(RiskFillBooking, ACancellationReleasesWhatWasWorking) {
	fill_booking_desk desk;
	desk.booking.retire(
		order_outcome{.id        = 42,
					  .type      = engine::OutcomeType::CANCELLED,
					  .reason    = reject_reason::NONE,
					  .status    = engine::OrderStatus::CANCELLED,
					  .traded    = {},
					  .remaining = {},
					  .sequence  = 0,
					  .trade_id  = 0});
	EXPECT_EQ(desk.snapshot().working_bid_lots, volume_t{});
	EXPECT_FALSE(desk.ledger.find(42).has_value());
}

TEST(RiskFillBooking, FeesComeOffTheProfit) {
	fill_booking_desk desk;
	desk.booking.book_engine(fill_booking_print(0, 42, 4 * units::lot));
	const notional_t before = desk.booking.pnl(at_tick(100));
	desk.booking.charge(5 * (units::tick * units::lot));
	EXPECT_EQ(desk.booking.pnl(at_tick(100)),
			  before - 5 * (units::tick * units::lot));
	EXPECT_EQ(desk.booking.fees(), 5 * (units::tick * units::lot));
}

} // namespace
