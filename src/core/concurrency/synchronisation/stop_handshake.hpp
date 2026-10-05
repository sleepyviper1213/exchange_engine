#pragma once

#include "core/util/function_ref.hpp"
#include "core_export.hpp" // CORE_EXPORT (generated)

#include <atomic>
#include <cstddef>

namespace exchange::core::concurrency::synchronisation {

/**
 * @brief Ends a producer/consumer pair whose consumer also publishes back to
 *        the producer, without losing work or deadlocking.
 *
 * Two flags, one per direction, and each one is there because the shutdown it
 * replaced got that direction wrong.
 *
 * @par The stop flag is release/acquire, because it marks the end of the data
 * The consumer's last drain promises to apply everything the producer
 * submitted before it stopped. A relaxed flag cannot keep that promise: the
 * queue's write cursor and the flag are different atomics, so reading the flag
 * as set orders nothing, and the drain that follows may legally load a stale
 * cursor, find the queue empty and exit with commands still in it. Release on
 * the store makes every submit before it happen-before the consumer's acquire
 * of the flag, and read coherence then forces the next cursor load to see at
 * least that last submit. x86 never reorders two loads, so the relaxed version
 * cannot fail on the dev box; ARM can, and does not have to tell anyone.
 * ThreadSanitizer cannot see this either - every access is atomic, so there is
 * no data race to report, only an ordering that admits a wrong answer.
 *
 * @par The producer pumps until the consumer has finished, rather than joining
 * The consumer's final drain can publish more than the return ring has room
 * for, and then it waits for the producer to empty the ring. A producer blocked
 * in @c std::thread::join is not emptying anything, so the two wait on each
 * other and the process never exits - the same two-ring deadlock every other
 * wait on the producer side avoids by pumping, reached at the one wait that did
 * not. @c stop keeps pumping until @c run has returned, so by the time the
 * caller joins there is nothing left to wait for.
 *
 * @par Threading contract
 * One consumer thread calls @c run, once. One producer thread calls @c stop,
 * once, after its last submit. Neither is reusable: a second session builds a
 * second handshake.
 *
 * @code
 * stop_handshake handshake;
 * std::thread matching([&] {
 *     handshake.run([&] { return session.drain_and_publish(); });
 * });
 * // ... submit, pump, submit ...
 * handshake.stop([&] { return session.pump_all(); });
 * matching.join();
 * @endcode
 */
class stop_handshake {
public:
	/**
	 * @brief Consumer side: run @p turn until stopped, then until it reports no
	 *        work, then say so.
	 * @param turn One drain-and-publish; returns how much it applied. Zero
	 *        means it found nothing, which ends the final drain.
	 * @param idle Called after a turn that found nothing, before the stop.
	 */
	CORE_EXPORT void run(core::util::function_ref<size_t() const noexcept> turn,
						 core::util::function_ref<void() const noexcept> idle);

	/// @brief @c run with a yielding idle.
	CORE_EXPORT void
	run(core::util::function_ref<size_t() const noexcept> turn);

	/**
	 * @brief Producer side: ask the consumer to stop, and keep emptying the
	 *        return path until it has.
	 * @param pump Drains whatever the consumer has published; returns how much
	 *        it moved, and zero when there was nothing.
	 * @post @c run has returned. What it published on its way out may still be
	 *       in the return ring - one more pump after the join collects it.
	 */
	CORE_EXPORT void
	stop(core::util::function_ref<size_t() const noexcept> pump);

	/// @brief Whether @c run has returned. Producer side.
	[[nodiscard]] CORE_EXPORT bool has_finished() const noexcept;

private:
	std::atomic<bool> stopping_{false};
	std::atomic<bool> finished_{false};
};

} // namespace exchange::core::concurrency::synchronisation
