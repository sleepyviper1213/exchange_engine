#include "session/order_router.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <utility>
#include <vector>

// The seam where an order stops being an idea and becomes something to send.
//
// Two properties carry this file and the rest is detail. The router must never
// queue a request the engine did not accept - because a refused batch is
// retried, and a router that queued first would place the same order twice at
// the venue with nothing downstream able to tell. And it must never offer the
// venue's own mirrored depth back to the venue it came from.

using exchange::at_tick;
using exchange::order_id_t;
using exchange::side_t;
using exchange::engine::OutcomeType;
using exchange::engine::symbol_spec;
using exchange::engine::event::command;
using exchange::engine::orders::amendment;
using exchange::engine::orders::order;
using exchange::engine::orders::order_type;
using exchange::engine::orders::time_in_force_instruction;
using exchange::risk::hooks::system::circuit_breaker;
using exchange::session::gateway_limits;
using exchange::session::order_router;
using exchange::session::replace_answer;
using exchange::session::request_kind;
using exchange::session::venue_gateway;
using exchange::venue::credentials;
using exchange::venue::environment;

namespace {

/// A sink that records what it was given and can be told to refuse.
///
/// Refusal is the interesting half: it is the state in which the caller above
/// retries, and therefore the state the router's idempotency is about.
class router_sink {
public:
	[[nodiscard]] bool submit_range(std::span<const command> batch) {
		if (!accepting) return false;
		for (const command &cmd : batch) seen.push_back(cmd);
		return true;
	}

	bool accepting = true;
	std::vector<command> seen;
};

/// SOLUSDT's grid: two decimals of price, three of size, and a unit tick and
/// lot so a case can write prices as plain integers.
[[nodiscard]] symbol_spec router_listing() {
	return symbol_spec{7,
					   "SOLUSDT",
					   2,
					   3,
					   1 * exchange::units::scaled_price,
					   1 * exchange::units::scaled_size,
					   exchange::at_scaled(15345),
					   symbol_spec::NO_COLLAR};
}

[[nodiscard]] credentials router_credentials() {
	return credentials{.key = "test-key", .secret = "test-secret"};
}

[[nodiscard]] order router_order(order_id_t id) {
	order placed{};
	placed.id        = id;
	placed.symbol_id = router_listing().id();
	placed.side      = side_t::bid;
	placed.type      = order_type::LIMIT;
	placed.tif       = time_in_force_instruction::GOOD_TILL_CANCELLED;
	placed.price     = at_tick(15345);
	placed.qty       = 1500 * exchange::units::lot;
	return placed;
}

/// A router, a sink and a gateway with lifetimes that outlive each other in the
/// right order - the gateway last, because the router points at it.
class router_desk {
public:
	explicit router_desk(gateway_limits limits = {}, std::size_t capacity = 8)
		: gateway_(router_credentials(), environment::testnet, limits,
				   &breaker_),
		  router_(sink_, spec_, "SOLUSDT", capacity) {}

	/// @brief Wire the gateway in. Left to the case, so the default state -
	///        a router that sends nothing - is testable too.
	void start_sending() { router_.attach(gateway_); }

	[[nodiscard]] bool place(order_id_t id) {
		const command one = command::place(router_order(id));
		return router_.submit_range(std::span{&one, 1});
	}

	[[nodiscard]] bool cancel(order_id_t id) {
		const command one = command::cancel(spec_.id(), id);
		return router_.submit_range(std::span{&one, 1});
	}

	/// @brief Amend @p id to @p ticks for @p lots in all - the order's
	///        lifetime quantity, the way the engine states an amendment.
	[[nodiscard]] bool amend(order_id_t id, std::uint32_t ticks,
							 std::int64_t lots) {
		const command one =
			command::modify(spec_.id(),
							amendment{.id       = id,
									  .price    = at_tick(ticks),
									  .quantity = lots * exchange::units::lot});
		return router_.submit_range(std::span{&one, 1});
	}

	/// @brief One frame's worth of mirrored venue depth.
	[[nodiscard]] bool mirror_depth() {
		const std::array<command, 2> batch{
			command::add(spec_.id(),
						 side_t::bid,
						 at_tick(100),
						 5 * exchange::units::lot),
			command::reduce(spec_.id(),
							side_t::ask,
							at_tick(104),
							2 * exchange::units::lot)};
		return router_.submit_range(batch);
	}

	[[nodiscard]] router_sink &sink() noexcept { return sink_; }

	[[nodiscard]] auto &router() noexcept { return router_; }

	[[nodiscard]] circuit_breaker &breaker() noexcept { return breaker_; }

private:
	symbol_spec spec_ = router_listing();
	router_sink sink_{};
	circuit_breaker breaker_{};
	venue_gateway gateway_;
	order_router<router_sink> router_;
};

} // namespace

TEST(SessionOrderRouter, WithoutAGatewayNothingIsEverQueued) {
	router_desk desk;

	EXPECT_TRUE(desk.place(1));
	EXPECT_TRUE(desk.cancel(1));

	// The default posture of every command in this tree, and the one every run
	// of `serve` had before there was a gateway at all: the engine sees the
	// orders and the venue hears nothing.
	EXPECT_EQ(desk.sink().seen.size(), 2U);
	EXPECT_FALSE(desk.router().has_outbound());
	EXPECT_FALSE(desk.router().is_sending());
	EXPECT_EQ(desk.router().stats().offered, 0U);
}

TEST(SessionOrderRouter, APlacementAndACancelAreQueuedInTheOrderWritten) {
	router_desk desk;
	desk.start_sending();

	ASSERT_TRUE(desk.place(42));
	ASSERT_TRUE(desk.cancel(42));

	const auto queued = desk.router().take_outbound();
	ASSERT_EQ(queued.size(), 2U);
	// A cancel that overtook its own placement would be answered with "unknown
	// order" and the order it was meant to withdraw would stay working, so the
	// order of these two is a correctness property rather than a nicety.
	EXPECT_TRUE(queued[0].request.target.contains("/api/v3/order"));
	EXPECT_TRUE(queued[0].request.target.contains("newClientOrderId=ex-42"));
	EXPECT_TRUE(queued[1].request.target.contains("origClientOrderId=ex-42"));
	// Signed, and the key travels in a header rather than in the query.
	EXPECT_TRUE(queued[0].request.target.contains("signature="));
	EXPECT_FALSE(queued[0].request.target.contains("test-key"));
}

TEST(SessionOrderRouter, MirroredDepthIsPassedThroughAndNeverOffered) {
	router_desk desk;
	desk.start_sending();

	ASSERT_TRUE(desk.mirror_depth());

	// ADD and REDUCE are liquidity the venue already published, which this
	// process is copying into its own book. Sending it back would be absurd.
	EXPECT_EQ(desk.sink().seen.size(), 2U);
	EXPECT_FALSE(desk.router().has_outbound());
	EXPECT_EQ(desk.router().stats().offered, 0U);
}

TEST(SessionOrderRouter, ARefusedBatchQueuesNothingSoARetryCannotDouble) {
	router_desk desk;
	desk.start_sending();
	desk.sink().accepting = false;

	// The failure this ordering exists to prevent: the caller above retries a
	// refused batch verbatim - that is what makes the gate's back-pressure
	// lossless - so a router that queued before asking would send the same
	// order once per retry. The engine would see one command and the venue
	// would hold several real orders.
	EXPECT_FALSE(desk.place(7));
	EXPECT_FALSE(desk.place(7));
	EXPECT_FALSE(desk.router().has_outbound());
	EXPECT_EQ(desk.router().stats().offered, 0U);

	// And the retry that finally lands queues it exactly once.
	desk.sink().accepting = true;
	EXPECT_TRUE(desk.place(7));
	EXPECT_EQ(desk.router().take_outbound().size(), 1U);
}

TEST(SessionOrderRouter, AGatewayRefusalStillLetsTheEngineHaveTheCommand) {
	// One order, then the cap. The second placement cannot be sent.
	router_desk desk{gateway_limits{.max_orders = 1}};
	desk.start_sending();

	ASSERT_TRUE(desk.place(1));
	ASSERT_TRUE(desk.place(2));

	// Both reached the engine, and only the first reached the venue. The
	// divergence is the point: refusing upstream would make the caller's retry
	// loop a hang, because an order cap never clears. @see order_router.hpp
	EXPECT_EQ(desk.sink().seen.size(), 2U);
	EXPECT_EQ(desk.router().take_outbound().size(), 1U);
	EXPECT_EQ(desk.router().stats().offered, 2U);
	EXPECT_EQ(desk.router().stats().queued, 1U);
	EXPECT_EQ(desk.router().stats().refused, 1U);
}

TEST(SessionOrderRouter, AnOpenBreakerStopsPlacementsAndNotCancels) {
	router_desk desk;
	desk.start_sending();
	desk.breaker().trip(
		exchange::risk::hooks::system::trading_state::CANCEL_ONLY);

	ASSERT_TRUE(desk.place(3));
	ASSERT_TRUE(desk.cancel(3));

	// A cancel reduces exposure, so it goes out under a cancel-only breaker.
	// Refusing it would leave the very position the breaker tripped over.
	const auto queued = desk.router().take_outbound();
	ASSERT_EQ(queued.size(), 1U);
	EXPECT_TRUE(queued[0].request.target.contains("origClientOrderId=ex-3"));
}

TEST(SessionOrderRouter, AFullOutboxDropsRatherThanGrowingWithoutBound) {
	router_desk desk{gateway_limits{}, 2};
	desk.start_sending();

	ASSERT_TRUE(desk.place(1));
	ASSERT_TRUE(desk.place(2));
	ASSERT_TRUE(desk.place(3));

	// A backlog that grew without limit would turn a slow network into a flood
	// of orders priced minutes ago - real orders at prices nobody meant.
	EXPECT_EQ(desk.router().stats().queued, 2U);
	EXPECT_EQ(desk.router().stats().discarded, 1U);
	// And the drop costs no rate-limit weight, because the gateway is never
	// asked: weight spent on a request that is thrown away is weight the feed's
	// next resync cannot have.
	EXPECT_EQ(desk.router().gateway()->stats().placed, 2U);
	EXPECT_EQ(desk.router().stats().queued_now, 2U);
}

TEST(SessionOrderRouter, TakingTheOutboxLeavesItEmptyForTheNextFrame) {
	router_desk desk;
	desk.start_sending();
	ASSERT_TRUE(desk.place(1));

	EXPECT_EQ(desk.router().take_outbound().size(), 1U);
	// Moved out rather than borrowed, so the shipper can await on it while the
	// frame path keeps writing into a fresh one.
	EXPECT_FALSE(desk.router().has_outbound());
	EXPECT_EQ(desk.router().take_outbound().size(), 0U);
}

// --- amendments, which Binance spot can only take as a cancel-replace --------

TEST(SessionOrderRouter, AnAmendmentIsSentAsACancelReplaceOfTheWorkingLeg) {
	router_desk desk;
	desk.start_sending();
	ASSERT_TRUE(desk.place(42));
	(void)desk.router().take_outbound();

	ASSERT_TRUE(desk.amend(42, 15346, 1500));

	// The amendment this router dropped on the floor until it was found on
	// testnet: the engine repriced its quote and the venue's stayed put.
	const auto queued = desk.router().take_outbound();
	ASSERT_EQ(queued.size(), 1U);
	EXPECT_EQ(queued[0].kind, request_kind::replacement);
	EXPECT_TRUE(
		queued[0].request.target.contains("/api/v3/order/cancelReplace"));
	EXPECT_TRUE(
		queued[0].request.target.contains("cancelOrigClientOrderId=ex-42&"));
	EXPECT_TRUE(queued[0].request.target.contains("newClientOrderId=ex-42_1&"));
	EXPECT_TRUE(queued[0].request.target.contains("price=153.46"));
	EXPECT_EQ(desk.router().gateway()->stats().replaced, 1U);
}

TEST(SessionOrderRouter, TheReplacementRestsOnlyWhatHasNotTraded) {
	router_desk desk;
	desk.start_sending();
	ASSERT_TRUE(desk.place(42));
	(void)desk.router().take_outbound();
	(void)desk.router().on_report(42, 0, 400 * exchange::units::lot, false);

	// 1500 lots in all, 400 of them traded: the venue counts the new leg from
	// zero, so asking it for 1500 would show 400 lots nobody wanted.
	ASSERT_TRUE(desk.amend(42, 15346, 1500));
	const auto queued = desk.router().take_outbound();
	ASSERT_EQ(queued.size(), 1U);
	EXPECT_TRUE(queued[0].request.target.contains("quantity=1.100"))
		<< queued[0].request.target;
}

TEST(SessionOrderRouter, AnAmendmentToNoMoreThanTradedWithdrawsTheRest) {
	router_desk desk;
	desk.start_sending();
	ASSERT_TRUE(desk.place(42));
	(void)desk.router().take_outbound();
	(void)desk.router().on_report(42, 0, 400 * exchange::units::lot, false);

	ASSERT_TRUE(desk.amend(42, 15345, 400));
	const auto queued = desk.router().take_outbound();
	ASSERT_EQ(queued.size(), 1U);
	EXPECT_EQ(queued[0].kind, request_kind::cancel);
}

TEST(SessionOrderRouter, ASecondAmendmentWaitsForTheFirstsAnswer) {
	router_desk desk;
	desk.start_sending();
	ASSERT_TRUE(desk.place(42));
	(void)desk.router().take_outbound();

	ASSERT_TRUE(desk.amend(42, 15346, 1500));
	ASSERT_TRUE(desk.amend(42, 15347, 1500));
	ASSERT_TRUE(desk.amend(42, 15348, 1500));

	// Sent at once, the second would name leg 1 - which does not exist until
	// the first is answered, and never exists if the first fails.
	EXPECT_EQ(desk.router().take_outbound().size(), 1U);
	EXPECT_EQ(desk.router().stats().held, 2U);

	desk.router().on_replace_answered(42, replace_answer::replaced);
	const auto released = desk.router().take_outbound();
	ASSERT_EQ(released.size(), 1U);
	// Only the latest: an amendment says where the order should be.
	EXPECT_TRUE(released[0].request.target.contains("price=153.48"));
	EXPECT_TRUE(released[0].request.target.contains(
		"cancelOrigClientOrderId=ex-42_1&"));
	EXPECT_TRUE(
		released[0].request.target.contains("newClientOrderId=ex-42_2&"));
}

TEST(SessionOrderRouter, ACancelBehindAReplaceNamesTheLegTheVenueConfirmed) {
	for (const auto &[answer, leg] :
		 {std::pair{replace_answer::replaced, "ex-42_1"},
		  std::pair{replace_answer::unchanged, "ex-42&"}}) {
		router_desk desk;
		desk.start_sending();
		ASSERT_TRUE(desk.place(42));
		ASSERT_TRUE(desk.amend(42, 15346, 1500));
		(void)desk.router().take_outbound();

		ASSERT_TRUE(desk.cancel(42));
		EXPECT_FALSE(desk.router().has_outbound()) << "held until answered";

		desk.router().on_replace_answered(42, answer);
		const auto queued = desk.router().take_outbound();
		ASSERT_EQ(queued.size(), 1U);
		EXPECT_EQ(queued[0].kind, request_kind::cancel);
		EXPECT_TRUE(queued[0].request.target.contains(
			std::string("origClientOrderId=") + leg))
			<< queued[0].request.target;
	}
}

TEST(SessionOrderRouter, AnAmendmentTheVenueDidNotTakeIsToldToTheEngine) {
	router_desk desk;
	desk.start_sending();
	ASSERT_TRUE(desk.place(42));
	ASSERT_TRUE(desk.amend(42, 15346, 1500));
	(void)desk.router().take_outbound();

	desk.router().on_replace_answered(42, replace_answer::unchanged);

	// What makes the quoter withdraw the order instead of believing a price
	// the venue is not showing.
	const auto said = desk.router().take_feedback();
	ASSERT_EQ(said.size(), 1U);
	EXPECT_EQ(said[0].id, 42U);
	EXPECT_EQ(said[0].type, OutcomeType::MODIFY_REJECTED);
}

TEST(SessionOrderRouter, AReplaceThatFoundTheOrderGoneRetiresIt) {
	router_desk desk;
	desk.start_sending();
	ASSERT_TRUE(desk.place(42));
	ASSERT_TRUE(desk.amend(42, 15346, 1500));
	ASSERT_TRUE(desk.cancel(42));
	(void)desk.router().take_outbound();

	desk.router().on_replace_answered(42, replace_answer::order_gone);

	const auto said = desk.router().take_feedback();
	ASSERT_EQ(said.size(), 1U);
	EXPECT_EQ(said[0].type, OutcomeType::CANCELLED);
	EXPECT_FALSE(desk.router().has_outbound())
		<< "nothing is left to cancel, so the held cancel is not sent";
}

TEST(SessionOrderRouter, ARefusedPlacementIsToldToTheEngine) {
	router_desk desk{gateway_limits{.max_orders = 1}};
	desk.start_sending();
	ASSERT_TRUE(desk.place(1));
	ASSERT_TRUE(desk.place(2));

	// The engine still has the command - see the refusal case above - but it
	// is now told the venue does not, so the quoter places afresh rather than
	// amending an order nobody received.
	const auto said = desk.router().take_feedback();
	ASSERT_EQ(said.size(), 1U);
	EXPECT_EQ(said[0].id, 2U);
	EXPECT_EQ(said[0].type, OutcomeType::REJECTED);
	EXPECT_EQ(
		desk.router().gateway()->stats().refused_by.at(static_cast<std::size_t>(
			exchange::session::gateway_refusal::order_cap_reached)),
		1U);
}

TEST(SessionOrderRouter, AReplaceUnderACancelOnlyBreakerIsRefusedAndToldBack) {
	router_desk desk;
	desk.start_sending();
	ASSERT_TRUE(desk.place(42));
	(void)desk.router().take_outbound();
	desk.breaker().trip(
		exchange::risk::hooks::system::trading_state::CANCEL_ONLY);

	ASSERT_TRUE(desk.amend(42, 15346, 1500));

	EXPECT_FALSE(desk.router().has_outbound());
	const auto said = desk.router().take_feedback();
	ASSERT_EQ(said.size(), 1U);
	EXPECT_EQ(said[0].type, OutcomeType::MODIFY_REJECTED);
}
