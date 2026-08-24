#pragma once
// Book state, written down and read back: the half of recovery that keeps replay
// from having to start at the beginning of time.
//
// A journal alone is already sufficient - the engine is deterministic, so
// replaying every command ever accepted reproduces every book exactly. What it is
// not is *bounded*: a venue that has been up for a week replays a week. A
// snapshot is the bound. It says "the books looked like this after N commands", so
// recovery loads it and replays from N instead of from zero.
//
// It sits in execution/ rather than order_book/ because it is about *all* the
// books a partition carries, which is the book manager's knowledge and not any
// one book's. The record it writes is the book's own `resting_view` with a symbol
// stamped on it - the same move `engine_event` makes, and for the same reason: a
// listing is implied while you are looking at one book and gone the moment the
// records are pooled.

#include "execution_export.hpp" // EXECUTION_EXPORT (generated)
#include "book_manager.hpp"
#include "core/persistence/record_log.hpp"
#include "fwd.hpp"
#include "order_book/resting_view.hpp"
#include "orders/types.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <type_traits>

namespace exchange::engine::execution {

/**
 * @brief One resting order in a snapshot: which listing, and the order.
 *
 * @note Trivially copyable, so a snapshot file is an array of these on disk with
 *       no encode step - the same property @c event::command relies on for the
 *       journal. @see core::persistence::record_log
 */
struct resting_record {
	symbol_id_t symbol; ///< the listing the order rests on
	resting_view order; ///< where it rests and how far through its life it is

	bool operator==(const resting_record &) const noexcept = default;
};

static_assert(std::is_trivially_copyable_v<resting_record>,
			  "a snapshot record is written as its object representation");

/// @brief The log a book snapshot is written to and read from.
using snapshot_log = core::persistence::record_log<resting_record>;

/**
 * @brief Write every resting order in @p books to a new snapshot at @p path.
 *
 * @param books The listings to snapshot.
 * @param path The file to create. Truncated if it exists - a snapshot is a whole
 *        statement about a moment, never an append to an older one.
 * @return How many resting orders were written, or why it failed.
 * @post The file is on the device: this syncs before returning, because the
 *       caller's next act is to commit a manifest naming it, and a manifest may
 *       not point at a file the operating system has not written yet.
 *
 * @par What it does not do
 * It does not stop the world. Nothing here can - a snapshot is taken on the
 * partition's own consumer thread, between drains, which is the only moment the
 * books are not being mutated. Called from anywhere else it would be reading a
 * book while its owner writes it, and the result would be a file describing a
 * state the venue was never in.
 */
[[nodiscard]] EXECUTION_EXPORT std::expected<std::uint64_t, std::string>
save_snapshot(const book_manager &books, const std::filesystem::path &path);

/**
 * @brief Restore the snapshot at @p path into @p books.
 *
 * @param books Where the orders go. Listings must already be registered -
 *        @c listing / @c create - because which symbols a partition carries is a
 *        deployment's decision and not a snapshot's to reinstate.
 * @param path The snapshot to read.
 * @return How many orders were restored, or why it failed.
 *
 * @warning A record naming a listing @p books does not carry is skipped and
 *          counted as skipped, not restored. That combination is a snapshot and a
 *          partition assignment that disagree - the same fault @c misrouted
 *          reports on the command path - and it is louder to hand back a count
 *          that does not match the file than to invent a book for it.
 * @note The books should be empty. Restoring into a populated book is not
 *       checked, because "empty" is not a question @c order_book answers, but it
 *       would merge two states into one and every duplicate id would be dropped.
 */
[[nodiscard]] EXECUTION_EXPORT std::expected<std::uint64_t, std::string>
load_snapshot(book_manager &books, const std::filesystem::path &path);

/// @brief What a load found: what it put back, and what it could not.
struct load_report {
	std::uint64_t restored = 0; ///< orders now resting again
	std::uint64_t skipped  = 0; ///< records for listings this partition lacks

	bool operator==(const load_report &) const noexcept = default;
};

/// @brief @copybrief load_snapshot
/// @return The full accounting, rather than only the restored count. @see
///         load_snapshot for the contract; this is the same call with the
///         skipped records reported instead of implied.
[[nodiscard]] EXECUTION_EXPORT std::expected<load_report, std::string>
load_snapshot_reporting(book_manager &books,
						const std::filesystem::path &path);

} // namespace exchange::engine::execution
