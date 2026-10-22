// The order shipper: draining the session's outbox over one pipelined
// connection, and booking what the venue answered.

#include "serve_internal.hpp"

#include "core/concurrency/affinity/format.hpp" // IWYU pragma: keep - fmt::formatter<topology>
#include "core/metrics/format.hpp" // IWYU pragma: keep - fmt::formatter<registry>
#include "format.hpp" // IWYU pragma: keep - fmt::formatter<startup>, <shutdown>
#include "market_data/format.hpp" // IWYU pragma: keep - fmt::formatter<depth_speed>
#include "session/live_feed.hpp"
#include "session/live_session.hpp"
#include "session/venue_gateway.hpp"
#include "symbol/symbol_spec.hpp"
#include "transport/rest/pipeline.hpp"
#include "venue/binance/api_error.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <fmt/ranges.h>
#include <fmt/std.h> // IWYU pragma: keep - fmt::formatter<std::filesystem::path>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace exchange::engine;

namespace exchange::app::serve_detail {

namespace {

/**
 * @brief Apply the answer to one cancel-replace.
 *
 * Its own function because its refusal means something different from either
 * of the others'. A refused placement never became an order and a refused
 * cancel names one that may have filled - but a refused *replacement* may have
 * left the old leg working, left nothing working, or done nothing at all, and
 * the router has to know which before it can name the right leg in whatever it
 * held back while waiting. @see order_router::on_replace_answered
 */
void record_replace_answer(serving_session *run,
						   const session::outbound_request &sent_for,
						   const transport::rest::response &answer,
						   shipper_stats *stats) {
	using session::replace_answer;
	using venue::binance::replace_failure;

	if (answer) {
		++stats->replaced;
		run->on_replace_answered(sent_for.order_id, replace_answer::replaced);
		return;
	}
	const transport::rest::failure &why = answer.error();
	if (why.status == 0) {
		++stats->failed;
		spdlog::error(
			"cancel-replace of order {} not answered: {} - either leg "
			"may be working; the exit sweep will find out",
			sent_for.order_id,
			why.message());
		run->on_replace_answered(sent_for.order_id, replace_answer::unanswered);
		return;
	}

	switch (venue::binance::classify_replace_failure(why.body)) {
	case replace_failure::unchanged:
		++stats->replace_unchanged;
		spdlog::warn(
			"the venue did not reprice order {}, which stays where it "
			"was: {}",
			sent_for.order_id,
			venue::binance::describe_api_error(why.body, why.message()));
		run->on_replace_answered(sent_for.order_id, replace_answer::unchanged);
		return;
	case replace_failure::order_gone:
		// The replace's own "cancel too late": the order filled before the
		// reprice landed, and the fill is on its way on the account stream.
		++stats->replace_gone;
		spdlog::debug("order {} was gone before its reprice landed",
					  sent_for.order_id);
		run->on_replace_answered(sent_for.order_id, replace_answer::order_gone);
		return;
	case replace_failure::withdrawn:
		++stats->replace_withdrawn;
		spdlog::warn(
			"order {} was withdrawn but its replacement refused: {}",
			sent_for.order_id,
			venue::binance::describe_api_error(why.body, why.message()));
		run->on_replace_answered(sent_for.order_id, replace_answer::withdrawn);
		return;
	}
}

} // namespace

void record_answers(serving_session *run,
					std::span<const session::outbound_request> batch,
					std::span<const transport::rest::response> answers,
					shipper_stats *stats) {
	for (std::size_t i = 0; i < answers.size(); ++i) {
		const transport::rest::response &answer = answers[i];
		const session::outbound_request &sent_for =
			batch[std::min(i, batch.size() - 1)];
		const auto now = venue_gateway::clock::now();

		if (sent_for.kind == session::request_kind::trade_history) {
			run->observe_venue(answer ? answer->headers
									  : answer.error().headers,
							   now);
			if (answer) {
				++stats->accepted;
				const auto trades =
					venue::binance::parse_my_trades(answer->body);
				std::optional<std::vector<venue::execution_report>> reports;
				if (trades) {
					reports.emplace();
					reports->reserve(trades->size());
					for (const venue::binance::venue_trade &trade : *trades)
						reports->push_back(venue::binance::to_execution_report(
							trade,
							run->spec().price_scale(),
							run->spec().qty_scale()));
				}
				const auto before = run->report().fills_recovered;
				run->on_trade_history(reports);
				// Said whichever way it went: after an incident, "the history
				// was read and nothing was missing" is the line an operator
				// looks for, and silence cannot say it.
				spdlog::info("after the account-stream gap: {} trade(s) in the "
							 "history since the last one booked, {} recovered",
							 trades ? trades->size() : 0,
							 run->report().fills_recovered - before);
				continue;
			}
			if (answer.error().status == 0) ++stats->failed;
			else ++stats->refused;
			spdlog::warn(
				"trade-history read failed: {} - fills lost in the "
				"gap, if any, are missing from the position",
				venue::binance::describe_api_error(answer.error().body,
												   answer.error().message()));
			run->on_trade_history(std::nullopt);
			continue;
		}

		if (sent_for.kind == session::request_kind::open_orders) {
			run->observe_venue(answer ? answer->headers
									  : answer.error().headers,
							   now);
			if (answer) {
				++stats->accepted;
				run->on_open_orders(
					venue::binance::parse_open_order_ids(answer->body));
				continue;
			}
			if (answer.error().status == 0) ++stats->failed;
			else ++stats->refused;
			spdlog::warn(
				"reconciliation read failed: {}",
				venue::binance::describe_api_error(answer.error().body,
												   answer.error().message()));
			run->on_open_orders(std::nullopt);
			continue;
		}

		if (sent_for.kind == session::request_kind::replacement) {
			run->observe_venue(answer ? answer->headers
									  : answer.error().headers,
							   now);
			if (answer) ++stats->accepted;
			else if (answer.error().status != 0) ++stats->refused;
			record_replace_answer(run, sent_for, answer, stats);
			continue;
		}

		if (answer) {
			++stats->accepted;
			// The venue's own running total, adopted over our estimate.
			// Carried on every response, which is why it is read here rather
			// than only on a refusal. @see venue_gateway::observe
			run->observe_venue(answer->headers, now);
			continue;
		}

		const transport::rest::failure &why = answer.error();
		run->observe_venue(why.headers, now);
		if (why.status == 0) {
			++stats->failed;
			// Written and never answered. Not re-sent, and that is
			// `may_resend`'s rule rather than a shortcut: a placement in this
			// state may or may not be resting at the venue, and the only
			// honest way to find out is to ask what is working.
			// @see session::reconcile
			spdlog::error("order not answered: {} - it may or may not have "
						  "reached the venue",
						  why.message());
			continue;
		}

		// A cancel the venue answers with "unknown order" is not a refusal.
		// The order filled between the quoter deciding to withdraw it and the
		// request landing, which on a fast listing is the ordinary case - and
		// the outcome asked for, the order no longer working, is exactly what
		// happened. Counted apart from the failures and logged at debug,
		// because a run that fills briskly would otherwise bury its real
		// problems under warnings about the market working.
		// @see venue::binance::UNKNOWN_ORDER, and cmd_account, which draws the
		// same line for the same reason.
		const auto refusal = venue::binance::parse_api_error(why.body);
		const bool already_gone =
			!sent_for.is_placement() && refusal &&
			refusal->code == venue::binance::UNKNOWN_ORDER;
		if (already_gone) {
			++stats->cancels_too_late;
			spdlog::debug("cancel arrived after the order was gone: {}",
						  sent_for.order_id);
			continue;
		}

		++stats->refused;
		spdlog::warn(
			"the venue refused an order: {}",
			venue::binance::describe_api_error(why.body, why.message()));
		// The engine has to be told, because nothing else will: a placement
		// the venue refused never became an order, so the account stream has
		// no lifecycle to report for it. Left unsaid, the gate goes on
		// screening every later size against exposure that does not exist.
		//
		// Placements only, and the exclusion is load-bearing: a refused cancel
		// names an order that may have *filled*, and reporting that as
		// rejected would retire a live position from the gate's ledger.
		// @see live_session::on_send_refused
		if (sent_for.is_placement() && sent_for.order_id != 0)
			run->on_send_refused(sent_for.order_id);
		if (why.is_ip_banned()) {
			// Every subsequent request fails for as long as the ban lasts, so
			// quoting into it writes orders nobody will take while the engine
			// records them as working. HALTED rather than CANCEL_ONLY: a
			// cancel would be refused too.
			spdlog::error("this address is banned by the venue; halting order "
						  "entry");
			run->breaker().trip(risk::hooks::system::trading_state::HALTED,
								risk::hooks::system::trip_cause::OPERATOR);
		}
	}
}

asio::awaitable<void> ship_orders(serving_session *run, std::string host,
								  transport::tls_verify verify,
								  shipper_stats *stats) {
	/// Long enough that an idle order path is not a spin, short enough to be
	/// lost in the round trip that follows it.
	static constexpr auto IDLE_TICK = std::chrono::milliseconds{1};

	transport::rest::request_pipeline wire(
		std::move(host),
		// Verified, and not by default: `pipeline_options` defaults to
		// unverified because it was written for market-data fan-out. Every
		// request this loop sends carries an API key in a header.
		transport::rest::pipeline_options{.window = 1, .verify = verify});
	asio::steady_timer timer(co_await asio::this_coro::executor);

	std::vector<transport::rest::request> writing;
	for (;;) {
		if (!run->has_outbound()) {
			timer.expires_after(IDLE_TICK);
			auto [error] = co_await timer.async_wait(session::detail::TOKEN);
			if (error) break; // the context stopped
			continue;
		}

		std::vector<session::outbound_request> batch = run->take_outbound();
		writing.clear();
		writing.reserve(batch.size());
		for (session::outbound_request &queued : batch)
			writing.push_back(std::move(queued.request));

		const std::vector<transport::rest::response> answers =
			co_await wire.send(writing);
		stats->sent += writing.size();

		record_answers(run, batch, answers, stats);
	}
	co_await wire.close();
}

asio::awaitable<void> send_withdrawals(serving_session *run, std::string host,
									   transport::tls_verify verify,
									   shipper_stats *stats) {
	if (!run->has_outbound()) co_return;

	transport::rest::request_pipeline wire(
		std::move(host),
		transport::rest::pipeline_options{.window = 1, .verify = verify});

	const std::vector<session::outbound_request> batch = run->take_outbound();
	std::vector<transport::rest::request> writing;
	writing.reserve(batch.size());
	for (const session::outbound_request &queued : batch)
		writing.push_back(queued.request);

	const std::vector<transport::rest::response> answers =
		co_await wire.send(writing);
	stats->sent += writing.size();
	record_answers(run, batch, answers, stats);
	co_await wire.close();
}

} // namespace exchange::app::serve_detail
