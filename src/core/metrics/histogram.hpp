#pragma once
// A bounded-memory, O(1), always-on latency histogram.
//
// Deliberately not benchmark/latency.fixture.hpp's latency_sampler: that type
// keeps every sample (up to a million, ~8 MB) to place a precise p99.9 for one
// benchmark run, and calibrates a cycle counter to do it. Neither is
// affordable on a path that runs for the life of the process — unbounded
// sample retention is a leak by another name, and RDTSC calibration is a
// ~200 ms stall this module has no good place to pay. So this trades
// precision for boundedness: one bucket per power-of-two octave, each bucket
// a single relaxed counter, record() touches exactly one. A reported
// percentile lands on a bucket's upper bound rather than an exact value —
// coarse, but the coarseness is known and constant, which is what an
// always-on monitor needs more than a benchmark does.

#include "core_export.hpp" // CORE_EXPORT (generated)

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace exchange::core::metrics {

/**
 * @brief Power-of-two-bucket distribution, single-writer like counter.
 *
 * Bucket index is the bit width of the recorded value (`std::bit_width`):
 * bucket 0 holds exactly 0, bucket @c i for @c i in [1,63] holds
 * @c [2^(i-1), 2^i - 1], and bucket 64 holds everything from @c 2^63 up.
 * @c bit_width is a single instruction on every target this project builds
 * for (`bsr`/`lzcnt`/`clz`), so classifying a sample costs nothing more than
 * the counter bump itself.
 *
 * @warning Single-writer for @c record(), the same contract @c counter
 *          documents. @c read() may run on any thread.
 *
 * Members are exported individually, not the class — see counter.hpp's class
 * note on why.
 */
class histogram {
public:
	/// @brief One bucket per possible bit width of a 64-bit value (0..64).
	static constexpr std::size_t NUM_BUCKETS = 65;

	histogram() noexcept = default;

	histogram(const histogram &)            = delete;
	histogram &operator=(const histogram &) = delete;
	histogram(histogram &&)                 = delete;
	histogram &operator=(histogram &&)      = delete;
	~histogram()                            = default;

	/// @brief Writer side: record one observation. Whatever unit the reader
	///        is told to expect — this module always uses nanoseconds.
	CORE_EXPORT void record(std::uint64_t value) noexcept;

	/// @brief The largest value bucket @p index can hold.
	[[nodiscard]] CORE_EXPORT static std::uint64_t
	upper_bound(std::size_t index) noexcept;

	/// @brief A point-in-time read, snapshotted so percentiles are computed
	///        against one consistent copy rather than a moving target.
	struct snapshot {
		std::array<std::uint64_t, NUM_BUCKETS> counts{};
		std::uint64_t total = 0;

		/// @brief The upper bound of the bucket holding the @p q quantile,
		///        e.g. @c quantile(0.99) for p99. @p q outside [0,1] is
		///        clamped.
		[[nodiscard]] CORE_EXPORT std::uint64_t quantile(double q) const noexcept;
	};

	/**
	 * @brief Reader side: relaxed loads across every bucket.
	 *
	 * @warning Not a single atomic operation, same caveat as
	 *          @c position_snapshot: a writer's @c record() landing between
	 *          two of these loads means the total can be off by a handful
	 *          against a hot writer. Fine for a periodic exposition of a
	 *          latency distribution, not a quantity anything is reconciled
	 *          against.
	 */
	[[nodiscard]] CORE_EXPORT snapshot read() const noexcept;

	/// @brief Writer side: every bucket back to zero.
	CORE_EXPORT void reset() noexcept;

private:
	[[nodiscard]] static std::size_t bucket_of(std::uint64_t value) noexcept;

	// Plain atomics, not core::metrics::counter: every element here is
	// written by the same single thread that owns the whole histogram, so
	// there is no cross-thread false sharing between buckets to pad against —
	// see counter.hpp's class note. 65 * 8 bytes fits in a cache line and a
	// bit, against 65 cache lines if this reused counter.
	std::array<std::atomic<std::uint64_t>, NUM_BUCKETS> buckets_{};
};

} // namespace exchange::core::metrics
