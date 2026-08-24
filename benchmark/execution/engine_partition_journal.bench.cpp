// What journalling costs an engine_partition on the drain path.
//
// The question this exists to answer is narrow and was raised in review: drain()
// appends each command to the log individually, so a journalled partition pays
// one stdio fwrite per command - and fwrite is a locking call by default on
// every platform this builds on, since a FILE* is shared-by-default state. The
// log is single-writer by contract (record_log's own header says so), so that
// lock is pure overhead. The open question was whether it is *measurable*
// overhead, and the project's rule is that a claimed speedup without a benchmark
// is a guess. This is the measurement.
//
// The sampled operation is one drain(), matching engine_partition_metrics.bench
// and the unit partition_metrics::drain_latency_ns itself times. flush() is
// deliberately outside the sample: it is the durability barrier, a device round
// trip by design, and folding it in would bury the per-command append cost under
// an fsync. Group commit is what makes that split the honest one - the barrier
// is per batch, the append is per command, and only the second is on the path
// this measures.

#include "core/persistence/record_log.hpp"
#include "latency.fixture.hpp"
#include "execution/engine_partition.fixture.hpp"
#include "event/command.hpp"
#include "execution/engine_partition.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <system_error>
#include <vector>

using namespace exchange::engine;
using namespace exchange::engine::orders;
using namespace exchange::engine::event;
using namespace exchange::engine::execution;
using namespace exchange;
using exchange::bench::latency_sampler;
using exchange::bench::make_crossing_batch;
using exchange::bench::PARTITION_BATCH;

namespace {

using Engine = engine_partition<1U << 12>;

/// @brief A journal in the system temp, removed when the run ends.
///
/// A real file rather than a null sink, because the cost being priced is the
/// stdio call and its lock; a sink that skipped the write would answer a
/// different question. It is buffered, so no device traffic happens inside a
/// sampled drain() - that is what flush() is for, and flush() is outside.
class scratch_journal {
public:
	explicit scratch_journal(const char *leaf)
		: path_(std::filesystem::temp_directory_path() / leaf) {
		std::error_code ec;
		std::filesystem::remove(path_, ec);
	}

	scratch_journal(const scratch_journal &)            = delete;
	scratch_journal &operator=(const scratch_journal &) = delete;
	scratch_journal(scratch_journal &&)                 = delete;
	scratch_journal &operator=(scratch_journal &&)      = delete;

	~scratch_journal() {
		std::error_code ec;
		std::filesystem::remove(path_, ec);
	}

	[[nodiscard]] const std::filesystem::path &path() const noexcept {
		return path_;
	}

private:
	std::filesystem::path path_;
};

#ifdef EXCHANGE_HAS_CYCLE_CLOCK

// The control: the same flow through the same partition with nothing attached.
void BM_EnginePartitionLatency_DrainNoJournal(benchmark::State &state) {
	auto engine = std::make_unique<Engine>(nullptr);
	engine->listing(0);

	latency_sampler sampler;
	price_t base = 1;
	for (auto _ : state) {
		const auto batch = make_crossing_batch(PARTITION_BATCH, base);
		base += static_cast<price_t>(PARTITION_BATCH);
		for (const command &cmd : batch) (void)engine->submit(cmd);
		sampler.sample([&] { benchmark::DoNotOptimize(engine->drain()); });
		(void)engine->flush();
	}
	sampler.publish(state);
}

BENCHMARK(BM_EnginePartitionLatency_DrainNoJournal);

// And the same thing journalled. The delta between these two, divided by BATCH,
// is the per-command cost of an append: a memcpy into the FILE buffer plus
// whatever fwrite's locking costs. If that delta is small next to the drain
// itself, the review's concern is priced and closed; if it is not, the fix is
// either a batched append per drain or the platform's unlocked fwrite, and this
// benchmark is what either would have to beat.
void BM_EnginePartitionLatency_DrainJournalled(benchmark::State &state) {
	const scratch_journal scratch("exchange_bench_journal.bin");
	auto log = Engine::journal::open_for_append(scratch.path());
	if (!log) {
		state.SkipWithError("cannot open the scratch journal");
		return;
	}

	auto engine = std::make_unique<Engine>(nullptr);
	engine->listing(0);
	engine->attach_journal(&*log);

	latency_sampler sampler;
	price_t base = 1;
	for (auto _ : state) {
		const auto batch = make_crossing_batch(PARTITION_BATCH, base);
		base += static_cast<price_t>(PARTITION_BATCH);
		for (const command &cmd : batch) (void)engine->submit(cmd);
		sampler.sample([&] { benchmark::DoNotOptimize(engine->drain()); });
		(void)engine->flush();
	}
	sampler.publish(state);

	// Proof the arm did what it claims: a journalled drain that recorded nothing
	// would look identical to the control and be a very quiet way to measure
	// nothing at all.
	state.counters["journal_records"] = static_cast<double>(log->count());
	state.counters["journal_failures"] =
		static_cast<double>(engine->journal_failures());
}

BENCHMARK(BM_EnginePartitionLatency_DrainJournalled);

#endif // EXCHANGE_HAS_CYCLE_CLOCK

} // namespace
