#include "book_snapshot.hpp"

#include "book_manager.hpp"
#include "core/persistence/record_log.hpp"
#include "order_book/resting_view.hpp"

#include <fmt/std.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace exchange::engine::execution {
namespace {

/// @brief Orders staged before each write. Batching turns one @c fwrite per
///        resting order into one per this many, which for a book of any size is
///        the difference between a snapshot and a syscall storm.
constexpr std::size_t BATCH = 256;

} // namespace

std::expected<std::uint64_t, std::string>
save_snapshot(const book_manager &books, const std::filesystem::path &path) {
	// Truncated, not appended: a snapshot is a whole statement about one
	// moment. Appending would leave the previous snapshot's orders in front of
	// this one's, and a load would restore both - every id from the older one
	// colliding with the newer, which the duplicate check would then silently
	// drop.
	std::error_code ec;
	std::filesystem::remove(path, ec);
	if (ec)
		return std::unexpected(
			fmt::format("cannot replace snapshot {}: {}", path, ec.message()));

	auto log = snapshot_log::open_for_append(path);
	if (!log) return std::unexpected(std::move(log.error()));

	// A vector rather than a fixed array, because resting_view holds an
	// order_state and an order_state has no default constructor - a state
	// without a quantity is not a state, and the type says so. push_back needs
	// only the copy constructor that trivial copyability already gives.
	// Reserved once, so the allocation is one at the top and not one per batch.
	std::vector<resting_record> staged;
	staged.reserve(BATCH);
	std::uint64_t saved = 0;
	bool ok             = true;

	const auto push = [&] {
		if (staged.empty()) return;
		ok = ok && log->append(std::span<const resting_record>(staged));
		staged.clear();
	};

	books.for_each_listing([&](symbol_id_t symbol, const order_book &book) {
		book.for_each_resting([&](const resting_view &order) {
			staged.emplace_back(symbol, order);
			++saved;
			if (staged.size() == BATCH) push();
		});
	});
	push();

	if (!ok)
		return std::unexpected(fmt::format("cannot write snapshot {}", path));

	// Synced here rather than left to the caller, because the caller's next act
	// is to commit a manifest naming this file - and a manifest may not point
	// at something the operating system has not written yet.
	if (!log->sync())
		return std::unexpected(fmt::format("cannot sync snapshot {}", path));

	return saved;
}

std::expected<load_report, std::string>
load_snapshot_reporting(book_manager &books,
						const std::filesystem::path &path) {
	auto log = snapshot_log::open_for_read(path);
	if (!log) return std::unexpected(std::move(log.error()));

	load_report report;
	alignas(resting_record)
		std::array<std::byte, sizeof(resting_record) * BATCH>
			storage{};

	const std::uint64_t total = log->count();
	for (std::uint64_t at = 0; at < total;) {
		const std::span<const resting_record> batch =
			log->read_into(at, storage.data(), BATCH);
		if (batch.empty()) break;

		for (const resting_record &record : batch) {
			order_book *book = books.lookup(record.symbol);
			// A listing this partition does not carry. Counted rather than
			// invented: a snapshot and a partition assignment that disagree is
			// a configuration fault, and handing back a total that does not
			// match the file is how it gets noticed.
			if (book == nullptr) {
				++report.skipped;
				continue;
			}
			if (book->restore_order(record.order)) ++report.restored;
			else ++report.skipped;
		}
		at += batch.size();
	}
	return report;
}

std::expected<std::uint64_t, std::string>
load_snapshot(book_manager &books, const std::filesystem::path &path) {
	return load_snapshot_reporting(books, path)
		.transform([](const load_report &report) { return report.restored; });
}

} // namespace exchange::engine::execution
