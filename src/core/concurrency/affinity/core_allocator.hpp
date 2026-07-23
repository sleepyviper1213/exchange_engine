#pragma once

#include "affinity.hpp"
#include "fwd.hpp"
#include "topology.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

// Assigns a dedicated logical CPU to each named hot-path role (matching-engine,
// producer, consumer, market-data, …) so no two of them are pinned to the same
// core, and — by default — no two share a physical core's SMT siblings. This
// replaces hand-picked magic core numbers with topology-driven placement.
//
// Pure bookkeeping over a Topology: no syscalls except the pin_this_thread_to
// convenience. Configure it once at startup, single-threaded, before spawning
// the roles it hands out — it is not synchronized.
namespace concurrency::affinity {

class CoreAllocator {
public:
	explicit CoreAllocator(Topology topo) : topo_(std::move(topo)) {}

	/// Reserve a dedicated logical CPU for @p role.
	/// @param priority Scheduling priority applied by pin_this_thread_to when the
	///        role's thread pins itself (default Normal).
	/// @param distinct_physical When true (default) prefer a CPU on a physical
	///        core no other role holds, avoiding SMT-sibling contention; falls
	///        back to any free logical CPU once physical cores run out.
	/// @return The assigned CoreId; the role's existing reservation if it was
	///         already reserved (idempotent); or std::nullopt when no logical
	///         CPU remains free.
	std::optional<CoreId> reserve(std::string_view role,
								  ThreadPriority priority = ThreadPriority::Normal,
								  bool distinct_physical = true) {
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

	/// The CoreId reserved for @p role, or std::nullopt if never reserved.
	[[nodiscard]] std::optional<CoreId> core_for(std::string_view role) const {
		const auto it = roles_.find(std::string(role));
		if (it == roles_.end()) return std::nullopt;
		return it->second.core;
	}

	/// The scheduling priority reserved for @p role, or std::nullopt if never
	/// reserved.
	[[nodiscard]] std::optional<ThreadPriority>
	priority_for(std::string_view role) const {
		const auto it = roles_.find(std::string(role));
		if (it == roles_.end()) return std::nullopt;
		return it->second.priority;
	}

	/// Pin the CALLING thread to the core reserved for @p role AND apply the
	/// role's reserved scheduling priority. Call from inside that role's thread.
	/// @return true only if both the pin and the priority took effect; false if
	///         @p role is unreserved or either syscall failed (unsupported
	///         platform, denied permission). Both are best-effort — a false
	///         return costs scheduling determinism, never correctness.
	[[nodiscard]] bool pin_this_thread_to(std::string_view role) const {
		const auto it = roles_.find(std::string(role));
		if (it == roles_.end()) return false;
		const bool pinned      = pin_this_thread(it->second.core);
		const bool prioritized = set_this_thread_priority(it->second.priority);
		return pinned && prioritized;
	}

	[[nodiscard]] const Topology &topology() const noexcept { return topo_; }

	/// Logical CPUs not yet handed to any role.
	[[nodiscard]] unsigned free_cores() const noexcept {
		return topo_.logical_cpus - static_cast<unsigned>(used_cpu_.size());
	}

private:
	// First unused core, in ascending CoreId order. When fresh_physical is set,
	// restrict to primary siblings of physical cores no role holds yet.
	[[nodiscard]] const Core *find_free(bool fresh_physical) const {
		for (const Core &c : topo_.cores) {
			if (used_cpu_.contains(c.id)) continue;
			if (fresh_physical && (!c.primary_sibling ||
								   used_physical_.contains(c.physical_core)))
				continue;
			return &c;
		}
		return nullptr;
	}

	/// A role's assigned core and the priority to apply when it pins itself.
	struct Reservation {
		CoreId core;
		ThreadPriority priority;
	};

	Topology topo_;
	std::unordered_set<CoreId> used_cpu_;
	std::unordered_set<unsigned> used_physical_;
	std::unordered_map<std::string, Reservation> roles_;
};

} // namespace concurrency::affinity
