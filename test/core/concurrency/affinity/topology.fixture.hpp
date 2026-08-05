#pragma once
// Shared by topology.test.cpp and core_allocator.test.cpp: a topology built
// from explicit sibling groups, so neither depends on the host's real CPU
// layout.

#include "core/concurrency/affinity.hpp"

#include <utility>
#include <vector>

namespace exchange::test::affinity {

using namespace exchange::core::concurrency::affinity;

/// @brief Build a topology from explicit sibling groups. Group i is one
///        physical core; the core_ids it lists are that core's SMT siblings.
inline Topology make_topology(std::vector<std::vector<core_id> > groups) {
	return detail::from_sibling_groups(std::move(groups));
}

/// @brief 2 physical cores, 2 SMT siblings each: cpus {0,1} and {2,3}.
inline Topology two_by_two() { return make_topology({{0, 1}, {2, 3}}); }

} // namespace exchange::test::affinity
