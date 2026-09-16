// Throughput of the ladder kernels against the scalar loops they replace.
//
// A mean rather than a p99, unlike most of this tree: these are bulk reads
// over a whole side, so the question is how long one sweep of N levels takes,
// and Google Benchmark's own aggregate answers that directly. The per-order
// paths are where a tail latency is the number that matters.
//
// @par What the scalar baselines are, and why that matters to the reading
// They are the loops a call site would write by hand, compiled by the same
// compiler with the same flags. That is deliberately a *hard* baseline for
// total(): a bare accumulation loop is exactly what an optimiser
// auto-vectorises, so the scalar bar is usually already a vector bar at the
// build's baseline instruction set, and the kernel's only remaining edge is
// that run-time dispatch reaches a wider register than the compiler was
// allowed to assume. On a machine whose baseline and dispatched target are the
// same width, expect them to tie - and read that as the auto-vectoriser doing
// its job, not as the kernel failing.
//
// consume() is the other case. Its scalar form carries a loop-carried
// dependency and a data-dependent break, which is not vectorisable at all - so
// the comparison there is vector against genuinely scalar code, and the branch
// that stops being mispredicted is most of the difference.

#include "core/simd/ladder.hpp"
#include "core/simd/target.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <string>
#include <vector>

namespace {

/// @brief Depths a real side occupies: a top-of-book window, a full retained
///        window, and past the point where the side leaves L1.
void ladder_depths(benchmark::internal::Benchmark *bench) {
	for (const std::int64_t depth : {8, 32, 128, 512, 4096}) bench->Arg(depth);
}

/// @brief A ladder of strictly positive sizes, fixed seed so every benchmark
///        in this file walks the same numbers.
[[nodiscard]] std::vector<std::int64_t> ladder_sizes(std::size_t depth) {
	std::mt19937_64 engine{20260916};
	std::uniform_int_distribution<std::int64_t> size{1, 1000};

	std::vector<std::int64_t> sizes(depth);
	for (std::int64_t &entry : sizes) entry = size(engine);
	return sizes;
}

/// @brief The same ladder as the {price, size} pairs l2_book stores.
[[nodiscard]] std::vector<std::int64_t>
ladder_pairs(std::span<const std::int64_t> sizes) {
	std::vector<std::int64_t> pairs;
	pairs.reserve(sizes.size() * 2);
	for (std::size_t index = 0; index < sizes.size(); ++index) {
		pairs.push_back(1000000 + static_cast<std::int64_t>(index));
		pairs.push_back(sizes[index]);
	}
	return pairs;
}

/// @brief Stamp every result with the instruction set the dispatch actually
///        reached, so a number is never read against the wrong machine.
void label_target(benchmark::State &state) {
	state.SetLabel(std::string{exchange::core::simd::active_target()});
}

/// @brief A size that reaches about three quarters of the way down, which is
///        the shape that makes the scalar walk's exit branch hardest to
///        predict. A size that stops at the touch would measure nothing, and
///        one that clears the side would measure only the loop.
[[nodiscard]] std::int64_t sweep_target(std::span<const std::int64_t> sizes) {
	std::int64_t total = 0;
	for (const std::int64_t size : sizes) total += size;
	return (total * 3) / 4;
}

// --------------------------------------------------------------------------
// total - summing a side
// --------------------------------------------------------------------------

void BM_LadderTotalScalar(benchmark::State &state) {
	const auto sizes = ladder_sizes(static_cast<std::size_t>(state.range(0)));
	for (auto _ : state) {
		std::int64_t sum = 0;
		for (const std::int64_t size : sizes) sum += size;
		benchmark::DoNotOptimize(sum);
	}
	label_target(state);
	state.SetItemsProcessed(state.iterations() * state.range(0));
}

void BM_LadderTotalSimd(benchmark::State &state) {
	const auto sizes = ladder_sizes(static_cast<std::size_t>(state.range(0)));
	const std::span<const std::int64_t> span{sizes};
	for (auto _ : state) {
		benchmark::DoNotOptimize(exchange::core::simd::total(span));
	}
	label_target(state);
	state.SetItemsProcessed(state.iterations() * state.range(0));
}

// The AoS pair: the price lanes have to be brought in either way, so this is
// the comparison that says whether de-interleaving beats a strided scalar
// read.
void BM_LadderTotalInterleavedScalar(benchmark::State &state) {
	const auto sizes = ladder_sizes(static_cast<std::size_t>(state.range(0)));
	const auto pairs = ladder_pairs(sizes);
	const std::size_t levels = sizes.size();

	for (auto _ : state) {
		std::int64_t sum = 0;
		for (std::size_t index = 0; index < levels; ++index)
			sum += pairs[(2 * index) + 1];
		benchmark::DoNotOptimize(sum);
	}
	label_target(state);
	state.SetItemsProcessed(state.iterations() * state.range(0));
}

void BM_LadderTotalInterleavedSimd(benchmark::State &state) {
	const auto sizes = ladder_sizes(static_cast<std::size_t>(state.range(0)));
	const auto pairs = ladder_pairs(sizes);
	const std::span<const std::int64_t> span{pairs};

	for (auto _ : state) {
		benchmark::DoNotOptimize(
			exchange::core::simd::total_interleaved(span));
	}
	label_target(state);
	state.SetItemsProcessed(state.iterations() * state.range(0));
}

// --------------------------------------------------------------------------
// consume - how far a sweep reaches
// --------------------------------------------------------------------------

void BM_LadderConsumeScalar(benchmark::State &state) {
	const auto sizes = ladder_sizes(static_cast<std::size_t>(state.range(0)));
	const std::int64_t wanted = sweep_target(sizes);

	for (auto _ : state) {
		std::int64_t left   = wanted;
		std::size_t levels  = 0;
		std::int64_t filled = 0;
		for (const std::int64_t size : sizes) {
			if (left <= 0) break;
			const std::int64_t taken = size < left ? size : left;
			left -= taken;
			filled += taken;
			++levels;
		}
		benchmark::DoNotOptimize(levels);
		benchmark::DoNotOptimize(filled);
	}
	label_target(state);
	state.SetItemsProcessed(state.iterations() * state.range(0));
}

void BM_LadderConsumeSimd(benchmark::State &state) {
	const auto sizes = ladder_sizes(static_cast<std::size_t>(state.range(0)));
	const std::span<const std::int64_t> span{sizes};
	const std::int64_t wanted = sweep_target(sizes);

	for (auto _ : state) {
		benchmark::DoNotOptimize(exchange::core::simd::consume(span, wanted));
	}
	label_target(state);
	state.SetItemsProcessed(state.iterations() * state.range(0));
}

void BM_LadderConsumeInterleavedScalar(benchmark::State &state) {
	const auto sizes = ladder_sizes(static_cast<std::size_t>(state.range(0)));
	const auto pairs = ladder_pairs(sizes);
	const std::size_t levels_held = sizes.size();
	const std::int64_t wanted     = sweep_target(sizes);

	for (auto _ : state) {
		std::int64_t left   = wanted;
		std::size_t levels  = 0;
		std::int64_t filled = 0;
		for (std::size_t index = 0; index < levels_held && left > 0; ++index) {
			const std::int64_t size  = pairs[(2 * index) + 1];
			const std::int64_t taken = size < left ? size : left;
			left -= taken;
			filled += taken;
			++levels;
		}
		benchmark::DoNotOptimize(levels);
		benchmark::DoNotOptimize(filled);
	}
	label_target(state);
	state.SetItemsProcessed(state.iterations() * state.range(0));
}

void BM_LadderConsumeInterleavedSimd(benchmark::State &state) {
	const auto sizes = ladder_sizes(static_cast<std::size_t>(state.range(0)));
	const auto pairs = ladder_pairs(sizes);
	const std::span<const std::int64_t> span{pairs};
	const std::int64_t wanted = sweep_target(sizes);

	for (auto _ : state) {
		benchmark::DoNotOptimize(
			exchange::core::simd::consume_interleaved(span, wanted));
	}
	label_target(state);
	state.SetItemsProcessed(state.iterations() * state.range(0));
}

BENCHMARK(BM_LadderTotalScalar)->Apply(ladder_depths);
BENCHMARK(BM_LadderTotalSimd)->Apply(ladder_depths);
BENCHMARK(BM_LadderTotalInterleavedScalar)->Apply(ladder_depths);
BENCHMARK(BM_LadderTotalInterleavedSimd)->Apply(ladder_depths);
BENCHMARK(BM_LadderConsumeScalar)->Apply(ladder_depths);
BENCHMARK(BM_LadderConsumeSimd)->Apply(ladder_depths);
BENCHMARK(BM_LadderConsumeInterleavedScalar)->Apply(ladder_depths);
BENCHMARK(BM_LadderConsumeInterleavedSimd)->Apply(ladder_depths);

} // namespace
