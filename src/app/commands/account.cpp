#include "account.hpp"

#include "app/credentials_option.hpp"
#include "session/venue_bridge.hpp"
#include "transport/rest.hpp"
#include "venue/binance/api_error.hpp"
#include "venue/binance/order.hpp"
#include "venue/binance/rate_limit.hpp"
#include "venue/weight_budget.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdlib>
#include <simdjson.h>
#include <string>
#include <vector>

namespace exchange::app {
namespace {

/// @brief The venue's code for "no such order" - it filled, or was already
///        cancelled, between the read and the cancel.
///
/// A documented, stable code, which is why matching on it is safe where
/// matching on the message text would not be. For a cancel-all this is not a
/// failure: the order is gone, which is what was asked for.

/// Wall-clock milliseconds since the Unix epoch, for the venue's replay window.
[[nodiscard]] std::int64_t trade_now_ms() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(
			   std::chrono::system_clock::now().time_since_epoch())
		.count();
}

/**
 * The client order ids in an @c openOrders response.
 *
 * A small reader rather than a venue-module decoder: the response is an array
 * of order objects and the only field this command reads is one string per
 * entry. Anything that needs the rest belongs in `venue/` with a type to put it
 * in. @see venue::binance::parse_execution_report
 */
[[nodiscard]] std::vector<std::string> open_client_ids(std::string_view json) {
	std::vector<std::string> ids;
	try {
		simdjson::ondemand::parser parser;
		simdjson::padded_string padded{json};
		simdjson::ondemand::document doc;
		if (parser.iterate(padded).get(doc)) return ids;

		simdjson::ondemand::array orders;
		if (doc.get_array().get(orders)) return ids;
		for (auto entry : orders) {
			std::string_view id;
			if (entry["clientOrderId"].get_string().get(id)) continue;
			ids.emplace_back(id);
		}
	} catch (...) {
		// A body we could not read is reported as no orders, and the caller
		// says so. Guessing at a partial list would be worse: reconciliation
		// would call every order it did not see `presumed_gone`.
		ids.clear();
	}
	return ids;
}

/// One signed call, with the venue's own rate-limit count adopted from its
/// answer - whichever way the answer went. A refusal states the count in the
/// same header a success does, and that is when it matters most.
[[nodiscard]] transport::rest::response
call(const venue::binance::signed_request &signed_request,
	 transport::rest::method verb, bool insecure_tls,
	 venue::weight_budget &budget) {
	budget.spend(signed_request.weight, venue::weight_budget::clock::now());

	auto answer = transport::rest::send(
		signed_request.endpoint.host,
		transport::rest::request{.verb    = verb,
								 .target  = signed_request.endpoint.target,
								 .headers = {transport::rest::header{
									 .name  = "X-MBX-APIKEY",
									 .value = signed_request.api_key}}},
		transport::rest::request_options{
			.verify = insecure_tls ? transport::tls_verify::none
								   : transport::tls_verify::peer});

	const auto &headers = answer ? answer->headers : answer.error().headers;
	if (const auto raw =
			transport::rest::find_header(headers,
										 venue::binance::USED_WEIGHT_HEADER))
		if (const auto used = venue::binance::parse_used_weight(*raw))
			budget.reconcile(*used, venue::weight_budget::clock::now());

	return answer;
}

/// One line per open order, with what reconciling it concluded.
void report(const std::vector<session::reconciled_order> &found) {
	for (const auto &entry : found) {
		if (entry.finding == session::reconciliation::foreign) {
			spdlog::warn("  {} - not placed by this engine; left alone",
						 entry.client_order_id);
			continue;
		}
		spdlog::info("  {} - engine order {}, working at the venue",
					 entry.client_order_id,
					 entry.id);
	}
}

/**
 * Withdraw every working order this engine placed.
 *
 * @return Whether every one of ours is now gone.
 *
 * @par Why one request per order rather than the venue's bulk cancel
 * Binance has @c DELETE @c /api/v3/openOrders, which withdraws every order on a
 * symbol in a single call of one weight. It is not usable here, and the reason
 * is the whole point of the client-order-id prefix: that endpoint cancels
 * orders this process did not place, including ones a human placed by hand in
 * the venue's web UI while this was running. Paying one weight per order buys
 * the ability to leave those alone.
 */
[[nodiscard]] bool
cancel_ours(const std::vector<session::reconciled_order> &found,
			const account_settings &settings, venue::weight_budget &budget) {
	bool all_gone = true;
	for (const auto &entry : found) {
		if (entry.finding == session::reconciliation::foreign) continue;

		// Asked before each one rather than once for the batch: a long list can
		// exhaust the allowance part way, and stopping with a count is better
		// than earning a 418 on the orders that remain.
		if (!budget.can_spend(venue::binance::ORDER_WEIGHT,
							  venue::weight_budget::clock::now())) {
			spdlog::error("out of rate-limit budget with orders still working; "
						  "re-run in a minute");
			return false;
		}

		// The venue's own spelling of the id, verbatim rather than rebuilt from
		// the parsed engine id - what is being cancelled is the string the
		// venue reported, whatever shape it turned out to have.
		const auto request = venue::binance::cancel_order(
			venue::outbound_cancel{.symbol          = settings.symbol,
								   .client_order_id = entry.client_order_id},
			settings.credential,
			trade_now_ms(),
			settings.env);
		if (!request) {
			spdlog::error("  {} - could not build the cancel: {}",
						  entry.client_order_id,
						  venue::binance::describe(request.error()));
			all_gone = false;
			continue;
		}

		const auto answer = call(*request,
								 transport::rest::method::del,
								 settings.insecure_tls,
								 budget);
		if (answer) {
			spdlog::info("  {} - cancelled", entry.client_order_id);
			continue;
		}

		// Gone already is the outcome asked for, not a failure: the order can
		// fill between the read and the cancel, and that race is ordinary.
		const auto refusal =
			venue::binance::parse_api_error(answer.error().body);
		if (refusal && refusal->code == venue::binance::UNKNOWN_ORDER) {
			spdlog::info("  {} - already gone (filled or cancelled since the "
						 "read)",
						 entry.client_order_id);
			continue;
		}

		spdlog::error(
			"  {} - the venue refused the cancel: {}",
			entry.client_order_id,
			venue::binance::describe_api_error(answer.error().body,
											   answer.error().message()));
		all_gone = false;
	}
	return all_gone;
}

} // namespace

int cmd_account(const account_settings &settings) {
	spdlog::info("{}", describe(settings.credential));
	if (!settings.credential.is_complete()) {
		spdlog::error(
			"set {} and {} in the environment; they are read from there and "
			"from nowhere else",
			venue::API_KEY_VAR,
			venue::API_SECRET_VAR);
		return EXIT_FAILURE;
	}

	// Said every time, not only for production: an operator reading a log a
	// week later needs to know which account these orders were on - and the
	// environment is *named*, never described by a branch. A two-way message
	// over a three-way enum reported `demo` as "the testnet sandbox", which is
	// the one thing a log line about which account this is must not do.
	switch (settings.env) {
	case venue::environment::production:
		spdlog::warn("talking to PRODUCTION - these are real orders on a real "
					 "account");
		break;
	case venue::environment::testnet:
		spdlog::info("talking to {}: its own book, its own thin liquidity",
					 to_string(settings.env));
		break;
	case venue::environment::demo:
		spdlog::info("talking to {}: fake balances against depth that tracks "
					 "the live exchange",
					 to_string(settings.env));
		break;
	}

	const auto read = venue::binance::open_orders(settings.symbol,
												  settings.credential,
												  trade_now_ms(),
												  settings.env);
	if (!read) {
		spdlog::error("could not build the request: {}",
					  venue::binance::describe(read.error()));
		return EXIT_FAILURE;
	}

	venue::weight_budget budget;
	spdlog::debug("GET {} /api/v3/openOrders (query withheld - it is signed)",
				  read->endpoint.host);

	const auto answer = call(*read,
							 transport::rest::method::get,
							 settings.insecure_tls,
							 budget);
	if (!answer) {
		// The venue's own words where it gave any: "Signature for this request
		// is not valid" beats "HTTP 401" for whoever has to fix it.
		spdlog::error(
			"the venue refused: {}",
			venue::binance::describe_api_error(answer.error().body,
											   answer.error().message()));
		return EXIT_FAILURE;
	}

	const std::vector<std::string> open = open_client_ids(answer->body);
	spdlog::info("authenticated; {} order(s) working on {}",
				 open.size(),
				 settings.symbol);

	// Nothing is believed working locally: this process placed none of these,
	// it has just started. So every one of ours reads as `adopt`, which is the
	// honest finding - they are orders a previous run left behind.
	const auto found = session::reconcile({}, open);
	report(found);

	bool ok = true;
	if (settings.cancel_all && !open.empty()) {
		spdlog::warn("withdrawing every order this engine placed; anything it "
					 "did not place is left alone");
		ok = cancel_ours(found, settings, budget);
	}

	spdlog::info("rate limit: {} of {} weight left this minute",
				 budget.remaining(venue::weight_budget::clock::now()),
				 budget.limit());
	return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace exchange::app
