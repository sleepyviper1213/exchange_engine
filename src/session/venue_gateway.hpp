#pragma once
// The last thing that runs before an order leaves this process.
//
// Everything upstream has already decided the order is wanted: the strategy
// asked for it, the risk gate passed it, `venue_bridge` expressed it in the
// venue's terms. What is left are the questions only this layer can answer -
// is there rate-limit budget for it, has this session already sent more orders
// than it was allowed, and is the breaker open - and then producing the signed
// request for transport to move.
//
// --- why the sending is not here -------------------------------------------
//
// This class builds a request and accounts for it; a coroutine elsewhere puts
// it on a socket. That split is what makes the interesting half testable: every
// refusal below, every rate-limit decision and the whole weight-budget
// reconciliation can be driven from a test with no io_context, no network and
// no clock. What is left over is a `co_await transport::rest::send(...)`, which
// has no logic in it to get wrong.
//
// --- and why the breaker is borrowed, not reimplemented ---------------------
//
// `risk::circuit_breaker` already distinguishes NORMAL from CANCEL_ONLY from
// HALTED, already counts breaches, and is already the thing an operator's
// console trips. A second kill switch here would be a second thing to find and
// turn off in an incident. This holds a pointer to the one that exists and asks
// it - and asks it *again* at the gateway rather than trusting the gate's
// earlier answer, because an order can sit in a queue across a trip.

#include "orders/order.hpp"
#include "orders/types.hpp"
#include "risk_management/hooks/system/circuit_breaker.hpp"
#include "session/client_order_id.hpp"
#include "session/venue_bridge.hpp"
#include "symbol/symbol_spec.hpp"
#include "transport/rest/request.hpp"
#include "transport/rest/response.hpp"
#include "venue/binance/order.hpp"
#include "venue/binance/rate_limit.hpp"
#include "venue/credentials.hpp"
#include "venue/environment.hpp"
#include "venue/weight_budget.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace exchange::session {

#define VENUE_GATEWAY_REFUSAL_LIST(X)                                          \
	X(no_credentials, "no API key and secret are configured")                  \
	X(breaker_open, "the circuit breaker is not passing this command")         \
	X(order_cap_reached, "this session's order allowance is spent")            \
	X(rate_limited, "sending now would exceed the venue's weight budget")      \
	X(not_expressible, "the order cannot be stated in the venue's terms")   \
	X(below_min_notional, "the order is worth less than the venue accepts")

/**
 * @brief Why the gateway declined to build a request.
 *
 * @note Every one of these is a refusal to *send*, decided locally and before
 *       any byte reaches the venue. A refusal the venue itself made arrives as
 *       an @c api_error in a response body and is a different thing entirely.
 */
enum class gateway_refusal : std::uint8_t {
	EXCHANGE_ENUM_VALUES(VENUE_GATEWAY_REFUSAL_LIST)
};

/// @brief The category message for a @c gateway_refusal.
EXCHANGE_ENUM_LABEL(gateway_refusal, message, VENUE_GATEWAY_REFUSAL_LIST)

#define VENUE_GATEWAY_REFUSAL_COUNT_ONE(name, label) +1
/// @brief How many reasons there are; derived from the list so a new one
///        cannot be added without the per-reason counters growing with it.
inline constexpr std::size_t GATEWAY_REFUSAL_COUNT =
	0 VENUE_GATEWAY_REFUSAL_LIST(VENUE_GATEWAY_REFUSAL_COUNT_ONE);
#undef VENUE_GATEWAY_REFUSAL_COUNT_ONE

#undef VENUE_GATEWAY_REFUSAL_LIST

/// @brief Which of the three requests an @c outbound_request is.
enum class request_kind : std::uint8_t {
	placement,   ///< a new order
	cancel,      ///< a withdrawal
	replacement, ///< a cancel-replace standing in for an amendment
	open_orders, ///< a reconciliation read of what the venue has working
};

/// @brief A request and where to send it - everything transport needs.
struct outbound_request {
	/// @brief TLS host, from the environment's host table.
	std::string host{};

	/// @brief The request itself, signed, with the API key in a header.
	transport::rest::request request{};

	/// @brief Rate-limit weight already debited for it. @see weight_budget
	int weight = 0;

	/**
	 * @brief The engine order this is about.
	 *
	 * Carried so a *refusal* can be attributed. The venue's account stream
	 * names an order in every report it sends, but it sends none at all for an
	 * order it refused outright - a rejected placement never existed, so there
	 * is nothing for it to report. Without this the engine would go on
	 * believing an order is working that the venue declined at the door, and
	 * the only thing that could ever correct it is a reconciliation read.
	 *
	 * @note Not needed on the success path, where the account stream is the
	 *       authority and carries the id itself.
	 */
	order_id_t order_id = 0;

	/// @brief What kind of request this is, which decides what its refusal
	///        means. A refused *cancel* names an order that may have filled,
	///        and reporting it as rejected would retire a live position from
	///        the ledger; a refused *replacement* may have left either leg
	///        working. @see venue::binance::replace_failure
	request_kind kind = request_kind::placement;

	/// @brief Position in the order this router queued requests, from one.
	///        Requests go out one at a time and in this order, so a read's
	///        answer reflects every request with a smaller number and none
	///        with a larger one. @see order_router::on_open_orders
	std::uint64_t sequence = 0;

	[[nodiscard]] bool is_placement() const noexcept {
		return kind == request_kind::placement;
	}
};

/// @brief Bounds a single run may not exceed, whatever the strategy asks for.
struct gateway_limits {
	/**
	 * @brief Orders this session may send in total. Zero means unlimited.
	 *
	 * The blunt instrument, and the point of it is bluntness: a strategy bug
	 * that quotes in a loop is bounded by a number an operator chose, not by
	 * whether the risk gate happened to model that failure. It counts
	 * placements only - a cancel reduces exposure and must never be refused
	 * for want of allowance.
	 */
	std::uint32_t max_orders = 0;

	/**
	 * @brief Weight to leave unspent, as a floor under the venue's budget.
	 *
	 * Order entry competes with market data for one IP allowance, and running
	 * the budget to zero means the next depth resync cannot be fetched. The
	 * reserve is what keeps the feed alive when the order path is busy.
	 */
	int weight_reserve = 0;

	/**
	 * @brief The venue's @c NOTIONAL floor, scaled by price *and* quantity.
	 *
	 * Zero disables the check, which is the honest default: a listing may have
	 * no such filter, and a floor guessed at would refuse orders the venue
	 * would have taken.
	 *
	 * @par Why this scale
	 * An order's value is @c price_scaled multiplied by @c qty_scaled, and a
	 * product of two scaled integers carries the *sum* of their scales. So the
	 * floor has to be read at that same combined scale for the comparison to be
	 * exact integer arithmetic rather than a conversion through a double.
	 * @see venue::binance::symbol_filters::min_notional
	 */
	std::int64_t min_notional_scaled = 0;
};

/// @brief What a gateway has done, for a report at the end of a run.
struct gateway_stats {
	std::uint64_t placed       = 0; ///< placement requests built
	std::uint64_t cancelled    = 0; ///< cancellation requests built
	std::uint64_t replaced     = 0; ///< cancel-replace requests built
	std::uint64_t reads        = 0; ///< open-orders reads built
	std::uint64_t refused      = 0; ///< requests declined before sending
	std::uint64_t weight_spent = 0; ///< weight debited across the run

	/// @brief @c refused, by reason. A testnet run whose summary said only
	///        "2 refused by the gateway" placed nothing and gave no hint that
	///        the reason was the venue's minimum order value.
	std::array<std::uint64_t, GATEWAY_REFUSAL_COUNT> refused_by{};
};

/**
 * @brief Builds signed venue requests, and refuses to build the ones that
 *        should not be sent.
 *
 * @note Single-threaded by construction, like everything else on the producer
 *       side of @c live_session. It holds a @c weight_budget, which is
 *       deliberately not thread-safe: one budget lives with the coroutine that
 *       owns the connection. @see weight_budget
 */
class venue_gateway {
public:
	using clock      = venue::weight_budget::clock;
	using time_point = clock::time_point;

	/**
	 * @param creds The API credential. An incomplete one is not an error here -
	 *        it makes every placement refuse, which is what a run configured
	 *        without one should do.
	 * @param env Which deployment. @see venue::host_for
	 * @param limits This run's own bounds.
	 * @param breaker The kill switch, borrowed. Must outlive this gateway.
	 */
	venue_gateway(venue::credentials creds, venue::environment env,
				  gateway_limits limits,
				  risk::hooks::system::circuit_breaker *breaker) noexcept
		: creds_(std::move(creds)),
		  env_(env),
		  limits_(limits),
		  breaker_(breaker) {}

	/**
	 * @brief Build a signed placement for @p order, debiting what it costs.
	 *
	 * @param order The engine's order, in ticks and lots.
	 * @param spec The listing those are on.
	 * @param venue_symbol The listing as the venue spells it.
	 * @param timestamp_ms Wall-clock milliseconds, for the venue's replay
	 *        window. @see venue::binance::RECV_WINDOW_MS
	 * @param now Steady-clock reading, for the rate-limit window.
	 * @return The request, or why it was declined.
	 *
	 * @note The budget is debited *here*, before sending, rather than on the
	 *       response. A request in flight has already cost its weight, and a
	 *       gateway that only counted answered requests would sail past the
	 *       limit exactly when the venue was slowest to answer.
	 */
	[[nodiscard]] std::expected<outbound_request, gateway_refusal>
	place(const engine::orders::order &order, const engine::symbol_spec &spec,
		  std::string_view venue_symbol, std::int64_t timestamp_ms,
		  time_point now) {
		if (!creds_.is_complete())
			return decline(gateway_refusal::no_credentials);
		// Asked again here, not trusted from the gate: an order can sit in the
		// partition's queue across a trip, and this is the last look.
		if (breaker_ != nullptr && !breaker_->passes_new_orders())
			return decline(gateway_refusal::breaker_open);
		if (is_order_cap_reached())
			return decline(gateway_refusal::order_cap_reached);

		const auto outbound = to_outbound_order(order, spec, venue_symbol);
		if (!outbound) return decline(gateway_refusal::not_expressible);
		if (is_below_notional(*outbound))
			return decline(gateway_refusal::below_min_notional);

		return build(
			venue::binance::place_order(*outbound, creds_, timestamp_ms, env_),
			venue::binance::ORDER_WEIGHT,
			transport::rest::method::post,
			now,
			stats_.placed,
			order.id,
			request_kind::placement);
	}

	/**
	 * @brief Build a signed cancel-replace that moves leg @p from of
	 *        @p replacement's order to leg @p to.
	 *
	 * @param replacement The order as it should now rest: its id, side and
	 *        time in force unchanged, its price the amendment's, and its size
	 *        what should rest *now* - the venue counts the new leg's
	 *        executions from zero. @see order_router
	 * @param from The leg being withdrawn.
	 * @param to The leg the replacement is placed as.
	 *
	 * @note Screened as a placement - the breaker, the order cap, the minimum
	 *       notional - because that is what the venue sees: a new order at a
	 *       new price. Under a cancel-only breaker this refuses, and the
	 *       engine is told so and withdraws the old leg rather than leaving it
	 *       priced where the engine no longer believes it is.
	 */
	[[nodiscard]] std::expected<outbound_request, gateway_refusal>
	replace(const engine::orders::order &replacement, venue_leg_t from,
			venue_leg_t to, const engine::symbol_spec &spec,
			std::string_view venue_symbol, std::int64_t timestamp_ms,
			time_point now) {
		if (!creds_.is_complete())
			return decline(gateway_refusal::no_credentials);
		if (breaker_ != nullptr && !breaker_->passes_new_orders())
			return decline(gateway_refusal::breaker_open);
		if (is_order_cap_reached())
			return decline(gateway_refusal::order_cap_reached);

		auto outbound = to_outbound_order(replacement, spec, venue_symbol);
		if (!outbound) return decline(gateway_refusal::not_expressible);
		if (is_below_notional(*outbound))
			return decline(gateway_refusal::below_min_notional);
		outbound->client_order_id = client_order_id(replacement.id, to);

		return build(venue::binance::cancel_replace_order(
						 venue::outbound_replace{
							 .replacement = std::move(*outbound),
							 .cancel_client_order_id =
								 client_order_id(replacement.id, from)},
						 creds_,
						 timestamp_ms,
						 env_),
					 venue::binance::ORDER_WEIGHT,
					 transport::rest::method::post,
					 now,
					 stats_.replaced,
					 replacement.id,
					 request_kind::replacement);
	}

	/**
	 * @brief Build a signed cancellation of @p id.
	 *
	 * @note Refused only for want of a credential or budget - never for the
	 *       order cap, and only by a @b halted breaker rather than a
	 *       cancel-only one. A cancel reduces exposure, and a gateway that
	 *       refused one because a *placement* allowance was spent would leave
	 *       the position it was trying to close.
	 */
	[[nodiscard]] std::expected<outbound_request, gateway_refusal>
	cancel(order_id_t id, venue_leg_t leg, std::string_view venue_symbol,
		   std::int64_t timestamp_ms, time_point now) {
		if (!creds_.is_complete())
			return decline(gateway_refusal::no_credentials);
		if (breaker_ != nullptr && !breaker_->passes_cancels())
			return decline(gateway_refusal::breaker_open);

		return build(venue::binance::cancel_order(
						 to_outbound_cancel(id, leg, venue_symbol),
						 creds_,
						 timestamp_ms,
						 env_),
					 venue::binance::ORDER_WEIGHT,
					 transport::rest::method::del,
					 now,
					 stats_.cancelled,
					 id,
					 request_kind::cancel);
	}

	/**
	 * @brief Build a signed read of every order working on @p venue_symbol.
	 *
	 * @note Refused only for want of a credential or budget. Never by the
	 *       breaker: an open breaker is when knowing what is working matters
	 *       most.
	 */
	[[nodiscard]] std::expected<outbound_request, gateway_refusal>
	open_orders(std::string_view venue_symbol, std::int64_t timestamp_ms,
				time_point now) {
		if (!creds_.is_complete())
			return decline(gateway_refusal::no_credentials);
		return build(venue::binance::open_orders(venue_symbol,
												 creds_,
												 timestamp_ms,
												 env_),
					 venue::binance::OPEN_ORDERS_WEIGHT,
					 transport::rest::method::get,
					 now,
					 stats_.reads,
					 0,
					 request_kind::open_orders);
	}

	/**
	 * @brief Adopt the venue's own rate-limit count from a response.
	 *
	 * @param headers The response's headers, success or failure - both carry
	 *        it, and a 429 carries it when it matters most.
	 * @param now When the response arrived.
	 *
	 * @note Silently does nothing when the header is absent or unreadable. That
	 *       is deliberate: the local estimate is what stands, and it errs
	 *       towards having spent more rather than less.
	 *       @see venue::binance::parse_used_weight
	 */
	void observe(std::span<const transport::rest::header> headers,
				 time_point now) {
		const auto raw =
			transport::rest::find_header(headers,
										 venue::binance::USED_WEIGHT_HEADER);
		if (!raw) return;
		const auto used = venue::binance::parse_used_weight(*raw);
		if (!used) return;
		budget_.reconcile(*used, now);
	}

	/// @brief Weight still admissible at @p now, above the reserve.
	[[nodiscard]] int spare_weight(time_point now) {
		return budget_.remaining(now) - limits_.weight_reserve;
	}

	[[nodiscard]] const gateway_stats &stats() const noexcept { return stats_; }

	/// @brief The budget, for a caller that needs to debit a request this
	///        gateway did not build - a depth resync, say.
	[[nodiscard]] venue::weight_budget &budget() noexcept { return budget_; }

private:
	/// Whether this session has placed as many orders as it may. A replacement
	/// counts: the venue sees a new order, and a quoter repricing in a loop is
	/// exactly what the cap is for.
	[[nodiscard]] bool is_order_cap_reached() const noexcept {
		return limits_.max_orders != 0 &&
			   stats_.placed + stats_.replaced >= limits_.max_orders;
	}

	/**
	 * Is @p order worth less than the venue will accept?
	 *
	 * The refusal that saves the most for the least: a size can sit exactly on
	 * the tick grid and exactly on the lot grid and still be refused with
	 * @c -1013, because those filters constrain the *increments* and this one
	 * constrains the product. One step of BTCUSDT is under a dollar. Answering
	 * it here costs a multiply; letting the venue answer it costs a round trip,
	 * a rate-limit weight, and an order the engine believes is working.
	 *
	 * A market order is never refused on this ground - it carries no price, so
	 * there is no product to take, and the venue checks it against the book we
	 * do not have. That is the venue's answer to give.
	 */
	[[nodiscard]] bool
	is_below_notional(const venue::outbound_order &order) const noexcept {
		if (limits_.min_notional_scaled <= 0) return false;
		if (order.price_scaled <= 0) return false;
		return order.price_scaled * order.qty_scaled <
			   limits_.min_notional_scaled;
	}

	/// Count the refusal and report it. Every decline goes through here so the
	/// counter cannot drift from the answers actually given.
	[[nodiscard]] std::unexpected<gateway_refusal>
	decline(gateway_refusal why) noexcept {
		++stats_.refused;
		++stats_.refused_by[static_cast<std::size_t>(why)];
		return std::unexpected(why);
	}

	/// Turn an encoded venue request into a transport one, having first
	/// checked there is budget for it.
	template <typename Encoded>
	[[nodiscard]] std::expected<outbound_request, gateway_refusal>
	build(Encoded &&encoded, int weight, transport::rest::method verb,
		  time_point now, std::uint64_t &counter, order_id_t id,
		  request_kind kind) {
		if (!encoded) return decline(gateway_refusal::not_expressible);
		if (budget_.remaining(now) - limits_.weight_reserve < weight)
			return decline(gateway_refusal::rate_limited);

		budget_.spend(weight, now);
		stats_.weight_spent += static_cast<std::uint64_t>(weight);
		++counter;

		return outbound_request{
			.host = encoded->endpoint.host,
			.request =
				transport::rest::request{.verb    = verb,
										 .target  = encoded->endpoint.target,
										 .headers = {transport::rest::header{
											 .name  = "X-MBX-APIKEY",
											 .value = encoded->api_key}}},
			.weight   = weight,
			.order_id = id,
			.kind     = kind};
	}

	venue::credentials creds_;
	venue::environment env_;
	gateway_limits limits_;
	risk::hooks::system::circuit_breaker *breaker_;
	venue::weight_budget budget_{};
	gateway_stats stats_{};
};

} // namespace exchange::session
