#pragma once

#include "affinity.hpp"    // thread_priority
#include "core/util/string_hash.hpp"
#include "core_export.hpp" // CORE_EXPORT (generated)
#include "fwd.hpp"
#include "topology.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

// Assigns a dedicated logical CPU to each named hot-path role (matching-engine,
// producer, consumer, market_data, …) so no two of them are pinned to the same
// core, and - by default - no two share a physical core's SMT siblings. This
// replaces hand-picked magic core numbers with topology-driven placement.
//
// Where the kernel has isolated CPUs (`isolcpus=`, see isolation.hpp and
// docs/deployment.md), those are handed out first, because "dedicated" means
// two different things and only one of them is worth a latency budget. Pinning
// alone gives a role a core it never leaves; isolation gives it a core nothing
// else arrives on. A role on a housekeeping CPU is still preempted by timers,
// kworkers and every other process on the box - the tens-of-microseconds
// `max_ns` docs/performance.md attributes to "the OS preempting a pinned
// thread". Roles are handed out in reservation order, so reserve the one whose
// tail you care about first.
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
	/// @note Isolated CPUs are preferred over unisolated ones ahead of the
	///       distinct-physical preference, and an isolated SMT sibling is taken
	///       before an unisolated fresh core: sharing a physical core with
	///       another role of ours costs half an L1, where leaving the isolated
	///       set costs the whole machine's interference. A role that misses the
	///       isolated set on a host that has one is logged as a warning.
	/// @return The assigned core_id; the role's existing reservation if it was
	///         already reserved (idempotent); or std::nullopt when no logical
	///         CPU remains free.
	CORE_EXPORT std::optional<core_id>
	reserve(std::string_view role,
			thread_priority priority = thread_priority::normal,
			bool distinct_physical   = true);

	/// The core_id reserved for @p role, or std::nullopt if never reserved.
	[[nodiscard]] CORE_EXPORT std::optional<core_id>
	core_for(std::string_view role) const;

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
	[[nodiscard]] CORE_EXPORT bool
	pin_this_thread_to(std::string_view role) const;

	[[nodiscard]] CORE_EXPORT const topology &get_topology() const noexcept;

	/// Logical CPUs not yet handed to any role.
	[[nodiscard]] CORE_EXPORT unsigned free_cores() const noexcept;

private:
	// First unused core, in ascending core_id order. When fresh_physical is
	// set, restrict to primary siblings of physical cores no role holds yet;
	// when isolated_only is set, restrict to CPUs the kernel isolated. reserve
	// calls it with the pairs it wants in preference order. Never leaves core,
	// so it carries no export annotation.
	[[nodiscard]] const core *find_free(bool fresh_physical,
										bool isolated_only) const;

	topology topo_;
	std::unordered_set<core_id> used_cpu_;
	std::unordered_set<unsigned> used_physical_;
	std::unordered_map<std::string, detail::reservation,
					   exchange::core::util::string_hash, std::equal_to<>>
		roles_;
};

} // namespace exchange::core::concurrency::affinity
