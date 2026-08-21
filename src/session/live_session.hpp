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
//   └────────────────────────────────────────────────────────────┬───────┘
//                                                               │ SPSC queue
//   ┌── consumer thread ─────────────────────────────────────────▼───────┐
//   │            engine_partition ─▶ matching_engine ─▶ order_book       │
//   └────────────────────────────────────────────────────────────┬───────┘
//                                                               │ event_channel
//   ┌── producer thread again ───────────────────────────────────▼───────┐
//   │  event_dispatcher ─▶ feedback_router ─▶ risk_gate + spread_quoter  │
//   │                                     └─▶ post_trade_monitor        │
//   └────────────────────────────────────────────────────────────────────┘
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
//     same reading `backtest::feed_clock` gives it over a capture, so a quoter
//     behaves the same way live as it did in the replay that justified it.

#include "feedback_fanout.hpp"
#include "market-data/l2_book.hpp"
#include "market-data/normalised.hpp"
#include "market-data/reconstructor.hpp"
#include "market-data/sequencer.hpp"
#include "risk_management/clock.hpp"
#include "risk_management/gate.hpp"
#include "risk_management/hooks/feedback.hpp"
#include "risk_management/hooks/post_trade/limits.hpp"
#include "risk_management/hooks/post_trade/monitor.hpp"
#include "risk_management/hooks/pre_trade/position.hpp"
#include "risk_management/hooks/system/circuit_breaker.hpp"
#include "risk_management/hooks/system/heartbeat.hpp"
#include "risk_management/limits.hpp"
#include "strategy/backtest/depth_feed_bridge.hpp"
#include "strategy/quoter.hpp"
#include "trading-engine/event/command.hpp"
#include "trading-engine/event/event_channel.hpp"
#include "trading-engine/event/event_dispatcher.hpp"
#include "trading-engine/execution/book_manager.hpp"
#include "trading-engine/execution/engine_partition.hpp"
#include "trading-engine/execution/order_manager.hpp"
#include "trading-engine/orders/types.hpp"
#include "trading-engine/symbol/symbol_spec.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
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
 *         places. @see nanosecond_clock
 *
 * @note One listing, like everything else on this side of the queue. A second
 *       listing is a second gate, a second monitor and one more slot in the
 *       router - which is exactly why the router is here rather than a direct
 *       call, even though today it routes one symbol. @see feedback_router
 */
template <risk::nanosecond_clock Clock        = risk::steady_nanos,
		  risk::hooks::risk_observer Observer = risk::hooks::no_observer,
		  class Watcher                       = no_watcher>
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
	using gate_type   = risk::risk_gate<partition_type, clock_type, Observer>;
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
		  clock_(clock),
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
		  gate_(partition_, spec.id(), options.limits, positions_, breaker_, 0,
				clock, observer),
		  quoter_(gate_, spec, options.quoting),
		  watch_(breaker_, spec.id(), options.surveillance, clock_.now_ns()),
		  fanout_(gate_, quoter_, watcher),
		  hooks_(static_cast<std::size_t>(spec.id()) + 1U, clock),
		  dispatch_(channel_, hooks_),
		  feed_watch_(breaker_, options.feed_timeout_ns, clock_.now_ns()),
		  bridge_(spec, options.feed) {
		// On the consumer's side of the contract, and before either thread
		// starts: a partition refuses a symbol it was not given rather than
		// inventing a book for it.
		partition_.listing(spec.id());
		hooks_.attach(fanout_, watch_);
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
		if (action == market_data::sequence_action::gap) ++report_.gaps;

		submit_feed();
		quote(venue_ns);
		pump();
		return action;
	}

	/**
	 * @brief Seed or repair the replica from a REST snapshot.
	 * @return Whether the replica is live afterwards. @c false means the
	 *         snapshot predates the buffered events and a newer one is needed.
	 */
	bool on_snapshot(market_data::book_snapshot snapshot) {
		++report_.snapshots;
		feed_watch_.beat(clock_.now_ns());

		const auto venue_ns =
			static_cast<std::uint64_t>(snapshot.event_time.count());

		feed_.clear();
		const bool live = bridge_.on_snapshot(std::move(snapshot), feed_);

		submit_feed();
		quote(venue_ns);
		pump();
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
		feed_.clear();
		bridge_.invalidate(feed_);
		submit_feed();
		pump();
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

	/// @brief The market-data watchdog, for its silence and trip counts.
	[[nodiscard]] const risk::hooks::system::heartbeat_monitor &
	feed_watchdog() const noexcept {
		return feed_watch_;
	}

	/// @brief The reference quoter, for what it has shown and had filled.
	[[nodiscard]] const quoter_type &quoter() const noexcept { return quoter_; }

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
	///        a session from recorded time can move it. @see nanosecond_clock
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
	void quote(std::uint64_t venue_ns) {
		if (!bridge_.is_alive()) return;
		quoter_.on_market(bridge_.replica(), venue_ns);
		while (!quoter_.flush()) {
			++report_.stalls;
			pump();
			std::this_thread::yield();
		}
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

	live_session_report report_{};
};

} // namespace exchange::session
