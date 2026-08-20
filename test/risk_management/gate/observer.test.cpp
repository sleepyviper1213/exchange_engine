// The push side of the gate: what an observer is told, and when.
//
// The counters were always readable, so the interesting claims here are about
// *timing* and *arithmetic-path cost*, not about content: a refusal is
// announced once and only after it is real, an attempt the sink rolled back
// announces nothing, and a gate that opts out of observing is byte-for-byte the
// gate that never had the parameter.

#include "risk_management/hooks/observer.hpp"

#include "gate.fixture.hpp"
#include "risk_management/gate.hpp"
#include "risk_management/hooks/system/circuit_breaker.hpp"
#include "trading-engine/event/command.hpp"
#include "trading-engine/order_book/reject_reason.hpp"
#include "trading-engine/orders/types.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <memory>
#include <vector>

namespace {

using namespace exchange;
using namespace exchange::engine;
using namespace exchange::engine::event;
using namespace exchange::risk;
using namespace exchange::risk::hooks;
using namespace exchange::risk::hooks::system;

/// @brief One refusal as the observer saw it.
struct seen_breach {
	order_id_t id;      ///< the order refused, or zero for an anonymous one
	command::Type type;
	breach_set reasons; ///< every rule, not just the reported one
};

/// @brief One trip.
struct seen_halt {
	trading_state to;
	trip_cause why;
};

/// @brief What the observer wrote down, shared so a test can read it after the
///        gate copied the observer in.
struct observer_log {
	std::vector<seen_breach> breaches;
	std::vector<seen_halt> halts;
	std::vector<std::size_t> stalls;
};

/**
 * @brief An observer that subscribes to all three hooks and records them.
 *
 * Holds a handle rather than the state: the gate takes its observer *by value*,
 * which is what makes an empty one free, so anything a test wants to read
 * afterwards has to live outside the gate. Same shape as @c manual_clock.
 */
class recording_gate_observer {
public:
	explicit recording_gate_observer(std::shared_ptr<observer_log> log)
		: log_(std::move(log)) {}

	void on_breach(const command &cmd, breach_set reasons) noexcept {
		const order_id_t id = cmd.type == command::Type::PLACE
								  ? cmd.as_place().id
								  : order_id_t{0};
		log_->breaches.push_back(
			{.id = id, .type = cmd.type, .reasons = reasons});
	}

	void on_halt(trading_state to, trip_cause why) noexcept {
		log_->halts.push_back({.to = to, .why = why});
	}

	void on_stall(std::size_t retained) noexcept {
		log_->stalls.push_back(retained);
	}

private:
	std::shared_ptr<observer_log> log_;
};

/// @brief Subscribes to one hook and holds nothing - the shape the size claim
///        below is about.
struct empty_breach_observer {
	void on_breach(const command &, breach_set) noexcept {}
};

/// @brief What a typo looks like to the compiler: a hook nobody will ever call.
struct misspelled_observer {
	void on_breech(const command &, breach_set) noexcept {}
};

/// @brief A hook that can throw is not admissible on this path.
struct throwing_observer {
	void on_breach(const command &, breach_set) {}
};

// The concept discriminates, which is the whole reason it exists: a gate
// declared over an observer that subscribes to nothing would compile and stay
// silent forever. @see hooks/observer.hpp
static_assert(risk_observer<no_observer>);
static_assert(risk_observer<recording_gate_observer>);
static_assert(risk_observer<empty_breach_observer>);
static_assert(!risk_observer<misspelled_observer>);
static_assert(!risk_observer<throwing_observer>);

static_assert(breach_observer<recording_gate_observer>);
static_assert(halt_observer<recording_gate_observer>);
static_assert(stall_observer<recording_gate_observer>);
static_assert(!halt_observer<empty_breach_observer>);
static_assert(!stall_observer<empty_breach_observer>);

// An observer that carries no state costs the gate no space: the member is
// [[no_unique_address]], the way the clock already is. If this ever fails, an
// attached observer has started paying for itself in cache footprint on the
// screening path rather than only on the refusal path.
static_assert(
	sizeof(risk_gate<recording_sink, manual_clock>) ==
		sizeof(risk_gate<recording_sink, manual_clock, empty_breach_observer>),
	"an empty observer must not enlarge the gate");

/// @brief A gate wired to a recording observer, plus the pieces it needs.
///
/// Not the shared @c harness: that one builds a @c test_gate, and the whole
/// subject here is a third template argument it does not have.
class observed_gate {
public:
	using gate_t =
		risk_gate<recording_sink, manual_clock, recording_gate_observer>;

	explicit observed_gate(const risk_limits &limits = permissive(),
						   price_t reference = 0, auto_trip_after trip = {})
		: breaker_(trip.breaches, TEST_WINDOW_LOG2),
		  gate_(sink_, SYMBOL, limits, positions_, breaker_, reference, clock_,
				recording_gate_observer{log_}) {}

	[[nodiscard]] bool place(const order &o) {
		return gate_.submit(command::place(o));
	}

	[[nodiscard]] gate_t &gate() noexcept { return gate_; }

	[[nodiscard]] recording_sink &sink() noexcept { return sink_; }

	[[nodiscard]] circuit_breaker &breaker() noexcept { return breaker_; }

	[[nodiscard]] const observer_log &log() const noexcept { return *log_; }

private:
	std::shared_ptr<observer_log> log_ = std::make_shared<observer_log>();
	recording_sink sink_;
	position_book positions_{8};
	circuit_breaker breaker_;
	manual_clock clock_;
	gate_t gate_;
};

TEST(RiskGateObserver, APassedCommandIsNotAnnounced) {
	// The point of the design: nothing hangs off the accepted path.
	observed_gate g;
	ASSERT_TRUE(g.place(buy(1, 100, 10)));

	EXPECT_EQ(g.gate().passed(), 1U);
	EXPECT_TRUE(g.log().breaches.empty());
	EXPECT_TRUE(g.log().halts.empty());
	EXPECT_TRUE(g.log().stalls.empty());
}

TEST(RiskGateObserver, ARefusalNamesTheOrderAndEveryRuleItBroke) {
	// Two rules at once, which is exactly what a client's single reject_reason
	// cannot express and the reason the hook is handed the whole set.
	risk_limits limits        = permissive();
	limits.max_order_qty      = 5;
	limits.max_order_notional = 100;
	observed_gate g{limits};

	ASSERT_TRUE(g.place(buy(7, 100, 10)));

	ASSERT_EQ(g.log().breaches.size(), 1U);
	const seen_breach &seen = g.log().breaches.front();
	EXPECT_EQ(seen.id, 7U);
	EXPECT_EQ(seen.type, command::Type::PLACE);
	EXPECT_TRUE(seen.reasons.test(breach::ORDER_QUANTITY));
	EXPECT_TRUE(seen.reasons.test(breach::ORDER_NOTIONAL));
	// And it is the set, not the reported reason: the outcome a client is told
	// carries only the first of those two.
	EXPECT_EQ(first_reason(seen.reasons), reject_reason::RISK_ORDER_QUANTITY);
}

TEST(RiskGateObserver, AnAnonymousRefusalIsAnnouncedThoughNoOutcomeCanBe) {
	// An ADD names no order, so rejections() has nothing to describe it with -
	// the hook is the only way a refused seed is ever visible.
	risk_limits limits   = permissive();
	limits.max_order_qty = 5;
	observed_gate g{limits};

	ASSERT_TRUE(g.gate().submit(
		command::add(SYMBOL, side_t::bid, /*price=*/100, /*volume=*/10)));

	EXPECT_TRUE(g.gate().rejections().empty());
	ASSERT_EQ(g.log().breaches.size(), 1U);
	EXPECT_EQ(g.log().breaches.front().type, command::Type::ADD);
	EXPECT_EQ(g.log().breaches.front().id, 0U);
	EXPECT_TRUE(g.log().breaches.front().reasons.test(breach::ORDER_QUANTITY));
}

TEST(RiskGateObserver, OneAnnouncementPerRefusalAcrossABatch) {
	risk_limits limits   = permissive();
	limits.max_order_qty = 5;
	observed_gate g{limits};

	ASSERT_TRUE(g.gate().submit_range(std::array{
		command::place(buy(1, 100, 10)), // refused
		command::place(buy(2, 100, 1)),  // passes
		command::place(buy(3, 100, 10))  // refused
	}));

	ASSERT_EQ(g.log().breaches.size(), 2U);
	EXPECT_EQ(g.log().breaches[0].id, 1U);
	EXPECT_EQ(g.log().breaches[1].id, 3U);
	EXPECT_EQ(g.gate().refused(), 2U);
}

TEST(RiskGateObserver, ARolledBackAttemptAnnouncesTheStallAndNoRefusal) {
	// The property that makes the hook's call count equal refused(): a batch
	// the sink would not take is re-screened, so announcing its refusals on the
	// failed attempt would report each of them twice.
	risk_limits limits   = permissive();
	limits.max_order_qty = 5;
	observed_gate g{limits};
	g.sink().refuse(true);

	ASSERT_FALSE(g.gate().submit_range(std::array{
		command::place(buy(1, 100, 10)), // would be refused on risk grounds
		command::place(buy(2, 100, 1))   // would have been delivered
	}));

	EXPECT_TRUE(g.log().breaches.empty());
	ASSERT_EQ(g.log().stalls.size(), 1U);
	EXPECT_EQ(g.log().stalls.front(), 2U); // the whole batch is held back

	// The retry lands, and now the refusal is announced - exactly once.
	g.sink().refuse(false);
	ASSERT_TRUE(
		g.gate().submit_range(std::array{command::place(buy(1, 100, 10)),
										 command::place(buy(2, 100, 1))}));

	EXPECT_EQ(g.log().breaches.size(), 1U);
	EXPECT_EQ(g.log().breaches.front().id, 1U);
	EXPECT_EQ(g.log().stalls.size(), 1U);
	EXPECT_EQ(g.gate().refused(), 1U);
}

TEST(RiskGateObserver, TheLossFloorAnnouncesItsOwnTrip) {
	risk_limits limits = permissive();
	limits.max_loss    = 500;
	observed_gate g{limits, /*reference=*/100};

	ASSERT_TRUE(g.place(buy(1, 100, 10)));
	g.gate().on_trade(
		trade{.aggressor = 1, .resting = 0, .price = 100, .volume = 10});
	ASSERT_TRUE(g.log().halts.empty());

	// Marked down through the floor by somebody else's print.
	g.gate().on_trade(
		trade{.aggressor = 900, .resting = 901, .price = 40, .volume = 1});

	ASSERT_EQ(g.log().halts.size(), 1U);
	EXPECT_EQ(g.log().halts.front().to, trading_state::CANCEL_ONLY);
	EXPECT_EQ(g.log().halts.front().why, trip_cause::LOSS_LIMIT);

	// One trip, one announcement, however far it bleeds afterwards.
	g.gate().on_trade(
		trade{.aggressor = 900, .resting = 901, .price = 10, .volume = 1});
	EXPECT_EQ(g.log().halts.size(), 1U);
	EXPECT_EQ(g.breaker().trips(), 1U);
}

TEST(RiskGateObserver, TheBreachRateCutOutAnnouncesItsOwnTrip) {
	risk_limits limits   = permissive();
	limits.max_order_qty = 1;
	observed_gate g{limits, /*reference=*/0, auto_trip_after{2}};

	ASSERT_TRUE(g.place(buy(1, 100, 99)));
	EXPECT_TRUE(g.log().halts.empty()); // one breach is not a loop

	ASSERT_TRUE(g.place(buy(2, 100, 99)));
	ASSERT_EQ(g.log().halts.size(), 1U);
	EXPECT_EQ(g.log().halts.front().to, trading_state::CANCEL_ONLY);
	EXPECT_EQ(g.log().halts.front().why, trip_cause::BREACH_RATE);
	// The pair the gate reports is the pair the breaker actually moved to.
	EXPECT_EQ(g.breaker().state(), trading_state::CANCEL_ONLY);
	EXPECT_EQ(g.breaker().cause(), trip_cause::BREACH_RATE);

	// Refusals keep coming - now on HALTED grounds - and the trip is not
	// re-announced.
	ASSERT_TRUE(g.place(buy(3, 100, 1)));
	EXPECT_EQ(g.log().halts.size(), 1U);
	EXPECT_GE(g.log().breaches.size(), 3U);
}

TEST(RiskGateObserver, AnOperatorsOwnTripIsNotAnnounced) {
	// Deliberate: the breaker is shared, this gate did not decide it, and a
	// hook that claimed to see every trip would be lying. @see halt_observer
	observed_gate g;
	g.breaker().trip(trading_state::HALTED);

	ASSERT_TRUE(g.place(buy(1, 100, 10)));
	EXPECT_TRUE(g.log().halts.empty());
	EXPECT_EQ(g.log().breaches.size(), 1U); // the refusal itself is announced
	EXPECT_TRUE(g.log().breaches.front().reasons.test(breach::HALTED));
}

} // namespace
