#pragma once
// Heartbeat / Disconnect Hook: silence from the venue.
//
// The failure every other hook in this directory is blind to. A dead feed does
// not breach a limit, does not lose money and does not send anything wrong - it
// simply stops, and every rule that measures an order against the market goes
// on answering from a mark that is minutes old. The fat-finger collar is the
// clearest case: it is centred on the last print, so once prints stop it is
// centred on history, and it will happily admit an order at a price that no
// longer exists.
//
// So this is a watchdog, and the thing it watches is the absence of events.

#include "circuit_breaker.hpp"
#include "risk_management_export.hpp" // RISK_MANAGEMENT_EXPORT (generated)

#include <cstdint>

namespace exchange::risk::hooks::system {

/**
 * @brief Trips the breaker when the venue has been quiet for too long.
 *
 * @par What counts as a beat
 * Any evidence the venue is alive: a depth diff, a trade print, a
 * protocol-level ping, an empty keep-alive frame. Whoever reads the socket
 * calls @c beat; this type deliberately does not know what a frame is, because
 * knowing would point an edge from @c risk_management at @c transport or @c
 * market_data, and risk sits beside those rather than above them.
 *
 * @par Time arrives as an argument, the way it does everywhere else here
 * @c rate_limiter and @c circuit_breaker are both handed @c now_ns rather than
 * owning a clock, and this follows them. Not only for consistency: the caller
 * has *just* read a clock to notice the frame it is reporting, so reading one
 * again here would re-derive a number already in a register - and a monitor
 * that owned its clock would have to be a template for a backtest to drive it
 * from event time.
 *
 * @par Why the reading must be local and monotonic, never a venue timestamp
 * A frame's exchange timestamp says when the venue *sent* something, which is
 * the one thing a dead connection cannot tell you: the last frame's timestamp
 * stops advancing whether the venue went quiet or the link died, and those are
 * the same emergency from here. Feeding venue time in would compare two
 * unrelated timelines and produce either a permanent trip or none at all.
 *
 * @par Why it trips rather than refuses
 * Same argument as the drawdown breaker: staleness is not a property of the
 * order in front of it, so refusing that order while accepting the next would
 * be incoherent. It selects @c CANCEL_ONLY, never @c HALTED - a strategy that
 * has lost its market data must stop adding risk, and must absolutely keep the
 * ability to pull the quotes it has already shown.
 *
 * @par A watchdog is polled, not fired
 * Nothing here runs on a timer or a thread. @c poll is called from whatever
 * loop already runs - the feed loop, the dispatcher's pump, an operator's
 * console tick
 * - and a monitor nobody polls never trips, which is a property worth stating
 * plainly rather than discovering. A poll is a subtraction and a compare, so
 * calling it on every pass of the event loop is the intended usage.
 *
 * @par Threading
 * One monitor, one thread: whichever one reads the venue, which is also the one
 * that should poll it. If the reader and the risk loop are separate threads,
 * the reader owns the monitor and the *decision* crosses the boundary through
 * @c circuit_breaker, which is an atomic precisely so that it can. The counters
 * here are plain because they are single-writer, like every other counter in
 * this module.
 */
class heartbeat_monitor {
public:
	/// @brief A monitor that never trips - the disabled configuration, so a
	///        deployment without venue heartbeats spells that rather than
	///        omitting the object and changing its wiring.
	static constexpr std::uint64_t NO_TIMEOUT = 0;

	/**
	 * @brief Watch @p breaker, tripping it after @p timeout_ns of silence.
	 *
	 * @param breaker The shared kill switch. Must outlive the monitor.
	 * @param timeout_ns Silence, in nanoseconds, that means the feed is gone,
	 * or
	 *        @c NO_TIMEOUT to disable. Size it against the venue's own
	 * heartbeat interval rather than against how quiet a market can get: two
	 * missed keep-alives is a diagnosis, whereas "no trades for a second" is a
	 *        Tuesday afternoon.
	 * @param now_ns Counts as the first beat, so a monitor built before the
	 * feed has connected gets its whole timeout to see the first frame instead
	 *        of tripping on silence that predates it.
	 */
	RISK_MANAGEMENT_EXPORT heartbeat_monitor(circuit_breaker &breaker,
											 std::uint64_t timeout_ns,
											 std::uint64_t now_ns) noexcept;

	/// @brief The venue said something at @p now_ns.
	RISK_MANAGEMENT_EXPORT void beat(std::uint64_t now_ns) noexcept;

	/**
	 * @brief Check for silence as of @p now_ns, and trip if there is too much.
	 * @return Whether *this call* is what tripped the breaker.
	 *
	 * @note Does not trip a breaker that is already open, so one outage
	 * produces one trip. It will trip *again* if an operator re-arms while the
	 * feed is still silent, which is deliberate and matches @c
	 * circuit_breaker's own contract: a re-arm into a condition that still
	 * holds does not buy a fresh allowance.
	 */
	RISK_MANAGEMENT_EXPORT bool poll(std::uint64_t now_ns) noexcept;

	/// @brief Whether the feed is past its timeout as of @p now_ns. Always
	///        @c false when disabled.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT bool
	is_silent(std::uint64_t now_ns) const noexcept;

	/// @brief Nanoseconds between the last beat and @p now_ns.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT std::uint64_t
	silence_ns(std::uint64_t now_ns) const noexcept;

	/// @brief The reading the last beat was stamped with.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT std::uint64_t
	last_beat_ns() const noexcept;

	/// @brief The silence this monitor treats as a dead feed, or @c NO_TIMEOUT.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT std::uint64_t
	timeout_ns() const noexcept;

	/// @brief Beats seen since construction - the denominator an operator
	///        compares against the venue's advertised heartbeat rate.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT std::uint64_t beats() const noexcept;

	/// @brief Times this monitor has tripped the breaker.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT std::uint64_t trips() const noexcept;

private:
	circuit_breaker *breaker_;
	std::uint64_t timeout_ns_;
	std::uint64_t last_beat_ns_;
	std::uint64_t beats_ = 0;
	std::uint64_t trips_ = 0;
};

} // namespace exchange::risk::hooks::system