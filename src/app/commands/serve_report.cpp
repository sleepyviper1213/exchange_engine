// What an operator reads: progress while the run is up, and the report
// when it ends.

#include "serve_internal.hpp"

#include "core/concurrency/affinity/format.hpp" // IWYU pragma: keep - fmt::formatter<topology>
#include "core/logging.hpp"
#include "core/metrics/format.hpp" // IWYU pragma: keep - fmt::formatter<registry>
#include "core/scaled/decimal.hpp"
#include "format.hpp" // IWYU pragma: keep - fmt::formatter<startup>, <shutdown>
#include "market_data/format.hpp" // IWYU pragma: keep - fmt::formatter<depth_speed>
#include "session/live_feed.hpp"
#include "session/live_session.hpp"
#include "session/user_data_feed.hpp"
#include "session/venue_gateway.hpp"
#include "symbol/symbol_spec.hpp"

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
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <vector>

using namespace exchange::engine;

namespace exchange::app::serve_detail {

asio::awaitable<void> report_progress(std::chrono::milliseconds every,
									  const serving_session *run) {
	spdlog::logger *log =
		&core::logging::logger_for(core::logging::channel::feed);
	return periodically(
		every,
		[run,
		 log,
		 last_events = std::uint64_t{0},
		 last_fills  = std::uint64_t{0}]() mutable {
			const auto &r = run->report();
			const std::uint64_t fills =
				run->monitor().fills().total_executions();
			const std::uint64_t frames   = r.events - last_events;
			const std::uint64_t executed = fills - last_fills;
			last_events                  = r.events;
			last_fills                   = fills;

			log->info(
				"+{} frames, +{} fills | working {}, position {} lots, pnl "
				"{} tick-lots, refused {}, breaker {}",
				frames,
				executed,
				run->gate().working_orders(),
				run->positions().net_lots(run->symbol()),
				run->gate().pnl(),
				run->gate().refused(),
				run->breaker().state());
		});
}

void warn_unreachable_caps(const serving_session &run,
						   const serve_settings &settings,
						   std::chrono::milliseconds cadence) {
	const auto &limits    = run.monitor().limits();
	const auto cadence_ns = to_ns(cadence);
	if (cadence_ns == 0) return;

	if (has_burst_limit(limits)) {
		const std::uint64_t window = run.monitor().fills().window_ns();
		const std::uint64_t needed =
			(static_cast<std::uint64_t>(limits.max_executions_per_window) +
			 1U) *
			cadence_ns;
		if (window < needed)
			spdlog::warn("--max-fills-per-window {} cannot be crossed: its "
						 "window is {} ms and crossing it needs about {} ms at "
						 "this cadence. Widen it with --burst-window-ms",
						 limits.max_executions_per_window,
						 to_ms(window).count(),
						 to_ms(needed).count());
	}

	if (has_ratio_limit(limits)) {
		const std::uint64_t window   = run.monitor().ratio().window_ns();
		const std::uint64_t messages = window / cadence_ns;
		if (messages < limits.min_messages_to_judge)
			spdlog::warn("--max-otr {} cannot be judged: its window is {} ms, "
						 "which holds about {} messages at this cadence, and "
						 "the floor is {}. Widen --otr-window-ms or lower "
						 "--min-otr-messages",
						 limits.max_messages_per_execution,
						 to_ms(window).count(),
						 messages,
						 limits.min_messages_to_judge);
		if (settings.take)
			spdlog::warn(
				"--max-otr {} is measuring a taker, which fills every "
				"order it sends - so its ratio is about 1:1 and no cap "
				"above one can trip. An order-to-trade ratio is a rule "
				"about quoting churn; pass --quote for it to mean "
				"something",
				limits.max_messages_per_execution);
	}
}

void report_run(const serving_session &run,
				const std::optional<live_feed_report> &feed,
				const shipper_stats &shipping,
				const std::optional<user_data_report> &account) {
	const auto &r = run.report();
	if (feed)
		spdlog::info("feed: {} frames ({} malformed), {} reconnects, snapshots "
					 "{}/{} applied - {}",
					 feed->frames,
					 feed->malformed,
					 feed->reconnects,
					 feed->snapshots_applied,
					 feed->snapshots_requested,
					 feed->stopped);
	else
		spdlog::info("feed: report unavailable - the run was interrupted with "
					 "the pipeline still suspended, so it never returned one. "
					 "The bridge's event count below is what arrived");
	spdlog::info("bridge: {} events, {} snapshots ({} gaps, {} invalidations), "
				 "{} depth commands",
				 r.events,
				 r.snapshots,
				 r.gaps,
				 r.invalidations,
				 r.depth_commands);
	// `no_room` is here rather than left on the quoter because a report full of
	// zeroes has exactly one innocent explanation and this is it: the venue
	// quoted tighter than twice --improve, so there was never a price to rest
	// at. Nobody reaches that conclusion from "0 quotes".
	spdlog::info("strategy: {} quotes, {} takes, {} commands accepted, {} "
				 "frames with no room to quote, {} sink stalls",
				 run.quoter().quotes(),
				 run.quoter().takes(),
				 run.quoter().submitted(),
				 run.quoter().no_room(),
				 r.stalls);
	if (r.stale_frames != 0)
		spdlog::warn("          {} frame(s) not priced off: older than "
					 "--max-feed-lag-ms by the wall clock",
					 r.stale_frames);
	if (run.quoter().backed_off() != 0)
		spdlog::info(
			"          {} frame-side(s) left unquoted while backing off "
			"venue refusals",
			run.quoter().backed_off());
	// Printed only when the run was a simulation, and labelled as one. A line
	// that said "0 inferred" on a production run would invite the reading that
	// the model looked and found nothing, which is the opposite of the truth.
	if (run.options().simulate_fills)
		// The frame count comes first on purpose. Every other number here is
		// gated on the venue having crossed one of our prices, so they all read
		// zero on a quiet market *and* on a broken pipeline; the frame count is
		// what tells those two apart, and a reader should see it before the
		// zeroes rather than after them.
		spdlog::info("simulated: {} frames inferred, {} aggressors offering {} "
					 "lots, {} lots queued ahead of us - trade-through {}, "
					 "queue model {}. These fills are a judgement, not the "
					 "engine's",
					 r.inferred_frames,
					 r.injected_aggressors,
					 r.injected_lots,
					 r.queue_absorbed_lots,
					 run.options().fills.require_trade_through ? "on" : "off",
					 run.options().fills.model_queue_position ? "on" : "off");
	// Only when a wire was in the chain, and for the same reason the simulated
	// line is conditional: "0 in flight" on a run with no wire would read as a
	// measurement rather than as an absence.
	if (run.pipe().is_modelled())
		spdlog::info("wire: {} ns flight (+{} jitter), {} commands delivered, "
					 "{} still in flight at exit, {} refused for want of "
					 "flight room",
					 run.options().latency.order_entry_ns,
					 run.options().latency.jitter_ns,
					 r.wire_delivered,
					 r.orders_in_flight,
					 r.wire_refusals);
	spdlog::info("risk: {} passed, {} refused, {} events routed back, position "
				 "{} lots, pnl {} tick-lots",
				 run.gate().passed(),
				 run.gate().refused(),
				 r.engine_events,
				 run.positions().net_lots(run.symbol()),
				 run.gate().pnl());
	// Which rule, and not just how many. A refusal count on its own says the
	// gate is doing something; it does not say whether the number is the collar
	// trimming the far end of the venue's depth (ordinary) or the position
	// limit binding (worth knowing). Only the rules that fired are listed, so a
	// run with nothing configured prints nothing.
	for (const risk::hooks::breach rule : risk::hooks::ALL_BREACHES) {
		if (rule == risk::hooks::breach::NONE) continue;
		const std::uint64_t hits = run.gate().breaches(rule);
		if (hits != 0) spdlog::info("  refused by {}: {}", rule, hits);
	}
	// The windows the caps were actually measured over, read back from the
	// objects rather than from the settings: they are powers of two, so what an
	// operator asked for and what the rule used are not the same number.
	const auto &limits = run.monitor().limits();
	if (has_burst_limit(limits) || has_ratio_limit(limits))
		spdlog::info(
			"  windows: burst {} ms over {} executions, otr {} ms over "
			"{} messages",
			to_ms(run.monitor().fills().window_ns()).count(),
			limits.max_executions_per_window,
			to_ms(run.monitor().ratio().window_ns()).count(),
			limits.min_messages_to_judge);
	spdlog::info("surveillance: {} executions, {} messages, {} trips; feed "
				 "watchdog {} beats, {} trips",
				 run.monitor().fills().total_executions(),
				 run.monitor().ratio().total_messages(),
				 run.monitor().trips(),
				 run.feed_watchdog().beats(),
				 run.feed_watchdog().trips());
	// Printed only when the run could send, and labelled as one. "0 orders
	// sent" on a run with no gateway would read as a measurement of a strategy
	// that wrote nothing, which is the opposite of the truth. Same rule the
	// simulated and wire lines above already follow.
	if (run.router().is_sending()) {
		const session::router_stats routed = run.router().stats();
		const session::gateway_stats sent  = run.router().gateway()->stats();
		spdlog::info(
			"venue out: {} orders offered, {} queued, {} refused by "
			"the gateway, {} dropped for want of outbox room, {} still "
			"waiting at exit",
			routed.offered,
			routed.queued,
			routed.refused,
			routed.discarded,
			routed.queued_now);
		spdlog::info("           {} placements, {} reprices, {} cancels, {} "
					 "weight spent; {} written, {} accepted, {} refused, {} "
					 "unanswered, {} cancels too late",
					 sent.placed,
					 sent.replaced,
					 sent.cancelled,
					 sent.weight_spent,
					 shipping.sent,
					 shipping.accepted,
					 shipping.refused,
					 shipping.failed,
					 shipping.cancels_too_late);
		// Why the gateway refused, by reason. "2 refused by the gateway" alone
		// was the whole of what a testnet run said about orders that never
		// left - and the reason, the venue's minimum order value, was one
		// counter away.
		for (std::size_t why = 0; why < sent.refused_by.size(); ++why)
			if (sent.refused_by[why] != 0)
				spdlog::warn(
					"           {} refused because {}",
					sent.refused_by[why],
					message(static_cast<session::gateway_refusal>(why)));
		if (routed.reconciliations != 0 || routed.unreadable_reads != 0)
			spdlog::info("           reconciled {} time(s) ({} unreadable): {} "
						 "orphan(s) cancelled, {} order(s) presumed gone, {} "
						 "stale listing(s)",
						 routed.reconciliations,
						 routed.unreadable_reads,
						 routed.orphans_cancelled,
						 routed.presumed_gone,
						 routed.stale_listings);
		if (!run.router().orphan_ids().empty())
			spdlog::warn("           orphans: {}",
						 fmt::join(run.router().orphan_ids(), ", "));
		if (routed.orphans_cancelled != 0 || routed.presumed_gone != 0)
			spdlog::warn("           the engine's view of the venue drifted "
						 "during the run - each orphan or presumed-gone order "
						 "is a lost report or a bookkeeping bug worth tracing");
		if (sent.replaced != 0 || routed.held != 0)
			spdlog::info(
				"           reprices: {} taken, {} left the order where "
				"it was, {} found it gone, {} withdrew it; {} amendment(s) "
				"or cancel(s) waited behind one in flight",
				shipping.replaced,
				shipping.replace_unchanged,
				shipping.replace_gone,
				shipping.replace_withdrawn,
				routed.held);
		// An order written and never answered is the one number here that does
		// not settle by itself: it is neither placed nor refused, and only the
		// venue knows which. Said as a warning so it is not read past.
		if (shipping.failed != 0)
			spdlog::warn("           {} order(s) were written and never "
						 "answered - run `account` to find out whether they "
						 "are working",
						 shipping.failed);

		if (account)
			spdlog::info("venue in: {} frames, {} reports, {} malformed, {} "
						 "reconnects, {} subscribes - {}",
						 account->frames,
						 account->reports,
						 account->malformed,
						 account->reconnects,
						 account->subscribes,
						 account->stopped);
		else
			spdlog::info(
				"venue in: report unavailable - the account stream was "
				"still running when the run stopped. The counts below "
				"are what it delivered");
		spdlog::info("          {} reports for this listing: {} booked, {} "
					 "confirmed what the engine already had, {} unusable; {} "
					 "lots filled, {} stream gap(s)",
					 r.venue_reports,
					 r.venue_booked,
					 r.venue_confirmations,
					 r.venue_unusable,
					 r.venue_filled_lots,
					 r.venue_gaps);
		// Separate from the reports above because it did not come from the
		// account stream: a refused placement never became an order, so the
		// stream has nothing to say about it and the HTTP response is the whole
		// notification. @see live_session::on_send_refused
		if (r.venue_refused != 0)
			spdlog::info(
				"          {} placement(s) the venue refused outright, "
				"withdrawn from the engine's ledger",
				r.venue_refused);
		if (const auto fees = run.spec().usdt_from(run.gate().fees());
			fees && r.venue_filled_lots != volume_t{})
			spdlog::info(
				"          fees: {} USDT charged against the P&L above",
				core::scaled::to_decimal(usdt_e8_of(*fees), USDT_SCALE));
		if (r.venue_fee_base_scaled != 0)
			spdlog::info("          {} of the base asset paid in fees - the "
						 "account holds that much less than the position says",
						 core::scaled::to_decimal(r.venue_fee_base_scaled,
												  venue::COMMISSION_SCALE));
		if (const auto late = run.gate().unledgered_fills(); late != 0)
			spdlog::info("          {} fill(s) arrived after the engine had "
						 "withdrawn the order, booked on the venue's side",
						 late);
		if (r.fills_recovered != 0 || r.fills_unattributed != 0 ||
			r.fill_recovery_failed != 0)
			spdlog::warn("          {} fill(s) the account stream lost, "
						 "recovered from the trade history; {} not ours to "
						 "attribute; {} read(s) failed",
						 r.fills_recovered,
						 r.fills_unattributed,
						 r.fill_recovery_failed);
		if (r.venue_duplicate_fills != 0)
			spdlog::warn("          {} fill report(s) delivered twice, booked "
						 "once",
						 r.venue_duplicate_fills);
		if (const auto ignored = run.gate().ignored_engine_fills();
			ignored != 0)
			spdlog::warn("          {} execution(s) the engine printed against "
						 "our orders, not booked - the venue never made them",
						 ignored);
		if (r.venue_side_unknown != 0)
			spdlog::warn(
				"          {} fill(s) not booked: the venue did not say "
				"which side the order was on",
				r.venue_side_unknown);
		if (r.venue_fees_unvalued != 0)
			spdlog::warn(
				"          {} fill(s) charged a fee in an asset this run "
				"cannot value - the P&L and the loss floor omit them",
				r.venue_fees_unvalued);
		if (r.engine_withdrawals != 0)
			spdlog::info("          {} order(s) the venue finished, withdrawn "
						 "from the engine's own book",
						 r.engine_withdrawals);
		if (r.venue_feedback != 0)
			spdlog::info(
				"          {} order(s) or amendment(s) that never took "
				"effect at the venue, told to the engine",
				r.venue_feedback);
		// Said whichever way it went, because "nothing was left resting" is the
		// reassurance the line exists to give and a silent report cannot give
		// it. @see live_session::withdraw_all
		spdlog::info("          {} order(s) withdrawn on the way out",
					 r.venue_withdrawn);
	}

	if (run.breaker().state() != risk::hooks::system::trading_state::NORMAL)
		spdlog::warn("the breaker is open: {} ({})",
					 run.breaker().state(),
					 run.breaker().cause());
}

} // namespace exchange::app::serve_detail
