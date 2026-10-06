#include "venue_orders.hpp"

#include "transport/rest.hpp"
#include "venue/binance/api_error.hpp"
#include "venue/binance/order.hpp"
#include "venue/binance/rate_limit.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <simdjson.h>

namespace exchange::app {
namespace {

/// Wall-clock milliseconds since the Unix epoch, for the venue's replay window.
[[nodiscard]] std::int64_t venue_now_ms() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(
			   std::chrono::system_clock::now().time_since_epoch())
		.count();
}

/**
 * The client order ids in an @c openOrders response.
 *
 * A small reader rather than a venue-module decoder: the response is an array
 * of order objects and the only field this reads is one string per entry.
 * Anything that needs the rest belongs in `venue/` with a type to put it in.
 * @see venue::binance::parse_execution_report
 */
[[nodiscard]] std::vector<std::string> open_client_ids(std::string_view json) {
	std::vector<std::string> ids;
	try {
		simdjson::ondemand::parser parser;
		simdjson::padded_string padded{json};
		simdjson::ondemand::document doc;
		if (parser.iterate(padded).get(doc) != simdjson::SUCCESS) return ids;

		simdjson::ondemand::array orders;
		if (doc.get_array().get(orders) != simdjson::SUCCESS) return ids;
		for (auto entry : orders) {
			std::string_view id;
			if (entry["clientOrderId"].get_string().get(id) !=
				simdjson::SUCCESS)
				continue;
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

} // namespace

std::optional<std::vector<std::string>>
read_open_orders(const venue_access &access, venue::weight_budget &budget) {
	const auto read = venue::binance::open_orders(access.symbol,
												  access.credential,
												  venue_now_ms(),
												  access.env);
	if (!read) {
		spdlog::error("could not build the request: {}",
					  venue::binance::describe(read.error()));
		return std::nullopt;
	}
	spdlog::debug("GET {} /api/v3/openOrders (query withheld - it is signed)",
				  read->endpoint.host);

	const auto answer =
		call(*read, transport::rest::method::get, access.insecure_tls, budget);
	if (!answer) {
		// The venue's own words where it gave any: "Signature for this request
		// is not valid" beats "HTTP 401" for whoever has to fix it.
		spdlog::error(
			"the venue refused: {}",
			venue::binance::describe_api_error(answer.error().body,
											   answer.error().message()));
		return std::nullopt;
	}
	return open_client_ids(answer->body);
}

void report_open(const std::vector<session::reconciled_order> &found) {
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

bool cancel_ours(const std::vector<session::reconciled_order> &found,
				 const venue_access &access, venue::weight_budget &budget) {
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
		// venue reported, whatever leg it turned out to be.
		const auto request = venue::binance::cancel_order(
			venue::outbound_cancel{.symbol          = access.symbol,
								   .client_order_id = entry.client_order_id},
			access.credential,
			venue_now_ms(),
			access.env);
		if (!request) {
			spdlog::error("  {} - could not build the cancel: {}",
						  entry.client_order_id,
						  venue::binance::describe(request.error()));
			all_gone = false;
			continue;
		}

		const auto answer = call(*request,
								 transport::rest::method::del,
								 access.insecure_tls,
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

} // namespace exchange::app
