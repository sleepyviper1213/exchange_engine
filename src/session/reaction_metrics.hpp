#pragma once
// Reaction time: from a venue message landing on this box to the commands it
// caused reaching the engine.
//
// Every other latency number in this tree is a *component* number - the gate's
// per-order cost, the partition's per-batch drain. Each one is measured at the
// boundary of the subsystem that owns it, which is what makes them defensible
// and also what makes them silent about the thing a trading system is actually
// judged on. docs/performance.md says so itself: the targets are "not
// end-to-end wire-to-wire latency, which is tracked separately". This is that
// separate track.
//
// --- what is inside the interval, and what is not -------------------------
//
// Inside: the coroutine resume after the socket read, simdjson decoding the
// frame, normalisation, the sequencer's verdict, l2_book's writes, the bridge's
// ADD/REDUCE, the quoter's decision, the risk gate, and the SPSC enqueue.
//
// Outside, and unmeasurable from in here: the venue's own matching-to-publish
// delay, the wire, and the kernel's receive path up to the point Beast handed
// us a frame. A local monotonic clock cannot see any of them - the first two
// need the venue's clock and the third needs the NIC's. So this is
// *ingress-to-egress*, which is the half of wire-to-wire this process is
// responsible for and the only half it can improve.
//
// --- why the interval ends where it does ----------------------------------
//
// At the SPSC enqueue, not at the fill. The engine runs on the other thread and
// answers through the event channel, so waiting for it would fold the
// consumer's scheduling into a number about the producer's reaction, and a
// stalled consumer would show up as slow decision-making. Where the command
// crosses the queue is the last instant this thread controls, so it is where
// this thread's reaction is over.

#include "core/metrics/counter.hpp"
#include "core/metrics/histogram.hpp"

namespace exchange::session {

/**
 * @brief Where a live session records what its reaction time was.
 *
 * Optional, like @c execution::partition_metrics and for the same reason: a
 * deployment that never asked for metrics should pay nothing, so a session
 * holds a pointer and null means "do not measure". Two clock reads per frame is
 * nothing against a 100 ms diff stream, but it is not nothing against a binary
 * feed, and the shape that lets this be switched off is the shape that survives
 * one.
 *
 * @warning Single-writer, like every metric in @c core::metrics: these are
 *          written by the producer thread alone. @c read() from an operator's
 *          console is fine.
 */
struct reaction_metrics {
	/**
	 * @brief Frame ingress to commands enqueued, in nanoseconds.
	 *
	 * The headline number. One observation per frame that carried an ingress
	 * stamp, whether or not it produced any commands - a frame the strategy
	 * declined to act on still cost the time it took to decide that, and
	 * dropping those observations would report only the expensive frames.
	 */
	core::metrics::histogram frame_reaction_ns;

	/**
	 * @brief Resync ingress to commands enqueued, in nanoseconds.
	 *
	 * Kept apart from @c frame_reaction_ns rather than pooled with it, because
	 * the two have different floors and pooling them would bury the interesting
	 * one. A resync's interval starts at the *oldest* thing being reacted to -
	 * the first buffered event the snapshot bridged, which arrived a REST round
	 * trip ago - so these samples are expected to be orders of magnitude
	 * larger. Mixed into one distribution they would move the p99 of a metric
	 * whose p99 is the point. @see
	 * market_data::depth_reconstructor::last_replay_ingress
	 */
	core::metrics::histogram resync_reaction_ns;

	/**
	 * @brief Messages that carried no ingress stamp, so were not timed.
	 *
	 * The counter that stops a silent zero being read as a fast run. A replayed
	 * capture and a scripted test event both legitimately arrive unstamped, and
	 * without this an empty histogram cannot be told from a pipeline that never
	 * reacted to anything.
	 */
	core::metrics::counter unstamped;
};

} // namespace exchange::session
