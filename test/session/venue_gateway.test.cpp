#include "session/venue_gateway.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

// The last thing that runs before an order leaves the process.
//
// Every case here is a refusal or an accounting rule, and each one exists
// because the alternative sends something. A test that only checked the happy
// path would pass on a gateway that never refused anything at all.

using exchange::order_id_t;
using exchange::side_t;
using exchange::engine::symbol_spec;
using exchange::engine::orders::order;
using exchange::engine::orders::order_type;
using exchange::engine::orders::time_in_force_instruction;
using exchange::risk::hooks::system::circuit_breaker;
using exchange::risk::hooks::system::trading_state;
using exchange::session::gateway_limits;
using exchange::session::gateway_refusal;
using exchange::session::venue_gateway;
using exchange::transport::rest::header;
using exchange::transport::rest::method;
using exchange::venue::credentials;
using exchange::venue::environment;

namespace {

using gateway_clock = venue_gateway::clock;

/// A fixed origin, so the rate-limit window reads in seconds from a known zero.
const gateway_clock::time_point GATEWAY_EPOCH{};

[[nodiscard]] gateway_clock::time_point gateway_at(int seconds) {
	return GATEWAY_EPOCH + std::chrono::seconds{seconds};
}

constexpr std::int64_t GATEWAY_TIME_MS = 1'499'827'319'559;

[[nodiscard]] credentials gateway_credentials() {
	return credentials{.key = "test-key", .secret = "test-secret"};
}

/// SOLUSDT's grid: two decimals of price, three of size.
[[nodiscard]] symbol_spec gateway_listing() {
	return symbol_spec{7, "SOLUSDT", 2, 3, 1, 1, 15345, symbol_spec::NO_COLLAR};
}

[[nodiscard]] order gateway_order(order_id_t id = 42) {
	order o{};
	o.id    = id;
	o.side  = side_t::bid;
	o.type  = order_type::LIMIT;
	o.tif   = time_in_force_instruction::GOOD_TILL_CANCELLED;
	o.price = 15345;
	o.qty   = 1500;
	return o;
}

/// A gateway with no breaker and no limits - the baseline every case narrows.
[[nodiscard]] venue_gateway gateway_open(circuit_breaker *breaker = nullptr,
										 gateway_limits limits    = {}) {
	return venue_gateway{gateway_credentials(),
						 environment::testnet,
						 limits,
						 breaker};
}

} // namespace

TEST(VenueGateway, APlacementIsSignedAndAddressedToTheEnvironmentsHost) {
	venue_gateway gateway = gateway_open();

	const auto sent = gateway.place(gateway_order(),
									gateway_listing(),
									"SOLUSDT",
									GATEWAY_TIME_MS,
									gateway_at(0));
	ASSERT_TRUE(sent.has_value()) << message(sent.error());

	EXPECT_EQ(sent->host, "testnet.binance.vision");
	EXPECT_EQ(sent->request.verb, method::post);
	EXPECT_TRUE(sent->request.target.contains("signature="))
		<< sent->request.target;
	EXPECT_TRUE(sent->request.target.contains("newClientOrderId=ex-42"));
}

TEST(VenueGateway, TheApiKeyTravelsInAHeaderAndTheSecretNowhere) {
	venue_gateway gateway = gateway_open();

	const auto sent = gateway.place(gateway_order(),
									gateway_listing(),
									"SOLUSDT",
									GATEWAY_TIME_MS,
									gateway_at(0));
	ASSERT_TRUE(sent.has_value());

	ASSERT_EQ(sent->request.headers.size(), 1U);
	EXPECT_EQ(sent->request.headers[0].name, "X-MBX-APIKEY");
	EXPECT_EQ(sent->request.headers[0].value, "test-key");
	// A key in the URL lands in every proxy log between here and the venue.
	EXPECT_FALSE(sent->request.target.contains("test-key"));
	EXPECT_FALSE(sent->request.target.contains("test-secret"));
}

TEST(VenueGateway, ARunWithNoCredentialSendsNothing) {
	venue_gateway gateway{credentials{}, environment::testnet, {}, nullptr};

	EXPECT_EQ(gateway
				  .place(gateway_order(),
						 gateway_listing(),
						 "SOLUSDT",
						 GATEWAY_TIME_MS,
						 gateway_at(0))
				  .error(),
			  gateway_refusal::no_credentials);
	EXPECT_EQ(gateway.stats().refused, 1U);
	EXPECT_EQ(gateway.stats().placed, 0U);
}

TEST(VenueGateway, TheOrderCapBoundsARunWhateverTheStrategyAsks) {
	// The blunt instrument: a strategy bug that quotes in a loop is stopped by
	// a number an operator chose, not by whether the risk gate modelled it.
	venue_gateway gateway =
		gateway_open(nullptr, gateway_limits{.max_orders = 2});

	EXPECT_TRUE(gateway
					.place(gateway_order(1),
						   gateway_listing(),
						   "SOLUSDT",
						   GATEWAY_TIME_MS,
						   gateway_at(0))
					.has_value());
	EXPECT_TRUE(gateway
					.place(gateway_order(2),
						   gateway_listing(),
						   "SOLUSDT",
						   GATEWAY_TIME_MS,
						   gateway_at(0))
					.has_value());
	EXPECT_EQ(gateway
				  .place(gateway_order(3),
						 gateway_listing(),
						 "SOLUSDT",
						 GATEWAY_TIME_MS,
						 gateway_at(0))
				  .error(),
			  gateway_refusal::order_cap_reached);
	EXPECT_EQ(gateway.stats().placed, 2U);
}

TEST(VenueGateway, TheOrderCapNeverRefusesACancel) {
	// A cancel reduces exposure. Refusing one because a *placement* allowance
	// was spent would leave the position it was trying to close.
	venue_gateway gateway =
		gateway_open(nullptr, gateway_limits{.max_orders = 1});

	ASSERT_TRUE(gateway
					.place(gateway_order(1),
						   gateway_listing(),
						   "SOLUSDT",
						   GATEWAY_TIME_MS,
						   gateway_at(0))
					.has_value());
	EXPECT_TRUE(gateway.cancel(1, "SOLUSDT", GATEWAY_TIME_MS, gateway_at(0))
					.has_value());
	EXPECT_TRUE(gateway.cancel(2, "SOLUSDT", GATEWAY_TIME_MS, gateway_at(0))
					.has_value());
}

TEST(VenueGateway, ACancelOnlyBreakerStopsPlacementsAndPassesCancels) {
	circuit_breaker breaker;
	breaker.trip(trading_state::CANCEL_ONLY);
	venue_gateway gateway = gateway_open(&breaker);

	EXPECT_EQ(gateway
				  .place(gateway_order(),
						 gateway_listing(),
						 "SOLUSDT",
						 GATEWAY_TIME_MS,
						 gateway_at(0))
				  .error(),
			  gateway_refusal::breaker_open);
	// The whole point of CANCEL_ONLY: risk-reducing commands still go.
	EXPECT_TRUE(gateway.cancel(42, "SOLUSDT", GATEWAY_TIME_MS, gateway_at(0))
					.has_value());
}

TEST(VenueGateway, AHaltedBreakerStopsCancelsToo) {
	circuit_breaker breaker;
	breaker.trip(trading_state::HALTED);
	venue_gateway gateway = gateway_open(&breaker);

	EXPECT_EQ(
		gateway.cancel(42, "SOLUSDT", GATEWAY_TIME_MS, gateway_at(0)).error(),
		gateway_refusal::breaker_open);
}

TEST(VenueGateway, TheBreakerIsAskedAgainAtTheGateway) {
	// An order can sit in the partition's queue across a trip. Asking once, at
	// the gate, would send it anyway - which is what this checks does not
	// happen: the same gateway answers differently after the breaker moves.
	circuit_breaker breaker;
	venue_gateway gateway = gateway_open(&breaker);

	ASSERT_TRUE(gateway
					.place(gateway_order(1),
						   gateway_listing(),
						   "SOLUSDT",
						   GATEWAY_TIME_MS,
						   gateway_at(0))
					.has_value());
	breaker.trip(trading_state::HALTED);
	EXPECT_EQ(gateway
				  .place(gateway_order(2),
						 gateway_listing(),
						 "SOLUSDT",
						 GATEWAY_TIME_MS,
						 gateway_at(0))
				  .error(),
			  gateway_refusal::breaker_open);
}

TEST(VenueGateway, WeightIsDebitedWhenTheRequestIsBuiltAndNotWhenAnswered) {
	// A request in flight has already cost its weight. Counting only answered
	// ones would sail past the limit exactly when the venue was slowest.
	venue_gateway gateway = gateway_open();
	const int before      = gateway.spare_weight(gateway_at(0));

	ASSERT_TRUE(gateway
					.place(gateway_order(),
						   gateway_listing(),
						   "SOLUSDT",
						   GATEWAY_TIME_MS,
						   gateway_at(0))
					.has_value());

	EXPECT_EQ(gateway.spare_weight(gateway_at(0)),
			  before - exchange::venue::binance::ORDER_WEIGHT);
	EXPECT_EQ(
		gateway.stats().weight_spent,
		static_cast<std::uint64_t>(exchange::venue::binance::ORDER_WEIGHT));
}

TEST(VenueGateway, TheReserveKeepsBudgetBackForTheRestOfTheProcess) {
	// Order entry and market data share one IP allowance. Running it to zero
	// means the next depth resync cannot be fetched, and the feed dies while
	// the order path is busy.
	venue_gateway gateway = gateway_open(
		nullptr,
		gateway_limits{.weight_reserve =
						   exchange::venue::BINANCE_SPOT_WEIGHT_PER_MINUTE});

	EXPECT_EQ(gateway
				  .place(gateway_order(),
						 gateway_listing(),
						 "SOLUSDT",
						 GATEWAY_TIME_MS,
						 gateway_at(0))
				  .error(),
			  gateway_refusal::rate_limited);
	EXPECT_LE(gateway.spare_weight(gateway_at(0)), 0);
}

TEST(VenueGateway, TheVenuesOwnWeightCountIsAdoptedOverOurEstimate) {
	venue_gateway gateway = gateway_open();
	ASSERT_TRUE(gateway
					.place(gateway_order(),
						   gateway_listing(),
						   "SOLUSDT",
						   GATEWAY_TIME_MS,
						   gateway_at(0))
					.has_value());

	// The venue says far more has been spent than we counted - a retry, or
	// another process on this address. Its number wins.
	const std::vector<header> headers{
		header{.name = "x-mbx-used-weight-1m", .value = "5900"}};
	gateway.observe(headers, gateway_at(1));

	EXPECT_EQ(gateway.spare_weight(gateway_at(1)),
			  exchange::venue::BINANCE_SPOT_WEIGHT_PER_MINUTE - 5900);
}

TEST(VenueGateway, AResponseWithNoUsableWeightHeaderLeavesTheEstimate) {
	venue_gateway gateway = gateway_open();
	ASSERT_TRUE(gateway
					.place(gateway_order(),
						   gateway_listing(),
						   "SOLUSDT",
						   GATEWAY_TIME_MS,
						   gateway_at(0))
					.has_value());
	const int estimate = gateway.spare_weight(gateway_at(0));

	// Absent, then present but unreadable. Neither is evidence about the
	// budget, and adopting a zero would licence a full minute of traffic.
	gateway.observe(std::vector<header>{}, gateway_at(0));
	EXPECT_EQ(gateway.spare_weight(gateway_at(0)), estimate);

	const std::vector<header> nonsense{
		header{.name = "X-MBX-USED-WEIGHT-1M", .value = "lots"}};
	gateway.observe(nonsense, gateway_at(0));
	EXPECT_EQ(gateway.spare_weight(gateway_at(0)), estimate);
}

TEST(VenueGateway, ARateLimitedGatewayRecoversAsTheWindowRolls) {
	// One weight of budget: the first order spends it, the second is refused,
	// and a minute later the window has rolled and the budget is back.
	venue_gateway gateway = gateway_open(
		nullptr,
		gateway_limits{.weight_reserve =
						   exchange::venue::BINANCE_SPOT_WEIGHT_PER_MINUTE -
						   1});

	ASSERT_TRUE(gateway
					.place(gateway_order(1),
						   gateway_listing(),
						   "SOLUSDT",
						   GATEWAY_TIME_MS,
						   gateway_at(0))
					.has_value());
	EXPECT_EQ(gateway
				  .place(gateway_order(2),
						 gateway_listing(),
						 "SOLUSDT",
						 GATEWAY_TIME_MS,
						 gateway_at(0))
				  .error(),
			  gateway_refusal::rate_limited);

	EXPECT_TRUE(gateway
					.place(gateway_order(3),
						   gateway_listing(),
						   "SOLUSDT",
						   GATEWAY_TIME_MS,
						   gateway_at(60))
					.has_value());
}

TEST(VenueGateway, ACancellationIsADeleteNamingOurOwnIdentifier) {
	venue_gateway gateway = gateway_open();

	const auto sent =
		gateway.cancel(42, "SOLUSDT", GATEWAY_TIME_MS, gateway_at(0));
	ASSERT_TRUE(sent.has_value()) << message(sent.error());

	EXPECT_EQ(sent->request.verb, method::del);
	// origClientOrderId, so a cancel works before the placement's ack returns.
	EXPECT_TRUE(sent->request.target.contains("origClientOrderId=ex-42"))
		<< sent->request.target;
	EXPECT_EQ(gateway.stats().cancelled, 1U);
}

TEST(VenueGateway, EveryRefusalIsCounted) {
	circuit_breaker breaker;
	breaker.trip(trading_state::HALTED);
	venue_gateway gateway = gateway_open(&breaker);

	(void)gateway.place(gateway_order(),
						gateway_listing(),
						"SOLUSDT",
						GATEWAY_TIME_MS,
						gateway_at(0));
	(void)gateway.cancel(42, "SOLUSDT", GATEWAY_TIME_MS, gateway_at(0));

	// A run that sent nothing and a run that was stopped from sending look
	// identical without this.
	EXPECT_EQ(gateway.stats().refused, 2U);
	EXPECT_EQ(gateway.stats().placed, 0U);
	EXPECT_EQ(gateway.stats().weight_spent, 0U);
}

TEST(SessionVenueGateway, RefusesAnOrderWorthLessThanTheVenueAccepts) {
	circuit_breaker breaker;
	// SOLUSDT's grid is 2 decimals of price and 3 of size, so a product carries
	// 5 - and a floor of 5.00 quote units is 500000 at that combined scale.
	venue_gateway gateway(gateway_credentials(),
						  environment::testnet,
						  gateway_limits{.min_notional_scaled = 500'000},
						  &breaker);
	const symbol_spec spec = gateway_listing();

	// 15345 ticks at scale 2 is 153.45; one lot at scale 3 is 0.001. The order
	// is worth about fifteen cents - exactly on the tick grid, exactly on the
	// lot grid, and refused by the venue with -1013. Both other filters pass,
	// which is what makes this one impossible to infer from them.
	order tiny = gateway_order();
	tiny.qty   = 1;
	EXPECT_EQ(gateway
				  .place(tiny, spec, "SOLUSDT", GATEWAY_TIME_MS, gateway_at(0))
				  .error(),
			  gateway_refusal::below_min_notional);

	// And it costs nothing to find out: the refusal happens before a request is
	// built, so no weight is spent and no round trip is paid.
	EXPECT_EQ(gateway.stats().placed, 0U);
	EXPECT_EQ(gateway.stats().weight_spent, 0U);

	// 1500 lots is 1.5 at scale 3, so 153.45 * 1.5 = 230.175 - comfortably over.
	EXPECT_TRUE(gateway
					.place(gateway_order(), spec, "SOLUSDT", GATEWAY_TIME_MS,
						   gateway_at(0))
					.has_value());
}

TEST(SessionVenueGateway, AListingWithNoNotionalFloorIsNotSecondGuessed) {
	circuit_breaker breaker;
	// Zero disables it, which is what an absent filter and an unreachable venue
	// both mean. Guessing a floor would refuse orders the venue would take.
	venue_gateway gateway(gateway_credentials(),
						  environment::testnet,
						  gateway_limits{},
						  &breaker);
	order tiny = gateway_order();
	tiny.qty   = 1;
	EXPECT_TRUE(gateway
					.place(tiny, gateway_listing(), "SOLUSDT", GATEWAY_TIME_MS,
						   gateway_at(0))
					.has_value());
}
