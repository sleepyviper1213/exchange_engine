#pragma once
// The harness's sense of "later": things that have been decided but have not
// happened yet.
//
// A backtest that models any delay at all needs somewhere to hold what is in
// flight, and the moment two things are in flight at once the question of which
// one happens first becomes part of the answer the run produces. So it is
// settled here - totally, and without reference to anything outside this file.

#include "fwd.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

namespace exchange::strategy::backtest {

/**
 * @brief A discrete-event queue: items come due at a stated time, and ties are
 *        broken by the order they were scheduled in.
 *
 * @tparam T The payload. Trivially copyable - see the note on why it is data.
 *
 * @par Why the tiebreak is the point of the class
 * Ordering scheduled items by time alone is not an ordering. Two items due at
 * the same nanosecond are equal under that comparison, and every container that
 * could hold them - @c std::priority_queue, a hand-rolled heap, a sorted vector
 * through @c std::sort - is explicitly permitted to return equal elements in
 * whatever order it likes. Which one it actually returns then depends on the
 * heap's internal layout, and so on the standard library the run was built
 * against.
 *
 * That is fatal here rather than untidy. A backtest's result must be a function
 * of its input alone - the property the whole harness is arranged around, @see
 * feed_clock and @c session's note on having no consumer thread - and a
 * simultaneous cancel and quote applied in either order can produce different
 * fills. A run would then answer differently on two machines and neither answer
 * could be attributed to the strategy.
 *
 * The fix is a monotonically increasing sequence number, compared after the due
 * time. Ties break in scheduling order, which is a property of the run rather
 * than of the library: simultaneous items happen in the order somebody asked
 * for them. The counter is never reset, not even by @c clear, so the order is
 * total across the whole run and not merely within one batch.
 *
 * @par Why a heap and not a queue
 * Because due times are not monotone. Flight times differ per message - a
 * jitter draw, a slower path for one kind of command - so an item scheduled
 * later can come due sooner, and a FIFO would release the two in the wrong
 * order. If every flight time in a run were identical a plain queue would do,
 * and nothing here could know that at compile time.
 *
 * @par Why the payload is data and not a callback
 * A @c std::function payload is the obvious shape and the wrong one. It
 * allocates for any capture that misses the small-buffer optimisation, it costs
 * an indirect call per event, and - the real objection - it puts the
 * *behaviour* of an event inside the queue, where nothing can inspect it,
 * record it, or compare two runs' worth of it. A trivially copyable payload
 * keeps this a container and leaves the interpretation with the caller, who is
 * the only one that knows what a released item means. @c static_assert enforces
 * it.
 *
 * @note Bounded at construction and reserved up front, so a run allocates once.
 *       A full queue refuses rather than growing, which is what lets a caller
 *       treat it as back-pressure. @see wire
 */
template <class T>
class delay_queue {
public:
	static_assert(std::is_trivially_copyable_v<T>,
				  "a scheduled item is data, not behaviour - see the class "
				  "note on why the payload is not a callback");

	using value_type = T;

	/// @brief Build a queue holding at most @p capacity items in flight.
	explicit delay_queue(std::size_t capacity) : capacity_(capacity) {
		heap_.reserve(capacity);
	}

	/// @brief Schedule @p item to come due at @p due_ns.
	/// @return @c false if the queue is full; nothing is scheduled.
	[[nodiscard]] bool schedule(std::uint64_t due_ns, const T &item) {
		return schedule_range(due_ns, std::span<const T>{&item, 1});
	}

	/**
	 * @brief Schedule every item in @p items to come due at @p due_ns.
	 *
	 * All at one time, and each with its own sequence number, so they release
	 * in the order they appear here. That is the case the tiebreak was built
	 * for: one message carrying several commands travels as a unit and arrives
	 * as a unit, and its contents must not be permuted on the way.
	 *
	 * @return @c false if the queue cannot take the whole batch, in which case
	 *         it takes none of it - the all-or-nothing contract a caller's own
	 *         rollback depends on. @see strategy::command_sink
	 */
	[[nodiscard]] bool schedule_range(std::uint64_t due_ns,
									  std::span<const T> items) {
		if (items.empty()) return true;
		if (!has_room(items.size())) return false;
		for (const T &item : items) {
			heap_.emplace_back(due_ns, next_sequence_++, item);
			std::ranges::push_heap(heap_, sooner_is_greater{});
		}
		return true;
	}

	/**
	 * @brief Append everything due at or before @p now_ns to @p out.
	 *
	 * @param now_ns The current time, on whatever clock the schedule was
	 *        written against. @see wire on why that is one clock and not two.
	 * @param out Appended to, never cleared - a caller that could not hand the
	 *        last release on keeps it in front of this one, and order survives.
	 * @return How many items were appended.
	 *
	 * @post The appended items are in @c (due_ns, sequence) order, which is the
	 *       total order this class exists to provide.
	 */
	std::size_t release(std::uint64_t now_ns, std::vector<T> &out) {
		const std::size_t before = out.size();
		while (!heap_.empty() && heap_.front().due_ns <= now_ns) {
			out.push_back(heap_.front().item);
			std::ranges::pop_heap(heap_, sooner_is_greater{});
			heap_.pop_back();
		}
		return out.size() - before;
	}

	/// @brief Abandon everything in flight. Does *not* reset the sequence
	///        counter: the order must stay total across the whole run.
	void clear() noexcept { heap_.clear(); }

	/// @brief Whether @p count more items would fit.
	///
	/// Exposed so a caller can decline *before* doing work that a refusal would
	/// waste - @c wire draws its jitter only once this says yes, so a batch the
	/// gate rolls back does not consume a random draw and the sequence of draws
	/// stays a function of the commands that were actually scheduled.
	[[nodiscard]] bool has_room(std::size_t count) const noexcept {
		return count <= capacity_ - heap_.size();
	}

	/// @brief Items scheduled and not yet released.
	[[nodiscard]] std::size_t pending() const noexcept { return heap_.size(); }

	/// @brief Whether anything is in flight.
	[[nodiscard]] bool is_empty() const noexcept { return heap_.empty(); }

	/// @brief The most items that may be in flight at once.
	[[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

	/// @brief When the next item comes due, or nothing if none is in flight.
	[[nodiscard]] std::optional<std::uint64_t> next_due_ns() const noexcept {
		if (heap_.empty()) return std::nullopt;
		return heap_.front().due_ns;
	}

	/// @brief Items scheduled since construction. Also the next sequence
	///        number, which is why it survives @c clear.
	[[nodiscard]] std::uint64_t scheduled() const noexcept {
		return next_sequence_;
	}

private:
	struct entry {
		std::uint64_t due_ns;
		std::uint64_t sequence;
		T item;
	};

	/// @brief The total order, expressed the way @c std::push_heap wants it.
	///
	/// A heap keeps the *greatest* element at its root, and the root is the
	/// item to release first - so "greater" here has to mean "happens sooner",
	/// and the comparison reads backwards on purpose.
	struct sooner_is_greater {
		[[nodiscard]] constexpr bool
		operator()(const entry &lhs, const entry &rhs) const noexcept {
			if (lhs.due_ns != rhs.due_ns) return lhs.due_ns > rhs.due_ns;
			return lhs.sequence > rhs.sequence;
		}
	};

	std::vector<entry> heap_;
	std::size_t capacity_;
	/// Monotone for the life of the queue. @see the class note on the tiebreak.
	std::uint64_t next_sequence_ = 0;
};

} // namespace exchange::strategy::backtest
