// What separation two hot atomics actually need. Two threads each hammer their
// own counter; the only variable is how many bytes apart the two counters sit.
//
// One line apart satisfies the coherence protocol, and folly argues that is not
// enough on x86: Intel's L2 spatial prefetcher moves lines in aligned 128-byte
// pairs, so two atomics 64 bytes apart can still ride one pair between two
// cores (folly/lang/Align.h hardcodes 128 on everything but ARM and s390x).
// cache.hpp declines to assume that and measures it instead - this is the
// measurement, and rewriting its FALSE_SHARING_RANGE as `2 * CACHE_LINE_SIZE`
// is what acting on the result looks like. That constant is source, not a build
// flag, so the two candidates are compared here as strides rather than as two
// builds: one run answers the question.
//
// The 8-byte case is a control, not a candidate: both counters are on one line,
// so it must come out dramatically the slowest. If it does not, the threads are
// not contending - wrong cores, a host that did not pin, an optimised-away
// loop - and the 64-vs-128 comparison beside it is meaningless. `alone` is the
// other end of the scale: the same loop with no neighbour at all, so the gap
// between it and a stride is the contention that stride is paying for.
//
// Placement comes from topology rather than hand-picked core numbers, and the
// two cores share an LLC because that is where the queues this pads actually
// run: a cross-socket pair measures the interconnect, not the prefetcher.

#include "core/concurrency/affinity.hpp" // discover, topology, pin_this_thread

#include <benchmark/benchmark.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>
#include <utility>

namespace {
using namespace exchange::core::concurrency;
using CorePair = std::pair<affinity::core_id, affinity::core_id>;

using slot_t = std::atomic<std::uint64_t>;

// Two counters @p Stride bytes apart. Aligned to the largest stride measured so
// the offsets are absolute rather than wherever the allocator happened to land:
// at 64 the two land in one 128-byte pair, at 128 in adjacent pairs, and that
// distinction is the whole experiment.
template <std::size_t Stride>
struct alignas(256) shared_counters {
	static_assert(Stride >= sizeof(slot_t));
	slot_t first{0};
	std::array<std::byte, Stride - sizeof(slot_t)> gap{};
	slot_t second{0};
};

// First distinct primary-core pair sharing an LLC. One primary sibling per
// physical core keeps SMT-sibling effects out of the signal.
std::optional<CorePair> pick_llc_sharing_pair(const affinity::topology &topo) {
	const auto cores = topo.primary_core_ids();
	for (std::size_t i = 0; i < cores.size(); ++i)
		for (std::size_t j = i + 1; j < cores.size(); ++j)
			if (topo.share_llc(cores[i], cores[j]))
				return CorePair{cores[i], cores[j]};
	return std::nullopt;
}

// Uncontended cost of the same increment, as the floor every stride is measured
// against.
void BM_FalseSharingAlone(benchmark::State &state, const CorePair &pair) {
	auto counters = std::make_unique<shared_counters<256>>();

	(void)affinity::pin_this_thread(pair.first);
	for (auto _ : state)
		counters->first.fetch_add(1, std::memory_order_relaxed);

	state.SetItemsProcessed(state.iterations());
}

// The neighbour thread writes `second` for the whole measurement; this thread's
// per-increment time on `first` is what is reported.
template <std::size_t Stride>
void BM_FalseSharing(benchmark::State &state, const CorePair &pair) {
	auto counters = std::make_unique<shared_counters<Stride>>();
	std::atomic<bool> stop{false};
	std::atomic<bool> running{false};

	const auto &[measured, neighbour] = pair;
	std::thread noise([&] {
		(void)affinity::pin_this_thread(neighbour);
		running.store(true, std::memory_order_release);
		while (!stop.load(std::memory_order_relaxed))
			counters->second.fetch_add(1, std::memory_order_relaxed);
	});

	// Do not start the clock until the neighbour is contending, or the first
	// iterations measure the uncontended case and dilute the result.
	while (!running.load(std::memory_order_acquire)) {}

	(void)affinity::pin_this_thread(measured);
	for (auto _ : state)
		counters->first.fetch_add(1, std::memory_order_relaxed);

	stop.store(true, std::memory_order_relaxed);
	noise.join();

	state.SetItemsProcessed(state.iterations());
}

// Registered only where the host offers two cores on one LLC - a single-core
// or single-sibling host cannot produce the contention this measures, and a
// benchmark that reports a number for it would be reporting noise.
const int false_sharing_registrar = [] {
	const affinity::topology topo = affinity::discover();
	const auto pair               = pick_llc_sharing_pair(topo);
	if (!pair) return 0;

	benchmark::RegisterBenchmark("FalseSharing/alone",
								 BM_FalseSharingAlone,
								 *pair)
		->UseRealTime();
	benchmark::RegisterBenchmark("FalseSharing/stride_8",
								 BM_FalseSharing<8>,
								 *pair)
		->UseRealTime();
	benchmark::RegisterBenchmark("FalseSharing/stride_64",
								 BM_FalseSharing<64>,
								 *pair)
		->UseRealTime();
	benchmark::RegisterBenchmark("FalseSharing/stride_128",
								 BM_FalseSharing<128>,
								 *pair)
		->UseRealTime();
	benchmark::RegisterBenchmark("FalseSharing/stride_256",
								 BM_FalseSharing<256>,
								 *pair)
		->UseRealTime();
	return 0;
}();
} // namespace
