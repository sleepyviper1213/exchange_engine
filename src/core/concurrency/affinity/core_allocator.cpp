#include "core_allocator.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace exchange::core::concurrency::affinity {

CoreAllocator::CoreAllocator(Topology topo) : topo_(std::move(topo)) {}

std::optional<core_id> CoreAllocator::reserve(std::string_view role,
											  thread_priority priority,
											  bool distinct_physical) {
	if (const auto existing = core_for(role)) return existing;

	const Core *pick = nullptr;
	if (distinct_physical) pick = find_free(/*fresh_physical=*/true);
	if (pick == nullptr) pick = find_free(/*fresh_physical=*/false);
	if (pick == nullptr) return std::nullopt;

	used_cpu_.insert(pick->id);
	used_physical_.insert(pick->physical_core);
	roles_.emplace(std::string(role), Reservation{pick->id, priority});
	return pick->id;
}

std::optional<core_id> CoreAllocator::core_for(std::string_view role) const {
	const auto it = roles_.find(std::string(role));
	if (it == roles_.end()) return std::nullopt;
	return it->second.core;
}

std::optional<thread_priority>
CoreAllocator::priority_for(std::string_view role) const {
	const auto it = roles_.find(std::string(role));
	if (it == roles_.end()) return std::nullopt;
	return it->second.priority;
}

bool CoreAllocator::pin_this_thread_to(std::string_view role) const {
	const auto it = roles_.find(std::string(role));
	if (it == roles_.end()) return false;
	const bool pinned      = pin_this_thread(it->second.core);
	const bool prioritised = set_this_thread_priority(it->second.priority);
	return pinned && prioritised;
}

const Topology &CoreAllocator::topology() const noexcept { return topo_; }

unsigned CoreAllocator::free_cores() const noexcept {
	return topo_.logical_cpus - static_cast<unsigned>(used_cpu_.size());
}

const Core *CoreAllocator::find_free(bool fresh_physical) const {
	for (const Core &c : topo_.cores) {
		if (used_cpu_.contains(c.id)) continue;
		if (fresh_physical &&
			(!c.primary_sibling || used_physical_.contains(c.physical_core)))
			continue;
		return &c;
	}
	return nullptr;
}

} // namespace exchange::core::concurrency::affinity
