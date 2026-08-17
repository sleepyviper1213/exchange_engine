#include "market-data/reconstructor.hpp"

#include "market-data/binance/normalise.hpp"
#include "market-data/replay.fixture.hpp"

#include <benchmark/benchmark.h>
#include <fmt/format.h>

#include <string>
#include <vector>


using namespace exchange::market_data;

// The managed-local-order-book path measured end to end, and split so the cost
// of the venue-neutral `depth_event` is visible on its own.
//
// BM_MarketReplay_L2Book already measures the shortcut: decoded levels applied
// straight to an l2_book, no event materialised, no sequencing. Everything here
// pays for what that skips — normalisation into a neutral event (two heap
// vectors per event), gap detection, and, on the recovery path, retaining those
// events in a deque until a snapshot bridges them.
//
// The split matters because those two vectors are the only per-message
// allocation left on the ingest path, so NormaliseOnly is what any inline- or
// arena-storage experiment has to beat, and the gap between SteadyState and
// BM_MarketReplay_L2Book is the whole price of routing through depth_event.
namespace {

// A snapshot sequence chosen so the feed's first event applies rather than
// gapping: the corpus numbers its events from firstUpdateId upward, and a
// snapshot seeds "everything through N", so the seed must sit one below.
std::uint64_t seed_sequence(const replay::ReplayData &data) {
	if (data.feed.empty()) return 0;
	const auto first = binance::sequence_of(data.feed.front()).first();
	return first == 0 ? 0 : first - 1;
}

book_snapshot seed_for(const replay::ReplayData &data) {
	book_snapshot seed = binance::normalise(data.snap);
	seed.sequence      = seed_sequence(data);
	return seed;
}

// Normalisation alone: two vector allocations plus the level copy, per event.
// The floor any inline-storage experiment is competing against.
void BM_Reconstructor_NormaliseOnly(benchmark::State &state) {
	const auto data = replay::load();

	for (auto _ : state) {
		for (const auto &update : data.feed) {
			auto event = binance::normalise(update);
			benchmark::DoNotOptimize(event.bids.data());
			benchmark::DoNotOptimize(event.asks.data());
		}
		benchmark::ClobberMemory();
	}
	state.SetItemsProcessed(state.iterations() *
							static_cast<std::int64_t>(data.levels));
	state.SetLabel(fmt::format("{} events / {} levels (normalise only)",
							   data.feed.size(),
							   data.levels));
}

BENCHMARK(BM_Reconstructor_NormaliseOnly);

// The steady in-sequence path: normalise, gap-check, apply. What a live feed
// costs once synced.
void BM_Reconstructor_SteadyState(benchmark::State &state) {
	const auto data = replay::load();
	const auto seed = seed_for(data);

	for (auto _ : state) {
		state.PauseTiming();
		depth_reconstructor recon;
		(void)recon.on_snapshot(seed);
		state.ResumeTiming();

		for (const auto &update : data.feed)
			benchmark::DoNotOptimize(
				recon.on_event(binance::normalise(update)));

		state.PauseTiming();
		// A run that gapped or buffered would be measuring the wrong path, so
		// fail loudly rather than reporting a fast, meaningless number. The
		// counters come with it: losing is_alive() does not say *why*, and the two
		// causes are unrelated. A gap means the corpus or the seed is wrong; a
		// cross means the feed produced a crossed book and resync_on_cross tore
		// the replica down with the sequence perfectly intact (which is why it
		// is counted in crosses(), not gaps()).
		std::string failure;
		if (!recon.is_alive() || recon.stats().gaps != 0)
			failure = fmt::format(
				"feed did not stay in sequence: live={}, gaps={}, crosses={}, "
				"applied={}, discarded={}, dropped={}, pending={}, "
				"last_sequence={}",
				recon.is_alive(),
				recon.stats().gaps,
				recon.crosses(),
				recon.stats().applied,
				recon.stats().discarded,
				recon.dropped(),
				recon.pending(),
				recon.last_sequence());
		state.ResumeTiming();

		if (!failure.empty()) {
			// SkipWithError stops the timer itself and asserts that no
			// Pause/ResumeTiming follows it, and it does not end a ranged-for
			// loop on its own — the iterator cached the trip count before the
			// first iteration. So it has to come after the last timer call and
			// be followed by an explicit break, or the run aborts inside
			// benchmark's own check instead of reporting the error.
			state.SkipWithError(failure);
			break;
		}
		benchmark::ClobberMemory();
	}
	state.SetItemsProcessed(state.iterations() *
							static_cast<std::int64_t>(data.levels));
	state.SetLabel(
		fmt::format("{} events / {} levels (normalise + sequence + apply)",
					data.feed.size(),
					data.levels));
}

BENCHMARK(BM_Reconstructor_SteadyState);

// The recovery path: every event arrives while the book is unsynced, so each
// one is retained in the pending deque, then replayed when the snapshot lands.
// This is where a depth_event's size is paid rather than just its allocation.
void BM_Reconstructor_BufferedReplay(benchmark::State &state) {
	const auto data = replay::load();
	const auto seed = seed_for(data);
	// Unbounded: eviction would drop events and measure a shorter replay.
	const reconstructor_options options{.max_pending = 0};

	for (auto _ : state) {
		depth_reconstructor recon(options);
		for (const auto &update : data.feed)
			benchmark::DoNotOptimize(
				recon.on_event(binance::normalise(update)));
		benchmark::DoNotOptimize(recon.on_snapshot(seed));
		benchmark::ClobberMemory();
	}
	state.SetItemsProcessed(state.iterations() *
							static_cast<std::int64_t>(data.levels));
	state.SetLabel(
		fmt::format("{} events / {} levels (buffer all, then replay)",
					data.feed.size(),
					data.levels));
}

BENCHMARK(BM_Reconstructor_BufferedReplay);

} // namespace
