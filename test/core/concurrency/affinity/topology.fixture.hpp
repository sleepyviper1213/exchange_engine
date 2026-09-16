#pragma once
// Shared by topology.test.cpp and core_allocator.test.cpp: a topology built
// from explicit sibling groups, so neither depends on the host's real CPU
// layout.

#include "core/concurrency/affinity.hpp"

#include <utility>
#include <vector>


using namespace exchange::core::concurrency::affinity;

/// @brief Build a topology from explicit sibling groups. Group i is one
///        physical core; the core_ids it lists are that core's SMT siblings.
///
/// @note Fully qualified rather than relying on the using-directive above.
///       Every module in this tree has a @c detail namespace, so an unqualified
///       @c detail is ambiguous the moment two of them are visible - which is
///       what a unity batch arranges. @see test/CMakeLists.txt
inline topology make_topology(std::vector<std::vector<core_id>> groups) {
	return exchange::core::concurrency::affinity::detail::from_sibling_groups(
		std::move(groups));
}

/// @brief 2 physical cores, 2 SMT siblings each: cpus {0,1} and {2,3}.
inline topology two_by_two() { return make_topology({{0, 1}, {2, 3}}); }

/// @brief Stamp kernel isolation onto @p topo - the CPUs a boot parameter took
///        out of the scheduler's domains. A test cannot boot a kernel, so this
///        is the seam that stands in for @c discover_isolation().
/// @param isolated_cpus The @c isolcpus= set. Ids naming no CPU in @p topo are
///        ignored, exactly as on a host whose boot line over-names.
inline topology make_isolated_topology(topology topo,
									   std::vector<core_id> isolated_cpus) {
	exchange::core::concurrency::affinity::detail::apply_isolation(
		topo,
		isolation{.isolated = std::move(isolated_cpus), .nohz_full = {}});
	return topo;
}
