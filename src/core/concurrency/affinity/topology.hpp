#pragma once

#include "core_export.hpp" // CORE_EXPORT (generated)
#include "fwd.hpp"
#include "types.hpp"

#include <vector>

// Machine CPU topology: which logical CPUs share a physical core (SMT
// siblings). The map is what lets the allocator spread hot roles across
// *physical* cores instead of accidentally co-scheduling two of them on one
// core's shared L1/L2.
//
// Discovery is done once at startup and is not on any hot path, so the queries
// are compiled into topology.cpp rather than inlined here. That is what keeps
// the OS query surface out of every translation unit that merely names a
// topology: <windows.h> on Win32, and <fstream>/<map>/fmt for the sysfs walk on
// Linux, are now included by exactly one .cpp.
namespace exchange::core::concurrency::affinity {

/// One logical CPU and the physical core it belongs to.
struct core {
	core_id id;           ///< OS logical-CPU index.
	unsigned
		physical_core;    ///< Dense physical-core index in [0, physical_cores).
	bool primary_sibling; ///< True for the lowest-id logical CPU of its core.
	unsigned llc_group =
		0; ///< Dense index of the last-level cache this CPU shares.
};

/// Snapshot of the host's CPU layout. @c cores is ordered by ascending core_id.
struct topology {
	unsigned logical_cpus   = 1; ///< Total logical CPUs (hardware threads).
	unsigned physical_cores = 1; ///< Distinct physical cores.
	unsigned llc_count =
		1; ///< Distinct last-level caches (L3, or the deepest present).
	bool smt = false;        ///< True when logical_cpus > physical_cores.
	std::vector<core> cores; ///< One entry per logical CPU.

	/// The primary (lowest-id) logical CPU of each physical core - the set to
	/// pin to when you want one thread per physical core, no sibling sharing.
	[[nodiscard]] CORE_EXPORT std::vector<core_id> primary_core_ids() const;

	/// True when @p a and @p b share a last-level cache - the cheap cross-core
	/// hand-off (a line bounces within one LLC instead of across sockets). A
	/// core always shares with itself; an unknown core_id yields false.
	[[nodiscard]] CORE_EXPORT bool share_llc(core_id a, core_id b) const;

	/// Every logical CPU sharing @p core's last-level cache, itself included
	/// (empty if @p core is unknown). Use to place a producer/consumer pair on
	/// LLC-close cores, or to keep contending roles on separate LLCs.
	[[nodiscard]] CORE_AUTOTEST_EXPORT std::vector<core_id> llc_peers(core_id id) const;

private:
	/// LLC group index of @p id, or -1 if no such core. Never leaves core.
	[[nodiscard]] int llc_group_of(core_id id) const;
};

namespace detail {

// The two builders below are the seam the tests drive: they let a test assemble
// a synthetic topology (a 2-socket SMT box on a single-core CI runner) without
// an OS query. Nothing else in the project calls them, so they carry
// CORE_AUTOTEST_EXPORT - the symbol is exported only in a test build and stays
// out of the shipping library's export table. The actual OS probes have no
// declaration here at all; they are internal to topology.cpp.

/// Assemble a topology from sibling groups (each group = the logical CPUs of
/// one physical core). Groups are assigned dense physical indices in arrival
/// order; the lowest core_id in a group is its primary sibling.
[[nodiscard]] CORE_AUTOTEST_EXPORT topology
from_sibling_groups(std::vector<std::vector<core_id>> groups);

/// Overlay LLC-sharing onto @p topo. With no cache info, assume a single shared
/// last-level cache (the common single-socket case) so share_llc stays usable.
CORE_AUTOTEST_EXPORT void
assign_llc(topology &topo, const std::vector<std::vector<core_id>> &groups);

} // namespace detail

/// Discover the host CPU topology - SMT siblings and last-level-cache sharing.
/// Never throws for platform reasons: an unavailable or partial OS query
/// degrades gracefully (flat physical-core model; single shared LLC) so callers
/// always get a usable, pinnable layout.
[[nodiscard]] CORE_EXPORT topology discover();

} // namespace exchange::core::concurrency::affinity
