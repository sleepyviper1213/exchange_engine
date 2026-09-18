// The other half of the kill switch: pulling what is already resting.
//
// `global_kill_switch.test.cpp` pins the rules that stop new orders. This pins
// the walk that withdraws the old ones, and the two properties that are easy to
// get backwards:
//
//   * it must work in HALTED, the one state in which a *strategy's* cancel is
//     refused - the refusal is about not trusting the ids, and these ids come
//     from the gate's own ledger;
//   * it must not retire the ledger, because a CANCEL that has been sent is not
//     a CANCEL that has been honoured.
//
// What failure looks like: an operator trips the switch, sees a clean return,
// and the book still holds quotes nobody is managing.

#include "event/command.hpp"
#include "gate.fixture.hpp"
#include "order_book/order_state.hpp"
#include "order_book/outcome.hpp"
#include "orders/types.hpp"
#include "risk_management/gate.hpp"
#include "risk_management/hooks/system/trading_state.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace {

using namespace exchange::risk;
using exchange::risk::hooks::system::trading_state;

/// @brief Ids 1..n placed as resting bids, one lot each.
void mass_cancel_place(harness &h, order_id_t count) {
	for (order_id_t id = 1; id <= count; ++id)
		ASSERT_TRUE(h.place(buy(id, 100, 1)));
}

/// @brief The order ids a batch of CANCELs names, sorted.
[[nodiscard]] std::vector<order_id_t>
mass_cancel_ids(std::span<const command> batch) {
	std::vector<order_id_t> ids;
	for (const command &c : batch) {
		EXPECT_EQ(c.type, exchange::engine::event::command_type::CANCEL);
		EXPECT_EQ(c.symbol, SYMBOL);
		ids.push_back(c.as_cancel());
	}
	std::ranges::sort(ids);
	return ids;
}

/**
 * @brief A sink that takes a fixed number of batches and then behaves like a
 *        full queue.
 *
 * `recording_sink` refuses all or nothing, which cannot stop a mass cancel
 * *part-way* - and part-way is the only interesting back-pressure case, because
 * it is the one where some CANCELs have already landed.
 */
class mass_cancel_stalling_sink {
public:
	bool submit_range(std::span<const command> batch) {
		if (budget_ == 0) return false;
		--budget_;
		commands_.append_range(batch);
		return true;
	}

	/// @brief Batches this sink will accept before it starts refusing.
	void set_budget(std::size_t batches) noexcept { budget_ = batches; }

	[[nodiscard]] const std::vector<command> &commands() const noexcept {
		return commands_;
	}

	void clear() noexcept { commands_.clear(); }

private:
	std::size_t budget_ = static_cast<std::size_t>(-1);
	std::vector<command> commands_;
};

TEST(RiskGateMassCancel, AnEmptyLedgerCancelsNothingAndIsComplete) {
	harness h;

	const mass_cancel_result result = h.gate().mass_cancel();

	EXPECT_EQ(result.cancelled, 0U);
	EXPECT_TRUE(is_complete(result));
	EXPECT_TRUE(h.delivered().empty());
}

TEST(RiskGateMassCancel, EveryWorkingOrderIsNamedByExactlyOneCancel) {
	harness h;
	mass_cancel_place(h, 5);
	h.sink().clear();

	const mass_cancel_result result = h.gate().mass_cancel();

	EXPECT_EQ(result.cancelled, 5U);
	EXPECT_TRUE(is_complete(result));
	EXPECT_EQ(mass_cancel_ids(h.delivered()),
			  (std::vector<order_id_t>{1, 2, 3, 4, 5}));
}

TEST(RiskGateMassCancel, ItRunsInTheOneStateAStrategySCancelIsRefusedIn) {
	harness h;
	mass_cancel_place(h, 3);
	h.breaker().trip(trading_state::HALTED);
	h.sink().clear();

	// The contrast that makes the point: the same command from a strategy is
	// refused here, because HALTED is the state that does not trust the ids.
	EXPECT_TRUE(h.cancel(1));
	EXPECT_TRUE(h.delivered().empty());
	EXPECT_TRUE(h.saw(breach::HALTED));

	// The gate's own walk goes through, because these ids came out of its
	// ledger rather than out of the strategy.
	const mass_cancel_result result = h.gate().mass_cancel();
	EXPECT_EQ(result.cancelled, 3U);
	EXPECT_EQ(mass_cancel_ids(h.delivered()),
			  (std::vector<order_id_t>{1, 2, 3}));
}

TEST(RiskGateMassCancel, TheLedgerStillHoldsWhatWasOnlyAskedToCancel) {
	harness h;
	mass_cancel_place(h, 4);
	ASSERT_EQ(h.gate().working_orders(), 4U);

	ASSERT_EQ(h.gate().mass_cancel().cancelled, 4U);

	// Still exposure: a CANCEL can cross a fill in flight, so nothing is
	// retired until the venue says so.
	EXPECT_EQ(h.gate().working_orders(), 4U);
	EXPECT_EQ(h.working(side_t::bid), 4);
}

TEST(RiskGateMassCancel, TheVenueSConfirmationIsWhatRetiresTheLedger) {
	harness h;
	mass_cancel_place(h, 4);
	ASSERT_EQ(h.gate().mass_cancel().cancelled, 4U);

	for (order_id_t id = 1; id <= 4; ++id) {
		const exchange::engine::order_state state{1};
		h.outcome(order_outcome::cancelled(id, state));
	}

	EXPECT_EQ(h.gate().working_orders(), 0U);
	EXPECT_EQ(h.working(side_t::bid), 0);
	// And a second walk then has nothing left to do.
	EXPECT_EQ(h.gate().mass_cancel().cancelled, 0U);
}

TEST(RiskGateMassCancel, ALedgerLargerThanOneChunkIsWalkedToTheEnd) {
	harness h;
	constexpr order_id_t MASS_CANCEL_SPAN =
		test_gate::MASS_CANCEL_CHUNK * 2 + 5;
	mass_cancel_place(h, MASS_CANCEL_SPAN);
	h.sink().clear();

	const mass_cancel_result result = h.gate().mass_cancel();

	EXPECT_EQ(result.cancelled, MASS_CANCEL_SPAN);
	EXPECT_TRUE(is_complete(result));
	const std::vector<order_id_t> ids = mass_cancel_ids(h.delivered());
	ASSERT_EQ(ids.size(), MASS_CANCEL_SPAN);
	for (std::size_t i = 0; i < ids.size(); ++i)
		EXPECT_EQ(ids[i], static_cast<order_id_t>(i + 1));
}

TEST(RiskGateMassCancel, AFullSinkStopsTheWalkAndSaysHowMuchWasLeft) {
	mass_cancel_stalling_sink sink;
	position_book positions{8};
	circuit_breaker breaker;
	manual_clock clock;
	risk_gate<mass_cancel_stalling_sink, manual_clock>
		gate{sink, SYMBOL, permissive(), positions, breaker, 0, clock};

	constexpr order_id_t MASS_CANCEL_STALL_COUNT =
		decltype(gate)::MASS_CANCEL_CHUNK + 10;
	for (order_id_t id = 1; id <= MASS_CANCEL_STALL_COUNT; ++id)
		ASSERT_TRUE(gate.submit(command::place(buy(id, 100, 1))));

	sink.clear();
	sink.set_budget(1); // one chunk through, then a full queue
	const std::uint64_t stalls_before = gate.stalls();

	const mass_cancel_result result = gate.mass_cancel();

	EXPECT_EQ(result.cancelled, decltype(gate)::MASS_CANCEL_CHUNK);
	EXPECT_EQ(result.remaining, 10U);
	EXPECT_FALSE(is_complete(result));
	EXPECT_EQ(gate.stalls(), stalls_before + 1)
		<< "back-pressure on a mass cancel is a stall, not a refusal";
	EXPECT_EQ(sink.commands().size(), decltype(gate)::MASS_CANCEL_CHUNK);

	// Retrying restarts from the first slot, because nothing was retired to
	// mark the first chunk done - so the whole ledger is re-sent. Duplicates
	// come back CANCEL_REJECTED and are the deliberate cost of not losing one.
	sink.clear();
	sink.set_budget(static_cast<std::size_t>(-1));
	const mass_cancel_result retry = gate.mass_cancel();
	EXPECT_EQ(retry.cancelled, MASS_CANCEL_STALL_COUNT);
	EXPECT_TRUE(is_complete(retry));
	EXPECT_EQ(sink.commands().size(), MASS_CANCEL_STALL_COUNT);
}

TEST(RiskGateMassCancel, TheCancelsAreChargedAgainstTheRateWindow) {
	harness h;
	mass_cancel_place(h, 6);
	const std::uint32_t used_before = h.gate().rate().used(h.clock().now());

	ASSERT_EQ(h.gate().mass_cancel().cancelled, 6U);

	// A cancel is a real message and should crowd out new orders, which is
	// exactly what screen_reducing already charges one for.
	EXPECT_EQ(h.gate().rate().used(h.clock().now()), used_before + 6);
}

TEST(RiskGateMassCancel, TheCounterTalliesEveryCancelIncludingARetrySResends) {
	harness h;
	mass_cancel_place(h, 3);
	EXPECT_EQ(h.gate().mass_cancelled(), 0U);

	ASSERT_EQ(h.gate().mass_cancel().cancelled, 3U);
	EXPECT_EQ(h.gate().mass_cancelled(), 3U);

	// Nothing was retired, so a second walk sends the same three again and the
	// counter says six. It counts messages, not orders.
	ASSERT_EQ(h.gate().mass_cancel().cancelled, 3U);
	EXPECT_EQ(h.gate().mass_cancelled(), 6U);
}

} // namespace
