#include "recover.hpp"

#include "core/logging.hpp"
#include "core/persistence/event_store.hpp"
#include "core/persistence/replay.hpp"
#include "trading-engine.hpp"
#include "trading-engine/event/lifecycle/lifecycle.hpp"
#include "trading-engine/format.hpp" // IWYU pragma: keep - fmt::formatter<order_book>, <startup>, <shutdown>, <recovery>

#include <fmt/std.h> // IWYU pragma: keep - fmt::formatter<std::filesystem::path>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <vector>

using namespace exchange::engine;
using namespace exchange::engine::orders;

namespace exchange::app {
namespace {

namespace lifecycle   = event::lifecycle;
namespace persistence = core::persistence;

using journalled_store = persistence::event_store<event::command>;

/// @brief The two listings this command carries. Fixed rather than
/// configurable:
///        the subject is the store, and a second listing is here only to prove
///        a snapshot spans them.
constexpr symbol_id_t LEFT  = 1;
constexpr symbol_id_t RIGHT = 2;

/// @brief Nanoseconds since the UNIX epoch - a session boundary's clock.
/// @copydetails cmd_demo's wall_clock_ns
[[nodiscard]] std::uint64_t wall_clock_ns() noexcept {
	return static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::system_clock::now().time_since_epoch())
			.count());
}

/// @brief Drain until nothing is queued, so the ring has room again.
std::uint64_t drain_fully(execution::engine_partition<1024> &partition) {
	std::uint64_t applied = 0;
	for (;;) {
		const std::size_t n = partition.drain();
		if (!partition.flush()) {
			spdlog::error("the journal could not be made durable; nothing from "
						  "this batch was published");
			return applied;
		}
		if (n == 0) return applied;
		applied += n;
	}
}

/**
 * @brief Orders that rest and stay rested, so state survives to the next run.
 *
 * Deliberately not the self-cancelling flow `demo` uses. That one is built so
 * the book returns to empty, which is right for a throughput figure and useless
 * here - a store whose recovered book is always empty demonstrates nothing.
 * These sit far from the touch on both sides and never cross.
 *
 * @param first_id Where to start numbering. Taken from the journal's length by
 * the caller, because a recovered session continues the previous one's id space
 *        and reusing an id is the duplicate the book rejects.
 */
std::vector<event::command> resting_flow(order_id_t first_id,
										 std::uint64_t count) {
	std::vector<event::command> flow;
	flow.reserve(count);
	for (std::uint64_t i = 0; i < count; ++i) {
		const order_id_t id = first_id + i;
		const bool is_bid   = (i % 2) == 0;
		const auto symbol   = (i % 4) < 2 ? LEFT : RIGHT;
		// Bids well below asks, and stepping away from the touch, so nothing
		// crosses however many runs accumulate.
		const auto price =
			static_cast<price_t>(is_bid ? 1000 - (i % 8) : 2000 + (i % 8));
		flow.push_back(event::command::place(
			order{.id        = id,
				  .symbol_id = symbol,
				  .side      = is_bid ? side_t::bid : side_t::ask,
				  .price     = price,
				  .qty       = static_cast<quantity_t>(1 + (i % 5))}));
	}
	return flow;
}

/// @brief Load the snapshot the manifest names, if it names one.
/// @return Orders restored, or nothing on failure (already logged).
std::optional<std::uint64_t>
restore_books(journalled_store &store,
			  execution::engine_partition<1024> &partition) {
	const persistence::manifest &at = store.checkpoint();
	if (at.snapshot_id == 0) return std::uint64_t{0};

	const auto loaded =
		execution::load_snapshot_reporting(partition.books(),
										   store.snapshot_path(at.snapshot_id));
	if (!loaded) {
		spdlog::error("snapshot load failed: {}", loaded.error());
		return std::nullopt;
	}
	// Skipped records mean the snapshot and this command's listings disagree -
	// the same class of fault `misrouted` reports on the command path.
	if (loaded->skipped != 0)
		spdlog::warn("{} snapshot records named a listing this partition does "
					 "not carry; they were not restored",
					 loaded->skipped);
	return loaded->restored;
}

/// @brief Replay every journal record the checkpoint does not already cover.
/// @return Records applied, or nothing on failure (already logged).
std::optional<std::uint64_t>
replay_tail(journalled_store &store,
			execution::engine_partition<1024> &partition) {
	std::uint64_t next     = store.checkpoint().sequence;
	const std::uint64_t at = next;

	for (;;) {
		// submit refuses when the ring is full, which during a replay is the
		// normal case rather than an error - the disk is faster than the
		// consumer. `step.next` is where to carry on from; re-reading from `at`
		// would apply the accepted prefix twice, and a second PLACE of a live
		// id is DUPLICATE_ORDER_ID.
		const auto step = persistence::replay(
			store.journal(),
			next,
			[&](const event::command &cmd) { return partition.submit(cmd); });
		next = step.next;
		(void)drain_fully(partition);
		if (partition.journal_failures() != 0) return std::nullopt;
		if (step.complete) break;
	}
	return next - at;
}

} // namespace

int cmd_recover(const recover_settings &settings) {
	auto store = journalled_store::open(settings.store);
	if (!store) {
		spdlog::error("cannot open store: {}", store.error());
		return EXIT_FAILURE;
	}

	const persistence::manifest opening = store->checkpoint();
	spdlog::info("store {} - journal {} records, checkpoint {{snapshot={} "
				 "sequence={} session={}}}",
				 store->root(),
				 store->journal().count(),
				 opening.snapshot_id,
				 opening.sequence,
				 opening.session);

	execution::engine_partition<1024> partition(nullptr);
	partition.listing(LEFT);
	partition.listing(RIGHT);

	// --- recovery, in the order docs/recovery.md sets out -------------------
	// Listings first (done), then the snapshot, then the journal tail. The
	// journal is deliberately not attached until all of that is finished:
	// re-journalling a replay would append the whole history to itself on every
	// restart.
	const auto restored = restore_books(*store, partition);
	if (!restored) return EXIT_FAILURE;
	const auto replayed = replay_tail(*store, partition);
	if (!replayed) {
		spdlog::error("replay could not be journalled durably; stopping");
		return EXIT_FAILURE;
	}

	const bool inherited          = *restored != 0 || *replayed != 0;
	const std::uint64_t opened_ns = wall_clock_ns();
	const lifecycle::startup opened{
		.session      = opened_ns,
		.timestamp_ns = opened_ns,
		.mode         = inherited ? lifecycle::StartMode::RECOVERED
								  : lifecycle::StartMode::COLD};
	spdlog::info("{}", opened);

	if (inherited) {
		// Which sources actually contributed, rather than which were available:
		// a bare JOURNAL is the case worth alerting on, because it means the
		// venue replayed from nothing and has lost its checkpoints.
		lifecycle::recovery_modes source;
		if (*restored != 0)
			source.set(
				lifecycle::recovery_modes{lifecycle::recovery_mode::SNAPSHOT});
		if (*replayed != 0)
			source.set(
				lifecycle::recovery_modes{lifecycle::recovery_mode::JOURNAL});
		const lifecycle::recovery rebuilt{.session          = opened.session,
										  .recovered_from   = opening.session,
										  .timestamp_ns     = wall_clock_ns(),
										  .source           = source,
										  .entries_replayed = *replayed,
										  .orders_restored  = *restored};
		spdlog::info("{}", rebuilt);
		if (!rebuilt.is_well_formed())
			spdlog::warn(
				"the recovery record is not well formed - the previous "
				"session left no id to continue from");
	}

	// --- steady state -------------------------------------------------------
	std::uint64_t applied = *replayed;
	if (!settings.recover_only && settings.orders != 0) {
		partition.attach_journal(&store->journal());
		// Numbering continues past everything the journal already holds,
		// because a recovered session inherits the previous one's live ids.
		const auto first =
			static_cast<order_id_t>(store->journal().count() + 1);
		const auto flow = resting_flow(first, settings.orders);
		for (const event::command &cmd : flow)
			while (!partition.submit(cmd)) applied += drain_fully(partition);
		applied += drain_fully(partition);
		if (partition.journal_failures() != 0) {
			spdlog::error("{} journal failures; this run's output was withheld "
						  "rather than published undurably",
						  partition.journal_failures());
			return EXIT_FAILURE;
		}
		spdlog::info("added {} resting orders from id {}",
					 settings.orders,
					 first);
	}

	// --- checkpoint ---------------------------------------------------------
	if (settings.checkpoint) {
		// Write, sync, then commit - and commit refuses an id with no file
		// behind it, so a crash between the two leaves the previous checkpoint
		// current and this snapshot orphaned rather than half-adopted.
		const std::uint64_t id = store->next_snapshot_id();
		const auto written = execution::save_snapshot(partition.books(),
													  store->snapshot_path(id));
		if (!written) {
			spdlog::error("snapshot write failed: {}", written.error());
			return EXIT_FAILURE;
		}
		if (const auto committed = store->commit(id, opened.session);
			!committed) {
			spdlog::error("checkpoint commit failed: {}", committed.error());
			return EXIT_FAILURE;
		}
		spdlog::info("checkpoint {} covers {} journal records ({} orders)",
					 id,
					 store->checkpoint().sequence,
					 *written);
	}

	spdlog::info("{}",
				 lifecycle::shutdown{.session      = opened.session,
									 .timestamp_ns = wall_clock_ns(),
									 .reason = lifecycle::StopReason::CLEAN,
									 .commands_applied = applied,
									 .events_published = 0});

	// The result, on stdout: the books as they will be found next run.
	fmt::println("listing {}: {}", LEFT, *partition.book(LEFT));
	fmt::println("listing {}: {}", RIGHT, *partition.book(RIGHT));
	fmt::println("journal {} records, checkpoint at {}",
				 store->journal().count(),
				 store->checkpoint().sequence);
	return EXIT_SUCCESS;
}

} // namespace exchange::app
