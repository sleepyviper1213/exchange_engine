#include "core_allocator.hpp"

// Placement is reported, not just returned - see pin_this_thread_to. This is
// startup-only code, so the logging costs nothing any hot path pays for.
#include "core/logging.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace exchange::core::concurrency::affinity {

core_allocator::core_allocator(topology topo) : topo_(std::move(topo)) {}

std::optional<core_id> core_allocator::reserve(std::string_view role,
											   thread_priority priority,
											   bool distinct_physical) {
	if (const auto existing = core_for(role)) return existing;

	const core *pick = nullptr;
	if (distinct_physical) pick = find_free(/*fresh_physical=*/true);
	if (pick == nullptr) pick = find_free(/*fresh_physical=*/false);
	if (pick == nullptr) return std::nullopt;

	used_cpu_.insert(pick->id);
	used_physical_.insert(pick->physical_core);
	roles_.emplace(role, detail::reservation{pick->id, priority});
	return pick->id;
}

std::optional<core_id> core_allocator::core_for(std::string_view role) const {
	const auto it = roles_.find(role);
	if (it == roles_.end()) return std::nullopt;
	return it->second.core;
}

std::optional<thread_priority>
core_allocator::priority_for(std::string_view role) const {
	const auto it = roles_.find(role);
	if (it == roles_.end()) return std::nullopt;
	return it->second.priority;
}

bool core_allocator::pin_this_thread_to(std::string_view role) const {
	const auto it = roles_.find(role);
	if (it == roles_.end()) {
		// Not a best-effort failure like the two below: nobody reserved this
		// role, so the thread is running wherever the scheduler put it and no
		// amount of privilege would change that. It is a wiring mistake.
		spdlog::warn("no core reserved for role '{}'; thread left unpinned",
					 role);
		return false;
	}

	const auto [core, priority] = it->second;
	const bool pinned           = pin_this_thread(core);
	const bool prioritised      = set_this_thread_priority(priority);

	// Reported here rather than left to each caller because this is the only
	// scope that knows *which* of the two syscalls refused, and on what core -
	// a bool handed back to the call site cannot say either. Both are
	// best-effort by contract: a thread that could not pin still executes
	// correctly, it just no longer has the scheduling determinism the placement
	// was for, and a latency figure taken afterwards is measuring the scheduler
	// as much as the code.
	if (!pinned)
		spdlog::warn("role '{}' could not be pinned to cpu {} - unsupported "
					 "platform or denied permission",
					 role,
					 core);
	if (!prioritised)
		spdlog::warn("role '{}' could not be set to {} priority - insufficient "
					 "privileges (elevated process on Windows, CAP_SYS_NICE on "
					 "Linux)",
					 role,
					 priority);
	if (pinned && prioritised)
		spdlog::debug("role '{}' pinned to cpu {} at {} priority",
					  role,
					  core,
					  priority);

	return pinned && prioritised;
}

const topology &core_allocator::get_topology() const noexcept { return topo_; }

unsigned core_allocator::free_cores() const noexcept {
	return topo_.logical_cpus - static_cast<unsigned>(used_cpu_.size());
}

const core *core_allocator::find_free(bool fresh_physical) const {
	for (const core &c : topo_.cores) {
		if (used_cpu_.contains(c.id)) continue;
		if (fresh_physical &&
			(!c.primary_sibling || used_physical_.contains(c.physical_core)))
			continue;
		return &c;
	}
	return nullptr;
}

} // namespace exchange::core::concurrency::affinity
