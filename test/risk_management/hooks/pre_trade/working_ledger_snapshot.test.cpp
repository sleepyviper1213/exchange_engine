// Enumerating the ledger, which until the mass cancel needed it nothing did.
//
// The interesting tests are the chunked ones. `snapshot` walks slots rather
// than entries, so a caller resuming from a cursor is trusting that the walk
// neither repeats an entry nor steps over one - and a table that is mostly
// empty slots is exactly where an off-by-one hides. What would failure look
// like: an order silently missing from a mass cancel, which is an order left
// live in the book after an operator believed they had pulled everything.

#include "orders/types.hpp"
#include "risk_management/hooks/pre_trade/working_ledger.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

using exchange::order_id_t;
using exchange::side_t;
using exchange::risk::hooks::pre_trade::ledger_cursor;
using exchange::risk::hooks::pre_trade::working_ledger;
using exchange::risk::hooks::pre_trade::working_order;

/// @brief Walk @p ledger to exhaustion through a buffer of @p chunk entries and
///        return every id it reported, in the order it reported them.
///
/// Duplicates are deliberately *not* removed - a walk that reports one entry
/// twice is the failure this file exists to catch, so the caller sorts and
/// checks rather than being handed a set that has already hidden it.
[[nodiscard]] std::vector<order_id_t>
ledger_walk_ids(const working_ledger &ledger, std::size_t chunk) {
	std::vector<working_order> buffer(chunk);
	std::vector<order_id_t> seen;
	ledger_cursor cursor{};

	for (;;) {
		const auto scan = ledger.snapshot(buffer, cursor);
		if (scan.written == 0) break;
		for (std::size_t i = 0; i < scan.written; ++i)
			seen.push_back(buffer[i].id);
		cursor = scan.next;
	}
	return seen;
}

TEST(RiskWorkingLedgerSnapshot, AnEmptyLedgerReportsNothing) {
	const working_ledger ledger{10};
	std::array<working_order, 4> buffer{};

	const auto scan = ledger.snapshot(buffer);
	EXPECT_EQ(scan.written, 0U);
}

TEST(RiskWorkingLedgerSnapshot, ASingleEntryComesBackWithEveryField) {
	working_ledger ledger{10};
	ASSERT_TRUE(ledger.insert(42, side_t::ask, 1250, 7));

	std::array<working_order, 4> buffer{};
	const auto scan = ledger.snapshot(buffer);

	ASSERT_EQ(scan.written, 1U);
	EXPECT_EQ(buffer[0].id, 42U);
	EXPECT_EQ(buffer[0].side, side_t::ask);
	EXPECT_EQ(buffer[0].price, 1250U);
	EXPECT_EQ(buffer[0].lots, 7);
}

TEST(RiskWorkingLedgerSnapshot, ABufferLargeEnoughTakesTheWholeLedgerAtOnce) {
	working_ledger ledger{64};
	for (order_id_t id = 1; id <= 20; ++id)
		ASSERT_TRUE(ledger.insert(id, side_t::bid, 100, 1));

	std::array<working_order, 32> buffer{};
	const auto scan = ledger.snapshot(buffer);
	EXPECT_EQ(scan.written, 20U);

	// And the next call from that cursor reports the walk finished rather than
	// starting over, which is what terminates a caller's loop.
	const auto done = ledger.snapshot(buffer, scan.next);
	EXPECT_EQ(done.written, 0U);
}

TEST(RiskWorkingLedgerSnapshot, AChunkedWalkReportsEveryEntryExactlyOnce) {
	working_ledger ledger{256};
	constexpr order_id_t LEDGER_WALK_COUNT = 100;
	for (order_id_t id = 1; id <= LEDGER_WALK_COUNT; ++id)
		ASSERT_TRUE(ledger.insert(id, side_t::bid, 100, 1));

	// Three widths, because the bug this is looking for is a cursor that skips
	// or repeats at a chunk boundary and a single width can sit clear of it.
	for (const std::size_t chunk :
		 {std::size_t{1}, std::size_t{7}, std::size_t{64}}) {
		std::vector<order_id_t> seen = ledger_walk_ids(ledger, chunk);
		std::ranges::sort(seen);

		ASSERT_EQ(seen.size(), LEDGER_WALK_COUNT) << "chunk of " << chunk;
		for (std::size_t i = 0; i < seen.size(); ++i)
			EXPECT_EQ(seen[i], static_cast<order_id_t>(i + 1))
				<< "chunk of " << chunk;
	}
}

TEST(RiskWorkingLedgerSnapshot, EmptySlotsBetweenEntriesAreSteppedOver) {
	// A deliberately sparse table: a handful of entries in a table sized for
	// far more, so almost every slot the walk visits is empty. A walk that
	// stopped at the first empty slot - which is what every *probe* in this
	// table does - would report only the entries before the first gap.
	working_ledger ledger{200};
	ASSERT_GT(ledger.slot_count(), 200U);
	for (order_id_t id = 1; id <= 5; ++id)
		ASSERT_TRUE(ledger.insert(id * 1000, side_t::ask, 50, 2));

	std::vector<order_id_t> seen = ledger_walk_ids(ledger, 2);
	std::ranges::sort(seen);
	EXPECT_EQ(seen, (std::vector<order_id_t>{1000, 2000, 3000, 4000, 5000}));
}

TEST(RiskWorkingLedgerSnapshot, ARetiredEntryIsNoLongerReported) {
	working_ledger ledger{32};
	for (order_id_t id = 1; id <= 6; ++id)
		ASSERT_TRUE(ledger.insert(id, side_t::bid, 100, 1));
	ASSERT_TRUE(ledger.retire(3).has_value());
	ASSERT_TRUE(ledger.retire(5).has_value());

	std::vector<order_id_t> seen = ledger_walk_ids(ledger, 4);
	std::ranges::sort(seen);
	EXPECT_EQ(seen, (std::vector<order_id_t>{1, 2, 4, 6}));
}

TEST(RiskWorkingLedgerSnapshot,
	 APartiallyFilledEntryReportsWhatIsStillWorking) {
	working_ledger ledger{10};
	ASSERT_TRUE(ledger.insert(9, side_t::bid, 300, 10));
	ASSERT_TRUE(ledger.take(9, 4).has_value());

	std::array<working_order, 4> buffer{};
	const auto scan = ledger.snapshot(buffer);

	ASSERT_EQ(scan.written, 1U);
	EXPECT_EQ(buffer[0].id, 9U);
	EXPECT_EQ(buffer[0].lots, 6) << "a snapshot reports the remainder, not the "
									"quantity the order was placed for";
}

TEST(RiskWorkingLedgerSnapshot, ClearingLeavesNothingToWalk) {
	working_ledger ledger{32};
	for (order_id_t id = 1; id <= 8; ++id)
		ASSERT_TRUE(ledger.insert(id, side_t::bid, 100, 1));
	ledger.clear();

	EXPECT_TRUE(ledger_walk_ids(ledger, 4).empty());
}

} // namespace
