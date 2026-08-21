#pragma once
// Replay: feed a journal's records back to whoever can apply them.
//
// This is the half of "deterministic is only worth it if you can recover" that
// spends the property. Journalling buys nothing on its own - a log nobody reads
// is a slower engine and nothing else. This reads it.
//
// It is deliberately not a loop the caller could not have written, and the two
// reasons are the two things a naive loop gets wrong. It streams, so a journal
// larger than memory replays without a vector of the whole thing. And it stops
// on the first refusal and says where, because the natural applier during
// recovery is `engine_partition::submit`, whose queue is bounded and which
// therefore *will* refuse - a loop that ignored that would drop commands in the
// middle of the one operation that must not drop any.

#include "core/util/function_ref.hpp"
#include "record_log.hpp"

#include <fmt/format.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <type_traits>

namespace exchange::core::persistence {

namespace detail {
/**
 * @brief The applier's type, hidden from template argument deduction.
 *
 * @c T is deduced from @p journal and nowhere else. Spelling the callable
 * parameter as a bare @c function_ref<bool(const T &) const> would make it a
 * second deduction site, and deduction there is against the *lambda* the caller
 * wrote - which is not a @c function_ref and is not derived from one, so it
 * fails outright rather than falling back on the conversion the implicit
 * constructor exists to provide. Routing it through a dependent qualified-id
 * makes it a non-deduced context: @c T is fixed by the journal, and the lambda
 * then converts on the way in like any other argument.
 */
template <class T>
using applier = std::type_identity_t<util::function_ref<bool(const T &) const>>;
} // namespace detail

/// @brief How a replay ended.
struct replay_result {
	/// @brief Records handed to the applier and accepted, in order.
	std::uint64_t applied = 0;

	/**
	 * @brief The record the applier refused, or the end of the journal.
	 *
	 * Where to resume. Always @c from + @c applied, and returned rather than
	 * left to the caller's arithmetic because getting it wrong replays a
	 * command twice - which for a journal of *commands* is not idempotent: a
	 * second PLACE of the same id is a duplicate the book rejects, and a second
	 * CANCEL is a CANCEL_REJECTED reported to a client who never asked.
	 */
	std::uint64_t next = 0;

	/// @brief Whether the journal was exhausted rather than the applier
	/// stopping.
	bool complete = false;

	bool operator==(const replay_result &) const noexcept = default;
};

/**
 * @brief Apply every record of @p journal from record @p from onwards.
 *
 * @param journal The log to read. Opened for read or for append; either works,
 *        since a replay only reads.
 * @param from The first record to apply - @c manifest::sequence during
 * recovery, or zero to replay a whole journal. Taken on trust: a @p from beyond
 * the end of @p journal reports a complete replay of nothing, because a bare log
 * cannot tell an offset that is wrong from one that is simply at the tail.
 * Whether an offset belongs to a journal is a question about a *store*, and
 * @c event_store::open is where it is answered - which is why a sequence read
 * from @c checkpoint() can be passed straight in.
 * @param apply Where the records go.
 * @return What was applied and where to resume, or why the journal could not be
 *         read.
 *
 * @par Why a failed read is an error and not an early end
 * Because the two are indistinguishable from the outside and a caller has to
 * treat them oppositely. A short read at the end of the log means "done"; a
 * short read because the device refused means "stop, and tell somebody". Handing
 * both back as a @c replay_result with @c complete false would leave the loop
 * below spinning on the same offset forever, which is the worst available
 * behaviour for the one operation a venue cannot start without. So the failure
 * comes back as an error the caller cannot quietly ignore.
 *
 * @post @c result.complete is @c true only when the applier accepted every
 *       record through to the end of the journal as it stood when this was
 *       called. A journal still being appended to is not an error and not
 *       chased - the count is read once, so this terminates against a writer
 *       rather than following it.
 *
 * @par Resuming after a refusal
 * @code
 * std::uint64_t at = store.checkpoint().sequence;
 * for (;;) {
 *     const auto step = replay(store.journal(), at, submit);
 *     if (!step) return std::unexpected(step.error());   // the log, not the queue
 *     at = step->next;
 *     if (step->complete) break;
 *     partition.drain_and_flush();   // make room, then carry on from `at`
 * }
 * @endcode
 * A caller that ignores @c next and re-reads from @c from instead would apply
 * the accepted prefix a second time. @see replay_result::next
 *
 * @note Allocates nothing. The staging buffer is a fixed array, which is what
 *       lets a journal of any size replay in bounded memory.
 */
template <class T>
[[nodiscard]] std::expected<replay_result, std::string>
replay(record_log<T> &journal, std::uint64_t from, detail::applier<T> apply) {
	// Read once. A journal the engine is still appending to would otherwise
	// make this a loop with no end, and "replay everything that exists now" is
	// the only version of the job that terminates.
	const std::uint64_t total = journal.count();
	replay_result result{.applied = 0, .next = from, .complete = from >= total};

	constexpr std::size_t CHUNK = 64;
	alignas(T) std::array<std::byte, sizeof(T) * CHUNK> storage{};

	while (result.next < total) {
		const std::span<const T> batch =
			journal.read_into(result.next, storage.data(), CHUNK);
		// Not the end of the log: `total` was read from this same journal and the
		// file only ever grows, so every offset below it exists. An empty batch
		// here is the read failing - a device error, or the file shrinking under
		// us - and either way the records between here and `total` are not
		// coming.
		if (batch.empty())
			return std::unexpected(
				fmt::format("cannot read record {} of {} from {}",
							result.next,
							total,
							journal.path().string()));

		for (const T &record : batch) {
			if (!apply(record)) return result;
			++result.applied;
			++result.next;
		}
	}
	result.complete = result.next >= total;
	return result;
}

/**
 * @brief Replay a whole journal from the beginning.
 *
 * The shape a test or a verification pass wants: no snapshot, no checkpoint,
 * just "does re-applying this log reproduce what it produced the first time".
 */
template <class T>
[[nodiscard]] std::expected<replay_result, std::string>
replay(record_log<T> &journal, detail::applier<T> apply) {
	return replay(journal, 0, apply);
}

} // namespace exchange::core::persistence
