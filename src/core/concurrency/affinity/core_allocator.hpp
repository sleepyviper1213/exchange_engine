#pragma once

#include "core_export.hpp" // CORE_EXPORT (generated)
#include "affinity.hpp" // ThreadPriority
#include "fwd.hpp"
#include "topology.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

// Assigns a dedicated logical CPU to each named hot-path role (matching-engine,
// producer, consumer, market-data, …) so no two of them are pinned to the same
// core, and - by default - no two share a physical core's SMT siblings. This
// replaces hand-picked magic core numbers with topology-driven placement.
//
// Pure bookkeeping over a topology: no syscalls except the pin_this_thread_to
// convenience. Configure it once at startup, single-threaded, before spawning
// the roles it hands out - it is not synchronized. Being startup-only is what
// makes core_allocator.cpp the right home for the bodies: nothing here is
// called often enough for the cross-module call to matter.
namespace exchange::core::concurrency::affinity {
namespace detail {

/// @brief A role's assigned core and the priority to apply when it pins itself.
struct reservation {
	core_id core;
	thread_priority priority;
};
} // namespace detail

class core_allocator {
public:
	CORE_EXPORT explicit core_allocator(topology topo);

	/// Reserve a dedicated logical CPU for @p role.
	/// @param priority Scheduling priority applied by pin_this_thread_to when
	/// the
	///        role's thread pins itself (default normal).
	/// @param distinct_physical When true (default) prefer a CPU on a physical
	///        core no other role holds, avoiding SMT-sibling contention; falls
	///        back to any free logical CPU once physical cores run out.
	/// @return The assigned core_id; the role's existing reservation if it was
	///         already reserved (idempotent); or std::nullopt when no logical
	///         CPU remains free.
	CORE_EXPORT std::optional<core_id>
	reserve(std::string_view role,
			thread_priority priority = thread_priority::normal,
			bool distinct_physical   = true);

	/// The core_id reserved for @p role, or std::nullopt if never reserved.
	[[nodiscard]] CORE_EXPORT std::optional<core_id> core_for(std::string_view role) const;

	/// The scheduling priority reserved for @p role, or std::nullopt if never
	/// reserved.
	[[nodiscard]] CORE_EXPORT std::optional<thread_priority>
	priority_for(std::string_view role) const;

	/// Pin the CALLING thread to the core reserved for @p role AND apply the
	/// role's reserved scheduling priority. Call from inside that role's
	/// thread.
	///
	/// Every outcome is logged here - a warning naming which of the two
	/// syscalls refused and on which core, or a debug line recording the
	/// placement that took. That is deliberate: the caller holds one bool and
	/// cannot tell an unreserved role from a denied privilege, so leaving each
	/// call site to report the failure means every one of them reports it less
	/// precisely. A caller that only wants best-effort placement can therefore
	/// discard the result knowing the failure is already on the record.
	///
	/// @return true only if both the pin and the priority took effect; false if
	///         @p role is unreserved or either syscall failed (unsupported
	///         platform, denied permission). Both are best-effort - a false
	///         return costs scheduling determinism, never correctness, so it is
	///         still returned for the callers that want to escalate it.
	[[nodiscard]] CORE_EXPORT bool pin_this_thread_to(std::string_view role) const;

	[[nodiscard]] CORE_EXPORT const topology &get_topology() const noexcept;

	/// Logical CPUs not yet handed to any role.
	[[nodiscard]] CORE_EXPORT unsigned free_cores() const noexcept;

private:
	// First unused core, in ascending core_id order. When fresh_physical is
	// set, restrict to primary siblings of physical cores no role holds yet.
	// Never leaves core, so it carries no export annotation.
	[[nodiscard]] const core *find_free(bool fresh_physical) const;

	topology topo_;
	std::unordered_set<core_id> used_cpu_;
	std::unordered_set<unsigned> used_physical_;
	std::unordered_map<std::string, detail::reservation> roles_;
};

} // namespace exchange::core::concurrency::affinity
