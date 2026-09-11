#pragma once
// The live topology: a venue's feed, this process's engine, and two threads.
//
// `backtest::session` is this same chain folded into one thread against a
// recording - the same depth bridge, the same quoter, the same risk gate, the
// same partition and matching engine. What it cannot be is *live*, and the
// difference is not the feed: it is that a deployment runs the matching engine
// on a thread of its own, so everything the harness does synchronously has to
// cross a queue in one direction and a channel in the other.
//
// This is that crossing, assembled. `serve.cpp` is the shim that gives it an
// io_context, a signal handler and a metrics timer; everything about *what is
// connected to what* is here, where a test can drive it with scripted depth
// events and no network.
//
//   ┌── producer thread (the io_context's) ──────────────────────────────┐
//   │  ws frame ─▶ reconstructor ─▶ depth_feed_bridge ─▶ ADD/REDUCE ──┐  │
//   │                     │                                           │  │
//   │                     └─▶ spread_quoter ─▶ PLACE/CANCEL ──────────┤  │
//   │                                                                 ▼  │
//   │                                                         risk_gate  │
//   │                                                              │     │
//   │                          order_router ─▶ outbox ─▶ venue ◀───┘     │
//   │                        (only with a gateway; nothing is sent       │
//   │                         without one, which is the default)         │
//   └────────────────────────────────────────────────────────────┬───────┘
//                                                               │ SPSC queue
//   ┌── consumer thread ─────────────────────────────────────────▼───────┐
//   │            engine_partition ─▶ matching_engine ─▶ order_book       │
//   └────────────────────────────────────────────────────────────┬───────┘
//                                                               │ event_channel
//   ┌── producer thread again ───────────────────────────────────▼───────┐
//   │  event_dispatcher ─▶ feedback_router ─▶ risk_gate + spread_quoter  │
//   │  venue account stream ─▶ on_report ─┘└─▶ post_trade_monitor        │
//   └────────────────────────────────────────────────────────────────────┘
//
// The account stream joins the return path at the router rather than at the
// channel, and it has to: the channel is the *consumer thread's* way of
// publishing, and a venue's report did not come from the consumer thread. Both
// coroutines run on the io_context, which is the producer thread, so both reach
// the router directly and neither crosses a boundary. @see on_report
//
// --- the threading contract, which is the only thing here worth memorising --
//
// Every member of this class belongs to the **producer thread** except
// `drain_and_publish`, which belongs to the **consumer thread** and is the only
// thing that thread may touch. The two never share a mutable object: commands
// go one way through the partition's SPSC queue and events come back the other
// way through the `event_channel`, and both are single-producer,
// single-consumer by construction.
//
// The two objects that *are* shared - `position_book` and `circuit_breaker` -
// are shared with nothing on the consumer thread. They are read and written by
// the producer thread alone here; their atomics exist for an operator's console
// and for a second gate, neither of which this composition has yet.
//
// --- why the producer pumps feedback while it waits ------------------------
//
// This is the deadlock the shape invites, and it is worth stating before
// somebody removes the loop that avoids it. The consumer's cycle is `drain`,
// `publish`, `flush`, and `event_channel` asserts that a backlog is cleared
// before the next publish - so a consumer that cannot publish stops draining.
// If the producer then blocks waiting for room in the partition's queue without
// draining the channel, neither thread can move: the queue is full because the
// consumer stopped, and the consumer stopped because the channel is full
// because the producer stopped reading it.
//
// So every wait in this file pumps. `submit_feed` and `quote` retry through
// `pump()` rather than through a bare yield, which is what makes the
// back-pressure lossless in both directions rather than only in one.
//
// --- where "now" comes from, and why it is two clocks ----------------------
//
// Deliberately split, and the split is the same argument `clock.hpp` and
// `heartbeat.hpp` already make from opposite ends:
//
//   * the risk gate, the post-trade window and the feed watchdog are handed a
//     *local monotonic* reading, because every one of them measures an interval
//     and a venue clock that steps turns an interval into a negative number;
//   * the quoter's requote cadence is handed the venue's *event time*, because
//     it is a statement about market activity rather than about wall time - the
//     same reading `core::chrono::feed_clock` gives it over a capture, so a
//     quoter behaves the same way live as it did in the replay that justified
//     it.

#include "core/chrono/clock.hpp"
#include "event/command.hpp"
#include "event/event_channel.hpp"
#include "event/event_dispatcher.hpp"
#include "execution/book_manager.hpp"
#include "execution/engine_partition.hpp"
#include "execution/order_manager.hpp"
#include "feedback_fanout.hpp"
#include "latency_pipe.hpp"
#include "ledger_view.hpp"
#include "market_data/l2_book.hpp"
#include "market_data/normalised.hpp"
#include "market_data/reconstructor.hpp"
#include "market_data/sequencer.hpp"
#include "order_book/trade.hpp"
#include "order_router.hpp"
#include "orders/types.hpp"
#include "reaction_metrics.hpp"
#include "risk_management/gate.hpp"
#include "risk_management/hooks/feedback.hpp"
#include "risk_management/hooks/post_trade/limits.hpp"
#include "risk_management/hooks/post_trade/monitor.hpp"
#include "risk_management/hooks/pre_trade/position.hpp"
#include "risk_management/hooks/system/circuit_breaker.hpp"
#include "risk_management/hooks/system/heartbeat.hpp"
#include "risk_management/limits.hpp"
#include "strategy/backtest/depth_feed_bridge.hpp"
#include "strategy/backtest/fill_model.hpp"
#include "strategy/quoter.hpp"
#include "symbol/symbol_spec.hpp"
#include "transport/rest/request.hpp"
#include "venue/execution_report.hpp"
#include "venue_bridge.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace exchange::session {

/// @brief How a live session is configured. Every default is the permissive or
///        disabled one, so a `serve` with no risk flags measures the engine
///        rather than the gate - the same posture @c session_options takes.
struct live_session_options {
	/// @brief Passed to the reconstructor that gap-checks the feed.
	market_data::reconstructor_options feed{};

	/// @brief Size, aggression and requote cadence of the reference quoter.
	strategy::quoter_options quoting{};

	/// @brief The pre-trade policy. @see risk_limits
	risk::risk_limits limits{};

	/// @brief The post-trade thresholds. Every rule off by default. @see
	///        post_trade_limits
	risk::hooks::post_trade::post_trade_limits surveillance{};

	/// @brief Breaches in one window that trip the breaker, or @c NO_AUTO_TRIP.
	std::uint32_t breaches_to_trip =
		risk::hooks::system::circuit_breaker::NO_AUTO_TRIP;

	/**
	 * @brief Silence from the venue's market data that trips the breaker, in
	 *        nanoseconds. Zero disables the watchdog.
	 *
	 * Sized against the diff stream's cadence rather than against how quiet a
	 * market can get: at the 100 ms speed two missed frames is a diagnosis,
	 * while "no trades for a second" is an ordinary afternoon. @see
	 * heartbeat_monitor
	 */
	std::uint64_t feed_timeout_ns = 0;

	/**
	 * @brief Infer the fills our *resting* orders would have taken, and inject
	 *        them as aggressing flow.
	 *
	 * Off by default, and the default is the load-bearing part. Without this a
	 * live session is the production chain and nothing else: every fill in it
	 * happened because one of our orders crossed real published depth in a real
	 * book. Turning it on adds the one thing in the system that is a
	 * *judgement* rather than the shipped code - see @c crossing_fill_model,
	 * which is the file to read before believing any number a run with this set
	 * produces.
	 *
	 * @par What it buys
	 * A passive strategy can fill at all. Seeded depth is rested with
	 * @c add_order, which does not match, so a resting quote on the live path
	 * fills against nothing and a market-making run measures to exactly zero
	 * executions. That is an artefact of how the venue's depth is mirrored, not
	 * a finding about the strategy.
	 *
	 * @note Not free when off, and deliberately so. The model stays spliced
	 * into the command chain either way, because that is how it learns which
	 *       resting orders are ours, so a disabled run still pays one
	 *       @c push_back per order placed. Making it disappear entirely would
	 *       mean a second instantiation of this whole class selected at run
	 * time
	 *       - a large compile-time cost to remove a store from a command path
	 *       that has already been through a hash probe in the gate.
	 */
	bool simulate_fills = false;

	/// @brief How generous that inference is allowed to be. Read only when
	///        @c simulate_fills is set. @see fill_model_options
	strategy::backtest::fill_model_options fills{};

	/**
	 * @brief How long a command of ours spends in flight before the engine has
	 *        it - the network, modelled.
	 *
	 * All-zero by default, and the default is the chain as it has always been:
	 * no wire is built at all and the gate writes into the fill model
	 * synchronously. @see latency_pipe on why zero must not mean "a wire with a
	 * zero delay".
	 *
	 * @par Why a live run wants this at all
	 * Because the offline harness has had it since `wire` landed, and a number
	 * measured under a modelled microsecond cannot be compared with one
	 * measured under an instantaneous order path. The two runs are then not the
	 * same experiment, and the axis they differ on is invisible in both
	 * reports. Setting the same @c latency_model on both is what makes them
	 * comparable.
	 *
	 * @par What is modelled and what is not
	 * The order path only, exactly as offline: our command takes time to reach
	 * the venue. The market_data delay on the way in is not modelled, and the
	 * approximation is the same one @c latency_model already documents - the
	 * two enter the result through their sum, so setting this to the whole
	 * round trip gets the arithmetic right while still letting the strategy see
	 * every frame it would really have been blind to.
	 *
	 * @note Nothing here drives delivery. A command comes due in wall-clock
	 *       time, so something has to look - @c deliver_due is that call, and
	 *       @c serve.cpp arms a timer from @c next_due_ns to make it. A session
	 *       driven only by frames would deliver on market activity, which is
	 * the very thing that made the offline wire unusable live.
	 */
	strategy::backtest::latency_model latency{};

	/**
	 * @brief The listing as the venue spells it - @c SOLUSDT.
	 *
	 * Read only by a session that has been given a gateway. Separate from
	 * @c symbol_spec::symbol() because the engine's symbol table and a venue's
	 * listing names are different namespaces, which is the same reason
	 * @c to_outbound_order takes it as its own parameter rather than reading it
	 * off the spec.
	 */
	std::string venue_symbol{};

	/// @brief Resting-order hint for the listing's book.
	std::size_t book_capacity =
		engine::execution::book_manager::DEFAULT_BOOK_CAPACITY;

	/// @brief Records the partition's order store holds.
	std::uint32_t order_capacity =
		engine::execution::order_manager::DEFAULT_CAPACITY;

	/**
	 * @brief Where the partition records its counters, or @c nullptr for none.
	 *
	 * Not owned, and it must outlive the session. A pointer rather than a
	 * member so a deployment that never asked for metrics pays exactly the cost
	 * of an unmetered partition - the same posture @c cmd_demo takes, and the
	 * reason @c core::metrics::settings defaults to off.
	 *
	 * @note Written by the consumer thread and read by whoever renders the
	 *       exposition, which is why the counters underneath are atomic.
	 */
	engine::execution::partition_metrics *metrics = nullptr;

	/**
	 * @brief Where reaction time is recorded, or @c nullptr to not measure it.
	 *
	 * Separate from @c metrics rather than folded into it, because they are
	 * owned by different things and read at different points. @c
	 * partition_metrics belongs to the consumer thread's partition and times a
	 * drain; this belongs to the producer thread and times the whole ingest-to-
	 * enqueue path in front of it. Sharing one struct would put two threads'
	 * single-writer counters on the same cache lines. @see reaction_metrics
	 */
	reaction_metrics *reaction = nullptr;
};

/// @brief What a live session did. The feed's own counters are
///        @c live_feed_report; these are the engine's, the gate's and the
///        strategy's.
struct live_session_report {
	std::uint64_t events        = 0; ///< Diffs handed to the bridge.
	std::uint64_t snapshots     = 0; ///< Snapshots handed to the bridge.
	std::uint64_t gaps          = 0; ///< Sequence gaps that killed the replica.
	std::uint64_t invalidations = 0; ///< Reconnects that withdrew seeded depth.
	std::uint64_t depth_commands = 0; ///< ADD / REDUCE the bridge emitted.
	std::uint64_t engine_events  = 0; ///< Trades and outcomes routed back.
	/**
	 * @brief Times a submit was refused for want of queue or channel room.
	 *
	 * Saturation, not error: the batch is retried and nothing is lost. It is
	 * the number that says the consumer is not keeping up, which is the one
	 * thing a report about a two-thread pipeline has to be able to say.
	 */
	std::uint64_t stalls = 0;

	// --- what the fill model did, all zero unless `simulate_fills` ----------

	/**
	 * @brief Frames on which the model actually looked.
	 *
	 * The counter that separates "inferred nothing because nothing crossed our
	 * price" from "never ran at all", which no other number here can do: every
	 * other field below is gated on the venue having crossed us, so they all go
	 * to zero together and a run of zeroes carries one bit rather than four.
	 * Without this a quiet market and a broken pipeline print the same report.
	 */
	std::uint64_t inferred_frames = 0;

	// --- what the modelled network did, all zero unless `latency` is set -----

	/**
	 * @brief Commands the wire has handed the engine.
	 *
	 * Assigned on every delivery pass rather than accumulated, so it is current
	 * as of the last @c deliver_due - which @c on_event makes once a frame and
	 * a timer makes in between. @see latency_pipe
	 */
	std::uint64_t wire_delivered = 0;

	/// @brief Commands of ours the engine has not been told about yet. A gauge,
	///        not a total: at the end of a run it is the tail of the wire -
	///        orders written and never delivered, because the run stopped
	///        first.
	std::uint64_t orders_in_flight = 0;

	/// @brief Batches the wire refused because too much of ours was already in
	///        flight. Back-pressure the gate rolled back, not loss.
	std::uint64_t wire_refusals = 0;

	/// @brief Aggressing orders injected on our resting orders' behalf.
	std::uint64_t injected_aggressors = 0;

	/// @brief Lots those orders offered. An *upper bound* on what they filled,
	///        for two reasons live: the book may hold less than the model
	///        thought, and the model reads a ledger that is a round-trip behind
	///        it. @see session::ledger_view
	volume_t injected_lots = 0;

	/// @brief Venue liquidity the queue model made our orders wait behind. What
	///        the front-of-queue assumption would have been worth.
	volume_t queue_absorbed_lots = 0;

	// --- the venue's own return leg, all zero unless a gateway is wired -----

	/// @brief Execution reports the account stream delivered for this listing.
	std::uint64_t venue_reports = 0;

	/**
	 * @brief Reports routed into the feedback path - the ones that told this
	 *        process something the engine could not know for itself.
	 *
	 * @see on_report, where the three kinds are argued.
	 */
	std::uint64_t venue_booked = 0;

	/// @brief Reports that only confirmed a transition the engine had already
	///        published. Not routed, and counted so the difference between
	///        "the venue said nothing" and "the venue agreed" is visible.
	std::uint64_t venue_confirmations = 0;

	/// @brief Lots the venue actually executed against our orders. The number a
	///        run with a gateway exists to produce.
	volume_t venue_filled_lots = 0;

	/**
	 * @brief Reports that named an order this process could not place in its own
	 *        terms.
	 *
	 * Never zero for an innocent reason once it is non-zero: it means either an
	 * order from an earlier run of this process is still working - client ids
	 * are derived from an engine id that restarts at one - or a quantity came
	 * back off the listing's grid. Both want looking at. @see venue_bridge
	 */
	std::uint64_t venue_unusable = 0;

	/// @brief Breaks in the account stream. Each one is a window in which a
	///        fill of ours may have gone unreported. @see on_gap
	std::uint64_t venue_gaps = 0;

	/// @brief Placements the venue refused outright, told to the engine from
	///        the HTTP response rather than the account stream - which sends
	///        nothing for an order it never accepted. @see on_send_refused
	std::uint64_t venue_refused = 0;

	/// @brief Cancels written on the way out, to leave nothing of ours resting
	///        in a book this process is about to stop watching.
	///        @see live_session::withdraw_all
	std::uint64_t venue_withdrawn = 0;
};

/**
 * @brief One listing, one live feed, one engine, two threads.
 *
 * @par What it assembles
 * The production chain, and nothing simulated. Depth arrives, the bridge turns
 * it into anonymous liquidity resting in a real @c order_book, the quoter shows
 * a two-sided market inside the venue's touch, the risk gate screens every
 * command either of them writes, the partition matches on its own thread, and
 * what comes back reaches the gate, the quoter and the post-trade monitor
 * through the same @c event_dispatcher a multi-listing deployment would use.
 *
 * @par The three risk lanes, wired
 * All of them, which is the point of this class existing rather than the
 * pipeline being written inline in `serve.cpp`:
 *
 *   * **pre-trade** - the gate in front of the partition. Every ADD from the
 *     bridge and every PLACE from the quoter passes the size, collar, position,
 *     exposure and rate rules first.
 *   * **post-trade** - a @c post_trade_monitor attached beside the gate,
 *     watching its own listing's executions and messages. @see poll
 *   * **system** - a @c heartbeat_monitor over the same breaker, beaten by
 *     every frame that arrives and polled by every pump, plus the breach-rate
 *     cut-out the breaker owns itself.
 *
 * @par It models @c live_handler
 * So @c run_live_feed drives it directly: the four snapshot members delegate to
 * the bridge's reconstructor, which is the same public surface
 * @c depth_reconstructor offers. There is no adapter, and there should not be.
 *
 * @tparam Observer Who is told when the gate refuses a command, trips the
 *         breaker or is pushed back on. Defaults to @c no_observer, which hears
 *         nothing and costs nothing - the refusal path must not pay a null
 * check for a hook nobody defined. @see risk_management/hooks/observer.hpp
 * @tparam Watcher Who else sees the trades and outcomes coming back, after the
 *         gate and the strategy have. Defaults to @c no_watcher. @see
 *         feedback_fanout
 * @tparam Clock Where the *local* reading comes from - the gate's rate window,
 *         the post-trade windows and the feed watchdog. A parameter for the
 *         reason @c risk::clock.hpp gives: a test that had to sleep across a
 *         watchdog's timeout to check it is slow and flaky in the same breath,
 *         and a replay needs recorded time or its windows fire in the wrong
 *         places. @see core::chrono::nanosecond_clock
 *
 * @note One listing, like everything else on this side of the queue. A second
 *       listing is a second gate, a second monitor and one more slot in the
 *       router - which is exactly why the router is here rather than a direct
 *       call, even though today it routes one symbol. @see feedback_router
 */
template <core::chrono::nanosecond_clock Clock = core::chrono::steady_nanos,
		  risk::hooks::risk_observer Observer  = risk::hooks::no_observer,
		  class Watcher                        = no_watcher>
class live_session {
public:
	/// @brief Slots in the partition's command ring. A frame's worth of depth
	///        diff plus a requote is a few dozen commands; this is three orders
	///        of magnitude of headroom, and it is what the producer stalls
	///        against when the consumer falls behind.
	static constexpr std::size_t QUEUE_CAPACITY = 4096;

	/// @brief Slots in the event channel coming back. Sized larger than the
	///        command ring on purpose: one crossing command can produce several
	///        trades and several outcomes, so the return path is the wider one.
	static constexpr std::size_t CHANNEL_CAPACITY = 8192;

	using command        = engine::event::command;
	using partition_type = engine::execution::engine_partition<QUEUE_CAPACITY>;
	using channel_type   = engine::event::event_channel<CHANNEL_CAPACITY>;
	using clock_type     = Clock;

	/// @brief The passive-fill inference, spliced between the gate and the
	///        partition exactly as the offline harness splices it.
	///
	/// In the chain rather than beside it because @c submit_range is how the
	/// model learns which resting orders are ours - the same command stream the
	/// partition sees, so the two cannot disagree about what was placed.
	using fill_model_type =
		strategy::backtest::crossing_fill_model<partition_type>;

	/// @brief The modelled network, between the gate and the fill model -
	///        exactly where @c backtest::wire sits offline, and for the reason
	///        that header gives: the model's working set has to be orders the
	///        engine has been *told about*, not orders we have written.
	using pipe_type = latency_pipe<fill_model_type, clock_type>;

	/// @brief The copy of our own orders that goes to the venue, spliced
	///        immediately below the gate.
	///
	/// Below it because nothing unscreened may reach a venue, and immediately
	/// below rather than under the wire because the wire models a network that
	/// a session with a gateway *has*. Delaying the real send by a fictional
	/// flight time would be two networks in series, one of them made up.
	using order_router_type = order_router<pipe_type>;

	using gate_type   = risk::risk_gate<order_router_type, clock_type, Observer>;
	using quoter_type = strategy::spread_quoter<gate_type>;


	/// @brief What the router's slot for this listing holds. @see
	///        feedback_fanout
	using fanout_type  = feedback_fanout<gate_type, quoter_type, Watcher>;
	using router_type  = risk::hooks::feedback_router<fanout_type, clock_type>;
	using monitor_type = risk::hooks::post_trade::post_trade_monitor;
	using dispatcher_type =
		engine::event::event_dispatcher<channel_type, router_type>;

	/**
	 * @brief Assemble a live session for @p spec.
	 *
	 * @param spec The listing. Supplies the engine-side id and the tick and lot
	 *        grid the feed's scaled decimals are converted on. Must outlive the
	 *        session - reference data is owned by the registry.
	 * @param options Feed, quoting, risk and capacity policy.
	 */
	/// @param observer Told about refusals and trips. Copied into the gate.
	/// @param watcher Told about trades and outcomes. Copied into the fan-out.
	explicit live_session(const engine::symbol_spec &spec,
						  live_session_options options = {}, Clock clock = {},
						  Observer observer = {}, Watcher watcher = {})
		: spec_(&spec),
		  options_(options),
		  clock_(std::move(clock)),
		  // Braces rather than a named sink type, because naming one inside a
		  // template needs `typename` for MSVC and then reads as redundant to
		  // clang. Both sinks are defaulted parameters, so there is nothing to
		  // name: this partition publishes through the event channel in
		  // `drain_and_publish`, not through a callback.
		  partition_({}, {}, options.book_capacity, options.order_capacity,
					 options.metrics),
		  positions_(std::max<std::size_t>(
			  risk::hooks::pre_trade::position_book::DEFAULT_CAPACITY,
			  static_cast<std::size_t>(spec.id()) + 1U)),
		  breaker_(options.breaches_to_trip),
		  fills_(partition_, spec, options.fills),
		  // `clock_` from here on, never the parameter: it was moved from
		  // above, and a clock with state - a test's hand-driven one - is left
		  // empty by that move. Copies of `clock_` also all read the same
		  // "now", which is the whole point of a stateful clock.
		  pipe_(fills_, clock_, options.latency),
		  orders_(pipe_, spec, options.venue_symbol),
		  gate_(orders_, spec.id(), options.limits, positions_, breaker_, 0,
				clock_, observer),
		  quoter_(gate_, spec, options.quoting),
		  watch_(breaker_, spec.id(), options.surveillance, clock_.now()),
		  fanout_(gate_, quoter_, watcher),
		  hooks_(static_cast<std::size_t>(spec.id()) + 1U, clock_),
		  dispatch_(channel_, hooks_),
		  feed_watch_(breaker_, options.feed_timeout_ns, clock_.now_ns()),
		  bridge_(spec, options.feed) {
		// On the consumer's side of the contract, and before either thread
		// starts: a partition refuses a symbol it was not given rather than
		// inventing a book for it.
		partition_.listing(spec.id());
		hooks_.attach(fanout_, watch_);

		// A batch larger than the whole schedule can never fit, however long
		// anyone waits - and `quote` waits by retrying, so a wire too small for
		// one requote is a hang rather than a slow run. The quoter states its
		// own worst case, so the check is exact rather than a guess at a floor.
		//
		// An assertion rather than a rejection, on the same grounds symbol_spec
		// gives for its own: this number comes from a flag an operator set, not
		// from a client, so it is a deployment mistake to be caught in a test
		// run rather than an input to be refused at run time.
		assert((!pipe_.is_modelled() ||
				options.latency.max_in_flight >=
					quoter_type::MAX_COMMANDS_PER_REQUOTE) &&
			   "a wire too small for one requote cannot ever deliver it");

	}

	/**
	 * @brief Send this session's own orders to @p gateway, and book what comes
	 *        back through @ref on_report.
	 *
	 * Off until it is called, which is what keeps a session that never asks for
	 * order entry exactly the chain it has always been: the strategy quotes,
	 * the gate screens, the engine matches against mirrored depth, and nothing
	 * leaves the process.
	 *
	 * @param gateway Borrowed; must outlive the session. Not owned because a
	 *        gateway holds a rate-limit budget that is per *IP* rather than per
	 *        session, so a deployment trading two listings shares one.
	 *
	 * @pre Called before the first frame, and once. @see order_router::attach
	 * @pre @c live_session_options::venue_symbol was set.
	 */
	void attach_gateway(venue_gateway &gateway) noexcept {
		// Two sources of fills for one set of orders. The inference model asks
		// what the venue's depth *would* have traded against our resting
		// orders; the account stream reports what it actually did. Running both
		// books every execution twice - the position, the PnL and every
		// post-trade window are then measuring a market that does not exist.
		// An assertion rather than a rejection, on the grounds symbol_spec
		// gives for its own: this is a combination of flags an operator set,
		// to be caught in a test run rather than refused at run time.
		assert(!options_.simulate_fills &&
			   "a session that receives real fills must not infer them too");
		orders_.attach(gateway);
	}

	// The gate points at the partition, the quoter at the gate, the fan-out at
	// both, and the router holds a pointer to the fan-out. Every one of those
	// is an interior reference, so this object cannot be relocated after the
	// wiring in the constructor body has run.
	live_session(const live_session &)            = delete;
	live_session &operator=(const live_session &) = delete;
	live_session(live_session &&)                 = delete;
	live_session &operator=(live_session &&)      = delete;
	~live_session()                               = default;

	// --- what run_live_feed calls: the live_handler surface ----------------

	/**
	 * @brief Advance the market by one frame, and let the whole chain react.
	 *
	 * @param event The decoded, normalised diff; consumed.
	 * @return What the sequencer did with it. @c gap means the replica died and
	 *         the liquidity it had seeded has been withdrawn from the book.
	 */
	market_data::sequence_action on_event(market_data::depth_event event) {
		++report_.events;
		// Read before the move, not after: the event is consumed by the bridge
		// below and the argument order of a call is not a sequencing guarantee.
		// @see the same rule in binance/normalise.cpp
		const core::chrono::ingress_time arrival = event.ingress;
		// A frame is evidence the venue is alive whatever it says, and the
		// reading is local: a venue timestamp stops advancing whether the venue
		// went quiet or the link died, and those are the same emergency from
		// here. @see heartbeat_monitor
		feed_watch_.beat(clock_.now_ns());

		const auto venue_ns =
			static_cast<std::uint64_t>(event.event_time.count());

		feed_.clear();
		const market_data::sequence_action action =
			bridge_.on_event(std::move(event), feed_);
		if (action == market_data::sequence_action::gap) {
			++report_.gaps;
			// Every queue estimate was measured against a replica that no
			// longer exists. Re-measuring is the conservative choice as well as
			// the simple one. @see queue_position_book::clear
			fills_.reset_queue();
		}

		// Before the frame's own commands, not after: what is due now was
		// written on an *earlier* frame, so releasing it first is what keeps
		// the wire a queue rather than a stack. Harmless when there is no wire,
		// and it does not make the frame the wake-up - the timer in serve.cpp
		// is that. This is only so a busy market never has to wait for one.
		// @see latency_pipe::deliver_due
		(void)deliver_due();

		submit_feed();
		inject();
		quote(venue_ns);
		pump();
		// Last, and after pump(): the reaction is over when the commands this
		// frame caused have crossed into the partition's queue, which is what
		// submit_feed and quote do and what pump finishes when either had to
		// retry. Anything measured earlier would exclude the back-pressure that
		// is the most likely reason a reaction was slow.
		record_frame_reaction(arrival);
		return action;
	}

	/**
	 * @brief Hand the engine every command of ours whose flight time has
	 *        elapsed.
	 *
	 * @return How many were delivered - always zero unless
	 *         @c live_session_options::latency asked for a wire.
	 *
	 * @par Who calls this, and why it is not the frame path alone
	 * A command comes due in wall-clock time, and wall-clock time passes during
	 * a quiet market. @c on_event calls this so an active market never waits,
	 * but a session that *only* delivered on frames would be back to market
	 * time driving the wire - which is precisely what made the offline model
	 * unusable live. @c serve.cpp arms a steady timer from @c next_due_ns and
	 * calls this when it fires; a test moves its clock and calls it directly.
	 *
	 * Idempotent and cheap: it releases what is due and nothing else, so
	 * calling it early is a no-op rather than an early delivery.
	 *
	 * @note Producer thread, like everything here but @c drain_and_publish.
	 */
	std::size_t deliver_due() {
		const std::size_t delivered = pipe_.deliver_due();
		if (delivered != 0) pump();

		// Assigned, not accumulated: the pipe keeps the running totals and
		// adding them here would count every earlier pass again. Same rule the
		// fill model's three fields follow. @see inject
		report_.wire_delivered   = pipe_.delivered();
		report_.orders_in_flight = pipe_.in_flight();
		report_.wire_refusals    = pipe_.refusals();
		return delivered;
	}

	/// @brief When the earliest command of ours in flight comes due, or nothing
	///        if none is. What a timer arms itself from. @see deliver_due
	[[nodiscard]] std::optional<std::uint64_t> next_due_ns() const noexcept {
		return pipe_.next_due_ns();
	}

	/**
	 * @brief Seed or repair the replica from a REST snapshot.
	 * @return Whether the replica is live afterwards. @c false means the
	 *         snapshot predates the buffered events and a newer one is needed.
	 */
	bool on_snapshot(const market_data::book_snapshot &snapshot) {
		++report_.snapshots;
		const core::chrono::ingress_time arrival = snapshot.ingress;
		feed_watch_.beat(clock_.now_ns());

		const auto venue_ns =
			static_cast<std::uint64_t>(snapshot.event_time.count());

		feed_.clear();
		const bool live = bridge_.on_snapshot(snapshot, feed_);
		// The oldest thing this resync is a reaction to. A snapshot that
		// bridged buffered events puts them into the book too, and the first of
		// those arrived before the fetch was even issued - so measuring from
		// the snapshot's own arrival would time the cheap half of a resync and
		// report it as the whole.
		const core::chrono::ingress_time replayed =
			bridge_.reconstructor().last_replay_ingress();

		submit_feed();
		inject();
		quote(venue_ns);
		pump();
		record_resync_reaction(older_of(replayed, arrival));
		return live;
	}

	/// @brief Whether the replica wants a snapshot fetched for it.
	[[nodiscard]] bool needs_snapshot() const noexcept {
		return bridge_.needs_snapshot();
	}

	/// @brief A fetch is in flight; do not ask again until it lands.
	void snapshot_requested() noexcept { bridge_.snapshot_requested(); }

	/// @brief The fetch did not land.
	void snapshot_failed() noexcept { bridge_.snapshot_failed(); }

	/**
	 * @brief The stream was rebuilt, so the replica is stale - withdraw the
	 *        depth it had seeded.
	 *
	 * Not optional and not tidiness: liquidity seeded from a dead replica is no
	 * longer evidence about the venue, and leaving it resting means matching
	 * this process's orders against a snapshot of the past. Orders the quoter
	 * originated are untouched; only the anonymous depth goes. @see
	 * depth_feed_bridge
	 */
	void invalidate() {
		++report_.invalidations;
		// Same reasoning as the gap above, and it has to be here too: a rebuilt
		// stream withdraws the seeded depth, so the liquidity every estimate
		// was measured against is gone whether a sequence number said so or
		// not.
		fills_.reset_queue();
		feed_.clear();
		bridge_.invalidate(feed_);
		submit_feed();
		pump();
	}

	// --- what run_user_data_feed calls: the user_data_handler surface -------

	/**
	 * @brief Apply one execution report from the venue's account stream.
	 *
	 * @param report What the venue said about one of our orders.
	 *
	 * @par Which reports are routed, and which are only counted
	 * A report is routed into the feedback path when it carries something the
	 * engine could not know for itself, and only then. Two kinds do not:
	 *
	 *   * an **acknowledgement** confirms the PLACE the partition has already
	 *     applied and published as @c ACCEPTED;
	 *   * a **cancellation** confirms the CANCEL it has already applied and
	 *     published as @c CANCELLED.
	 *
	 * Routing those again would deliver every transition twice. The gate would
	 * survive it - retiring a ledger entry that is already gone finds nothing -
	 * but the post-trade monitor would not: its order-to-trade rule counts
	 * *our* messages, and doubling them makes a ratio about quoting churn a
	 * statement about how chatty the venue is.
	 *
	 * A **fill**, a **rejection** and an **expiry** are all new information. The
	 * engine produces no fills at all on this path - depth mirrored from the
	 * venue is rested with @c add_order, which does not match - so the account
	 * stream is the only place an execution can come from, and a rejection is
	 * the venue disagreeing with a PLACE the engine accepted.
	 *
	 * @par What is not closed, and is deliberately left open
	 * A venue-initiated cancellation - one this process did not ask for - is
	 * counted as a confirmation and the engine's book keeps the order. The
	 * engine's book is this process's record of what it *decided*; the venue's
	 * is a record of what it *accepted*. @c session::reconcile is what compares
	 * them, and forcing them to agree here would mean the frame path deciding,
	 * from one message, that an order it never withdrew is gone.
	 *
	 * @note Producer thread, like everything but @c drain_and_publish. The
	 *       account stream's coroutine runs on the same io_context as the depth
	 *       feed's, which is what makes it safe to reach straight into the
	 *       feedback router from here. @see the file header.
	 */
	void on_report(const venue::execution_report &report) {
		++report_.venue_reports;

		const auto outcome = to_outcome(report, *spec_);
		if (!outcome) {
			++report_.venue_unusable;
			return;
		}

		if (report.kind == venue::execution_kind::acknowledgement ||
			report.kind == venue::execution_kind::cancellation) {
			++report_.venue_confirmations;
			return;
		}

		// Trades before outcomes, which is the order the partition publishes in
		// and the order the gate's two hooks assume: `on_trade` moves the
		// position and takes the quantity out of the ledger, and `on_outcome`
		// retires whatever is left. Reversed, a terminal FILL would retire an
		// entry whose quantity had not yet been applied to the position.
		if (report.has_fill()) book_fill(report, outcome->id);

		const std::array<engine::order_outcome, 1> one{*outcome};
		(void)hooks_.on_outcomes(spec_->id(), one);
		++report_.venue_booked;
	}

	/**
	 * @brief The account stream broke, so reports between the drop and now were
	 *        never delivered.
	 *
	 * @param reason What the pipeline said, for the log.
	 *
	 * Trips the breaker to @c CANCEL_ONLY when anything of ours is working,
	 * and that is not a precaution - it is the one condition where continuing
	 * to quote is unsound. A gap in *market data* loses public information the
	 * next snapshot restores in full; a gap here loses reports about our own
	 * orders, and nothing replays them. The position this session believes it
	 * holds is then a number it can no longer justify, and every size the gate
	 * screens is measured against it.
	 *
	 * @c CANCEL_ONLY rather than @c HALTED, because the right thing to do with
	 * an unknown position is to reduce it: cancels still pass. Re-arming is an
	 * operator's decision, taken after a reconciliation, which is exactly what
	 * @c trip_cause::STALE_WORKING already means - "orders working and the
	 * order return path silent".
	 *
	 * @note Nothing is tripped when nothing is working. A stream that drops
	 *       while this session is flat has lost reports about no orders, and
	 *       halting a run over it would make an ordinary reconnect fatal.
	 */
	void on_gap(std::string_view reason) {
		++report_.venue_gaps;
		if (gate_.working_orders() == 0) return;
		breaker_.trip(risk::hooks::system::trading_state::CANCEL_ONLY,
					  risk::hooks::system::trip_cause::STALE_WORKING);
		(void)reason;
	}

	/**
	 * @brief Write a cancel for every order this session still has working.
	 *
	 * @return Cancels submitted. Zero when nothing was working, and zero on a
	 *         run that never had a gateway - there is nothing at a venue to
	 *         withdraw.
	 *
	 * @par Why a run owes this
	 * Because the alternative is quotes resting in a venue's book with the
	 * process that priced them gone. They do not expire: a GTC order outlives
	 * the strategy, and the next thing to happen to it is a fill nobody is
	 * watching for, against a position nobody is managing. The risk gate
	 * already argues this from the other end - it refuses to block a cancel
	 * even under an open breaker, because "the moment a strategy most needs to
	 * pull its orders is the moment it has been sending the most".
	 *
	 * @par What it asks, and what it does not
	 * The *quoter* for what it has live, rather than the gate's ledger. The
	 * ledger is the general answer and this is the honest one: the ledger holds
	 * what the gate screened, which on this path includes orders the gateway
	 * then refused to send. Cancelling those would name ids the venue never
	 * saw. What the quoter believes it has live is what was actually written.
	 *
	 * Orders from an *earlier* run of this process are not withdrawn and cannot
	 * be: their ids are gone with the process that chose them. @c
	 * exchange_tool @c account is what finds those, and @c reconcile is what
	 * classifies them.
	 *
	 * @note Producer thread, and it goes through the gate like everything else
	 *       - so a HALTED breaker refuses even this, which is what HALTED
	 *       means. The refusal is counted by the gate and the caller can see it
	 *       in @c gate().refused().
	 */
	std::size_t withdraw_all() {
		if (!orders_.is_sending()) return 0;

		std::vector<command> cancels;
		for (const side_t side : {side_t::bid, side_t::ask})
			if (const order_id_t id = quoter_.live_order(side); id != 0)
				cancels.push_back(command::cancel(spec_->id(), id));
		if (cancels.empty()) return 0;

		// The same lossless retry every other submission here uses, and it
		// needs the consumer still running to drain the queue - which is why
		// the caller withdraws *before* it stops the matching thread.
		while (!gate_.submit_range(cancels)) {
			++report_.stalls;
			pump();
			std::this_thread::yield();
		}
		report_.venue_withdrawn += cancels.size();
		return cancels.size();
	}

	// --- what the order shipper calls --------------------------------------

	/// @brief Whether any request is waiting to go to the venue.
	[[nodiscard]] bool has_outbound() const noexcept {
		return orders_.has_outbound();
	}

	/// @brief Take every request waiting, leaving none. @see
	///        order_router::take_outbound
	[[nodiscard]] std::vector<outbound_request> take_outbound() {
		return orders_.take_outbound();
	}

	/**
	 * @brief The venue refused a placement outright, so tell the engine.
	 *
	 * @param id The order it declined.
	 *
	 * @par Why this cannot wait for the account stream
	 * Because there is nothing coming. Every other thing that happens to an
	 * order is reported on the account stream and reaches @ref on_report - but
	 * a placement the venue *refused* never became an order, so it has no
	 * lifecycle to report and no id the venue recognises. The HTTP response is
	 * the only notification there will ever be, and this is where it lands.
	 *
	 * Without it the gate goes on counting an order as working that the venue
	 * declined at the door: every later size is screened against exposure that
	 * does not exist, and the only thing that could ever correct it is a
	 * reconciliation read nobody has been given a reason to run.
	 *
	 * @note Placements only. A refused *cancel* is a different thing entirely -
	 *       the order it named may have filled in the meantime, and reporting
	 *       that as rejected would retire a live position from the ledger. A
	 *       cancel that did not land is retried by the quoter's next requote.
	 */
	void on_send_refused(order_id_t id) {
		++report_.venue_refused;
		const std::array<engine::order_outcome, 1> one{engine::order_outcome{
			.id     = id,
			.type   = engine::OutcomeType::REJECTED,
			.reason = engine::reject_reason::VENUE_REJECTED,
			.status = engine::OrderStatus::REJECTED,
			// Nothing traded and nothing left working: an order the venue never
			// accepted has no quantity in either place.
			.traded    = 0,
			.remaining = 0}};
		(void)hooks_.on_outcomes(spec_->id(), one);
	}

	/**
	 * @brief Adopt the venue's own rate-limit count from a response.
	 *
	 * Forwarded rather than reached through, so the shipper never has to know
	 * whether a gateway exists. A run without one has no headers to hand over
	 * and this is never called.
	 */
	void observe_venue(std::span<const transport::rest::header> headers,
					   venue_gateway::time_point now) {
		if (venue_gateway *gateway = orders_.gateway(); gateway != nullptr)
			gateway->observe(headers, now);
	}

	// --- what the producer's loop calls ------------------------------------

	/**
	 * @brief Route whatever the engine has published, and check the watchdogs.
	 * @return Events routed on this call.
	 *
	 * Called after every frame, and from inside every wait - see the file
	 * header on why a wait that does not pump deadlocks. Cheap when there is
	 * nothing to do: an empty dequeue, a subtraction and two compares.
	 */
	std::size_t pump() {
		const std::size_t routed = dispatch_.pump();
		report_.engine_events += routed;

		const std::uint64_t now = clock_.now_ns();
		// Both watchdogs are polled rather than fired, for the same reason:
		// what each of them watches is an *absence*, and an absence delivers no
		// callback. A loop that never polls them never trips them, which is
		// stated in both their headers rather than left to be discovered.
		feed_watch_.poll(now);
		hooks_.poll();
		return routed;
	}

	/// @brief Drain the channel until it is empty. The shutdown path: whatever
	///        the engine published on its last cycle still has to reach the
	///        gate, or a report reads a position the run did not end with.
	std::size_t pump_all() {
		const std::size_t routed = dispatch_.pump_all();
		report_.engine_events += routed;
		return routed;
	}

	// --- what the consumer thread calls, and the only thing it may ---------

	/**
	 * @brief One turn of the engine's cycle: match what was submitted, publish
	 *        what that produced.
	 * @return Commands applied on this turn; zero means the queue was empty.
	 *
	 * @warning Consumer thread only. Every other member of this class belongs
	 *          to the producer. @see the file header's threading contract.
	 *
	 * The three steps are the order @c event_channel documents and they cannot
	 * be reordered: drain fills the partition's buffers, publish stages and
	 * pushes them, flush clears them for the next turn. Publishing after the
	 * flush would publish nothing; flushing before the publish would drop a
	 * batch on the floor.
	 */
	std::size_t drain_and_publish() {
		const std::size_t applied = partition_.drain();
		if (applied == 0) return 0;

		if (!channel_.publish(partition_.runs(),
							  partition_.trades(),
							  partition_.outcomes())) {
			// The producer is what empties this ring, and it pumps on every
			// frame and inside every wait, so this clears in bounded time. A
			// yield rather than a spin: the producer may be on a hyperthread
			// sibling and burning its issue slots does not help it read faster.
			while (!channel_.retry()) std::this_thread::yield();
		}
		partition_.flush();
		return applied;
	}

	// --- what an operator and a report read --------------------------------

	/// @brief The listing this session trades.
	[[nodiscard]] symbol_id_t symbol() const noexcept { return spec_->id(); }

	/// @brief The policy in force.
	[[nodiscard]] const live_session_options &options() const noexcept {
		return options_;
	}

	/// @brief What the session did. @see live_session_report
	[[nodiscard]] const live_session_report &report() const noexcept {
		return report_;
	}

	/// @brief The pre-trade gate, for its refusal and breach counters.
	[[nodiscard]] const gate_type &gate() const noexcept { return gate_; }

	/// @brief The post-trade monitor, for its three rules' counters.
	[[nodiscard]] const monitor_type &monitor() const noexcept {
		return watch_;
	}

	/// @brief The shared kill switch, for its state and cause.
	[[nodiscard]] const risk::hooks::system::circuit_breaker &
	breaker() const noexcept {
		return breaker_;
	}

	/**
	 * @brief The same switch, to throw.
	 *
	 * For a path that has learned something no rule inside this session can:
	 * the venue answering a placement with a ban, an operator's console. A
	 * mutable accessor rather than a @c halt method because @c circuit_breaker
	 * already spells the vocabulary - state and cause - and a second wording of
	 * it here would be a second thing to find in an incident. @see the class
	 * note on why its state is a real atomic.
	 */
	[[nodiscard]] risk::hooks::system::circuit_breaker &breaker() noexcept {
		return breaker_;
	}

	/// @brief The market_data watchdog, for its silence and trip counts.
	[[nodiscard]] const risk::hooks::system::heartbeat_monitor &
	feed_watchdog() const noexcept {
		return feed_watch_;
	}

	/// @brief The reference quoter, for what it has shown and had filled.
	[[nodiscard]] const quoter_type &quoter() const noexcept { return quoter_; }

	/// @brief The order path out to the venue, for what it queued and what it
	///        could not. Inert - and every counter zero - without a gateway.
	[[nodiscard]] const order_router_type &router() const noexcept {
		return orders_;
	}

	/// @brief The passive-fill inference, for what it injected and what it
	///        believes is still queued. Inert unless @c simulate_fills.
	/// @brief The modelled network. @see latency_pipe
	[[nodiscard]] const pipe_type &pipe() const noexcept { return pipe_; }

	[[nodiscard]] const fill_model_type &fills() const noexcept {
		return fills_;
	}

	/// @brief The bridge, for the replica and its command counters.
	[[nodiscard]] const strategy::backtest::depth_feed_bridge &
	bridge() const noexcept {
		return bridge_;
	}

	/// @brief The position and exposure every limit is measured against.
	[[nodiscard]] const risk::hooks::pre_trade::position_book &
	positions() const noexcept {
		return positions_;
	}

	/// @brief The clock the local windows are measured on, so a caller driving
	///        a session from recorded time can move it. @see
	///        core::chrono::nanosecond_clock
	[[nodiscard]] Clock &clock() noexcept { return clock_; }

	/// @brief The venue replica the quoter is quoting around.
	[[nodiscard]] const market_data::l2_book &replica() const noexcept {
		return bridge_.replica();
	}

	/// @brief Whether the replica is in sequence with the venue.
	[[nodiscard]] bool is_alive() const noexcept { return bridge_.is_alive(); }

	/**
	 * @brief The partition, for its books, records and metrics.
	 *
	 * @warning Reading this from the producer thread races the consumer, which
	 *          is why it is only ever called after the consumer has been
	 *          joined. The engine's own counters are the report's business, not
	 *          a live console's.
	 */
	[[nodiscard]] const partition_type &partition() const noexcept {
		return partition_;
	}

private:
	// --- the venue's fills -------------------------------------------------

	/**
	 * @brief Move the position, the PnL and the fat-finger band by what the
	 *        venue actually executed.
	 *
	 * @param report A report whose @c has_fill is true.
	 * @param id The engine order it names, already parsed by @c to_outcome.
	 *
	 * @par Why this is a trade, when venue_bridge refuses to make one
	 * Because the gate books executions through @c on_trade and nowhere else -
	 * an @c order_outcome names quantities and a status, and only a @c trade
	 * carries the *price* something executed at. A FILL outcome alone moves
	 * nothing, so a session that routed one and stopped would report fills it
	 * had not booked.
	 *
	 * @c venue_bridge still declines to build this, and the two positions are
	 * consistent rather than in tension. It is a translation with no context: it
	 * produces values a caller may journal, and a fabricated order id on the
	 * event stream is a fact about an order that never existed. This is the
	 * composition root, it knows the counterparty is anonymous *because the
	 * venue does not name it*, and what it builds goes to the gate, the quoter
	 * and a logger - nothing that persists it. @see venue_bridge.hpp
	 *
	 * @par The empty side
	 * Zero, which is this tree's "no order" throughout - @c spread_quoter tests
	 * @c id != 0 for a live order and hands out ids from @c ++next_id_, and an
	 * ADD carries none at all. So @c working_ledger::take finds nothing for it
	 * and the execution applies exactly once, to our side. Naming our own id in
	 * *both* slots would apply it twice, which is how a self-trade is meant to
	 * net to zero and is the wrong arithmetic entirely for a fill against
	 * somebody else.
	 *
	 * Which slot we occupy comes from the venue rather than from an assumption:
	 * @c is_maker says whether our order was the resting one. It changes
	 * nothing the gate does - both slots are looked up the same way - and it is
	 * set correctly because a log line and a post-trade rule reading this
	 * should not be told we aggressed when we did not.
	 */
	void book_fill(const venue::execution_report &report, order_id_t id) {
		const auto lots = lots_from(report.last_qty_scaled, *spec_);
		if (!lots || *lots <= 0) return;
		// Off the tick grid means the report is about a listing whose reference
		// data we have wrong, and marking a position at a made-up price is
		// worse than not marking it. Counted where every other unusable report
		// is.
		const auto price = spec_->price_from_scaled(report.last_price_scaled);
		if (!price) {
			++report_.venue_unusable;
			return;
		}

		const std::array<engine::trade, 1> filled{
			engine::trade{.aggressor = report.is_maker ? 0 : id,
						  .resting   = report.is_maker ? id : 0,
						  .price     = *price,
						  .volume    = *lots}};
		(void)hooks_.on_trades(spec_->id(), filled);
		report_.venue_filled_lots += static_cast<volume_t>(*lots);
	}

	// --- reaction time -----------------------------------------------------

	/// @brief The older of two stamps, disregarding either that was never
	///        taken. Two unstamped inputs give an unstamped answer.
	[[nodiscard]] static core::chrono::ingress_time
	older_of(core::chrono::ingress_time first,
			 core::chrono::ingress_time second) noexcept {
		if (!core::chrono::has_ingress(first)) return second;
		if (!core::chrono::has_ingress(second)) return first;
		return std::min(first, second);
	}

	/**
	 * @brief Close a reaction interval that began at @p arrival.
	 *
	 * @param arrival When the message being reacted to landed on this box.
	 * @param into Where the sample goes. Frames and resyncs are kept apart -
	 *        @see reaction_metrics::resync_reaction_ns
	 *
	 * @pre @c options_.reaction is not null - the callers below check, so the
	 *      unmeasured configuration never reaches a histogram reference it has
	 *      no histogram for.
	 */
	void record_reaction(core::chrono::ingress_time arrival,
						 core::metrics::histogram &into) noexcept {
		if (!core::chrono::has_ingress(arrival)) {
			// A replayed capture, a scripted test event, an offline snapshot.
			// Counted so an empty distribution cannot be misread as a fast one.
			options_.reaction->unstamped.increment();
			return;
		}

		const auto elapsed = core::chrono::ingress_clock::now() - arrival;
		if (elapsed.count() < 0) {
			// Unreachable from one monotonic clock, so it means the stamp came
			// from a different one. Counted rather than clamped to zero: a
			// fabricated sample is indistinguishable from a real one once it is
			// in a bucket, and a floor of zero would flatter the p50.
			options_.reaction->unstamped.increment();
			return;
		}
		into.record(static_cast<std::uint64_t>(elapsed.count()));
	}

	/// @brief Ingress of one frame to the commands it caused. @see
	///        reaction_metrics::frame_reaction_ns
	void record_frame_reaction(core::chrono::ingress_time arrival) noexcept {
		if (options_.reaction == nullptr) return;
		record_reaction(arrival, options_.reaction->frame_reaction_ns);
	}

	/// @brief Ingress of the oldest message a resync reacted to, to the
	/// commands
	///        it caused. @see reaction_metrics::resync_reaction_ns
	void record_resync_reaction(core::chrono::ingress_time arrival) noexcept {
		if (options_.reaction == nullptr) return;
		record_reaction(arrival, options_.reaction->resync_reaction_ns);
	}

	/**
	 * @brief Hand the bridge's commands to the gate, retrying until they land.
	 *
	 * The retry is the lossless half of back-pressure and the pump inside it is
	 * what keeps the retry finite. @c risk_gate::submit_range is idempotent
	 * under a refusal - it rolls its ledger back and re-screens - so handing it
	 * the identical batch again is the documented way to wait.
	 *
	 * @note A gate *refusal* is not a stall and does not come back here: the
	 *       gate drops what it refused, delivers the rest and returns @c true.
	 *       Only the sink pushing back produces a @c false, which is why the
	 *       counter below is named for saturation rather than for risk.
	 */
	void submit_feed() {
		if (feed_.empty()) return;
		report_.depth_commands += feed_.size();
		while (!gate_.submit_range(feed_)) {
			++report_.stalls;
			pump();
			std::this_thread::yield();
		}
		feed_.clear();
	}

	/// @brief Let the quoter look at the venue's book, and deliver what it
	///        wrote.
	///
	/// Skipped outright while the replica is not live. A quoter quoting around
	/// a book that has fallen out of sequence is quoting around history, and
	/// the depth it would be quoting *inside* has already been withdrawn from
	/// the engine's book by the bridge.
	/// @par Why the retry delivers as well as pumps
	/// Because with a wire in the chain there are two different reasons a flush
	/// can be refused, and only one of them is relieved by pumping. A full
	/// partition queue is: the consumer drains it and room appears. A full
	/// *wire* is not - the only thing that frees a slot there is @c
	/// deliver_due, and the timer that would call it cannot run while this loop
	/// is holding the io_context's thread. Without this line that is a deadlock
	/// rather than back-pressure: the producer spins forever waiting for a wire
	/// nothing is allowed to drain.
	///
	/// With it the loop makes progress as wall-clock time passes - the commands
	/// in flight come due, are handed over, and their slots free. That is a
	/// busy wait, and it is the honest shape of the situation: the strategy is
	/// writing faster than the modelled network can carry, so it waits for the
	/// network. @c latency_model::max_in_flight is what bounds how much of ours
	/// may be outstanding before that happens.
	void quote(std::uint64_t venue_ns) {
		if (!bridge_.is_alive()) return;
		quoter_.on_market(bridge_.replica(), venue_ns);
		while (!quoter_.flush()) {
			++report_.stalls;
			// Both, and neither is redundant. Pumping relieves a refusal that
			// came from a full partition queue - the consumer cannot drain
			// while the channel it publishes into is backed up, which is the
			// deadlock this file's header warns about, and it is the only
			// reason a flush is refused when no wire is configured. Delivering
			// relieves the other one. `deliver_due` pumps only when it actually
			// handed something over, so the unconditional pump has to stay.
			(void)deliver_due();
			pump();
			std::this_thread::yield();
		}
	}

	/**
	 * @brief Infer what the venue's depth must have traded against our resting
	 *        orders, and submit it.
	 *
	 * Runs after the frame's depth is in flight and **before** the quoter
	 reacts
	 * to it, and that ordering is the whole correctness of this function rather
	 * than a preference.
	 *
	 * @par Why not after the quoter
	 * Because there would then be nothing left to fill. The quoter requotes off
	 * every frame, and the gate inserts a new order into its ledger at screen
	 * time - so a model reading the ledger after @c quote sees our bid already
	 * moved inside the touch that has just arrived, and an order inside the
	 * touch is by construction never traded through. Passive fills would come
	 * out at exactly zero, which is the answer this whole path exists to stop
	 * being an artefact.
	 *
	 * Run first, the ledger still holds the orders the *book* holds: the ones
	 * placed on earlier frames, which is what the venue's new depth would
	 * actually have traded against. The queue order that follows is the real
	 * one too - depth, then the aggressor, then the cancel and replace - so the
	 * aggressor reaches the stale quote before the requote retires it, which is
	 * precisely the race a resting order loses on a live venue.
	 *

	 * @par Where this differs from the offline harness, and why it has to
	 * The harness runs @c infer inside a settle loop, repeatedly, until a round
	 * changes nothing: offline it can apply a command and observe the result
	 * before the event is over, so a fill that makes the quoter requote can be
	 * inferred against in the same frame. Here the engine is a second thread
	 and
	 * there is no such point. @c infer therefore runs exactly once per frame,
	 * and a fill it infers is applied by the consumer whenever it gets there -
	 * one frame later, or several under load.
	 *
	 * That gap is not a shortcoming of this function. It is the gap a
	 deployment
	 * has, and closing it would mean the producer waiting on the consumer once
	 * per frame, which is the one thing the two-thread split exists to avoid.
	 *
	 * @note @c open_step is called here rather than beside @c submit_feed,
	 where
	 *       the harness calls it. With one @c infer per frame the per-event
	 *       liquidity budget is released and spent in the same breath, so it is
	 *       inert live - it exists to stop a *settle loop* filling twice
	 against
	 *       depth that did not move. Kept because the model's contract asks for
	 *       it and a future frame-local retry would need it to be honest.
	 */
	void inject() {
		if (!options_.simulate_fills) return;
		// A replica out of sequence is not evidence about the venue, and the
		// depth it seeded has already been withdrawn from the book by the
		// bridge. Inferring against it would be inferring against history.
		if (!bridge_.is_alive()) return;

		++report_.inferred_frames;
		fills_.open_step();
		injected_.clear();
		if (fills_.infer(bridge_.replica(),
						 ledger_view{gate_.ledger()},
						 injected_) > 0)
			// Straight to the partition, past the gate. These are the *venue's*
			// orders: screening them would charge our rate limit for somebody
			// else's flow, and recording them in the ledger would have the
			// model inferring fills against its own injections next frame.
			while (!partition_.submit_range(injected_)) {
				++report_.stalls;
				pump();
				std::this_thread::yield();
			}

		fills_.retire_finished(ledger_view{gate_.ledger()});

		// Assigned, not accumulated: all three are running totals the model
		// keeps, and adding them would count every earlier frame again.
		report_.injected_aggressors = fills_.injected();
		report_.injected_lots       = fills_.injected_lots();
		report_.queue_absorbed_lots = fills_.queue().absorbed_lots();
	}

	const engine::symbol_spec *spec_;
	live_session_options options_;
	clock_type clock_;

	// Declaration order is construction order and every line of it matters:
	// the gate holds the partition, the quoter holds the gate, the fan-out
	// holds both, the router holds the fan-out, and the dispatcher holds the
	// channel and the router.
	partition_type partition_;
	channel_type channel_;
	risk::hooks::pre_trade::position_book positions_;
	risk::hooks::system::circuit_breaker breaker_;
	fill_model_type fills_;
	pipe_type pipe_;
	order_router_type orders_;
	gate_type gate_;
	quoter_type quoter_;
	monitor_type watch_;
	fanout_type fanout_;
	router_type hooks_;
	dispatcher_type dispatch_;
	risk::hooks::system::heartbeat_monitor feed_watch_;
	strategy::backtest::depth_feed_bridge bridge_;

	/// @brief The bridge's output, reused. Cleared per frame and never grown
	///        after the first few, so a steady-state frame allocates nothing.
	std::vector<command> feed_;

	/// @brief The fill model's output, reused on the same terms as @c feed_.
	///        Never touched unless @c simulate_fills. @see inject
	std::vector<command> injected_;

	live_session_report report_{};
};

} // namespace exchange::session
