#include "event_store.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <system_error>

#include <fmt/format.h>

namespace exchange::core::persistence {
namespace {

/// @brief The journal's name inside a store. One name, in one place — recovery
///        and steady state must agree on it or the venue silently starts a
///        second journal and forgets the first.
constexpr const char *JOURNAL_LEAF  = "journal.bin";
constexpr const char *MANIFEST_LEAF = "manifest";

/// @brief Digits a 64-bit decimal needs at its widest, so every representable
///        snapshot id pads to the same length and sorts lexically.
constexpr int SNAPSHOT_ID_WIDTH = 20;

} // namespace

std::expected<void, std::string>
ensure_directory(const std::filesystem::path &root) {
	std::error_code ec;
	// create_directories reports false for "it already existed", which is not an
	// error here — the distinction that matters is whether it exists *now*, so
	// the return value is ignored and the error code is what gets checked.
	std::filesystem::create_directories(root, ec);
	if (ec)
		return std::unexpected(fmt::format("cannot create store directory {}: {}",
										   root.string(),
										   ec.message()));
	if (!std::filesystem::is_directory(root))
		return std::unexpected(
			fmt::format("store path is not a directory: {}", root.string()));
	return {};
}

std::filesystem::path journal_path(const std::filesystem::path &root) {
	return root / JOURNAL_LEAF;
}

std::filesystem::path manifest_path(const std::filesystem::path &root) {
	return root / MANIFEST_LEAF;
}

std::filesystem::path snapshot_path(const std::filesystem::path &root,
									std::uint64_t id) {
	return root / fmt::format("snapshot-{:0{}}.bin", id, SNAPSHOT_ID_WIDTH);
}

} // namespace exchange::core::persistence
