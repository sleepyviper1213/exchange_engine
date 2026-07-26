#pragma once

#include "affinity.hpp"
#include "fwd.hpp"

#include <algorithm>
#include <cstddef>
#include <vector>

#ifdef __linux__
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <fmt/format.h>
#endif

// Machine CPU topology: which logical CPUs share a physical core (SMT
// siblings). The map is what lets the allocator spread hot roles across
// *physical* cores instead of accidentally co-scheduling two of them on one
// core's shared L1/L2.
//
// Discovery is done once at startup and is not on any hot path, so the queries
// live inline here rather than in a compiled TU — consistent with the rest of
// the header-only concurrency layer.
namespace exchange::core::concurrency::affinity {

/// One logical CPU and the physical core it belongs to.
struct Core {
	CoreId id;            ///< OS logical-CPU index.
	unsigned
		physical_core;    ///< Dense physical-core index in [0, physical_cores).
	bool primary_sibling; ///< True for the lowest-id logical CPU of its core.
	unsigned llc_group =
		0; ///< Dense index of the last-level cache this CPU shares.
};

/// Snapshot of the host's CPU layout. @c cores is ordered by ascending CoreId.
struct Topology {
	unsigned logical_cpus   = 1; ///< Total logical CPUs (hardware threads).
	unsigned physical_cores = 1; ///< Distinct physical cores.
	unsigned llc_count =
		1; ///< Distinct last-level caches (L3, or the deepest present).
	bool smt = false;        ///< True when logical_cpus > physical_cores.
	std::vector<Core> cores; ///< One entry per logical CPU.

	/// The primary (lowest-id) logical CPU of each physical core — the set to
	/// pin to when you want one thread per physical core, no sibling sharing.
	[[nodiscard]] std::vector<CoreId> primary_core_ids() const {
		std::vector<CoreId> ids;
		ids.reserve(physical_cores);
		for (const Core &c : cores)
			if (c.primary_sibling) ids.push_back(c.id);
		return ids;
	}

	/// True when @p a and @p b share a last-level cache — the cheap cross-core
	/// hand-off (a line bounces within one LLC instead of across sockets). A
	/// core always shares with itself; an unknown CoreId yields false.
	[[nodiscard]] bool share_llc(CoreId a, CoreId b) const {
		const int g = llc_group_of(a);
		return g >= 0 && g == llc_group_of(b);
	}

	/// Every logical CPU sharing @p core's last-level cache, itself included
	/// (empty if @p core is unknown). Use to place a producer/consumer pair on
	/// LLC-close cores, or to keep contending roles on separate LLCs.
	[[nodiscard]] std::vector<CoreId> llc_peers(CoreId core) const {
		std::vector<CoreId> peers;
		const int g = llc_group_of(core);
		if (g < 0) return peers;
		for (const Core &c : cores)
			if (static_cast<int>(c.llc_group) == g) peers.push_back(c.id);
		return peers;
	}

private:
	/// LLC group index of @p id, or -1 if no such core.
	[[nodiscard]] int llc_group_of(CoreId id) const {
		for (const Core &c : cores)
			if (c.id == id) return static_cast<int>(c.llc_group);
		return -1;
	}
};

namespace detail {

/// Assemble a Topology from sibling groups (each group = the logical CPUs of
/// one physical core). Groups are assigned dense physical indices in arrival
/// order; the lowest CoreId in a group is its primary sibling.
[[nodiscard]] inline Topology
from_sibling_groups(std::vector<std::vector<CoreId>> groups) {
	Topology topo;
	topo.physical_cores = static_cast<unsigned>(groups.size());
	unsigned logical    = 0;
	for (unsigned phys = 0; phys < groups.size(); ++phys) {
		std::vector<CoreId> &siblings = groups[phys];
		std::ranges::sort(siblings);
		for (std::size_t i = 0; i < siblings.size(); ++i) {
			topo.cores.push_back(Core{.id              = siblings[i],
									  .physical_core   = phys,
									  .primary_sibling = (i == 0)});
			++logical;
		}
	}
	std::ranges::sort(topo.cores,
					  [](const Core &a, const Core &b) { return a.id < b.id; });
	topo.logical_cpus = logical;
	topo.smt          = topo.logical_cpus > topo.physical_cores;
	return topo;
}

/// Fallback used when the OS query is unavailable: every logical CPU is treated
/// as its own physical core (no SMT knowledge, but still safe to pin to).
[[nodiscard]] inline Topology flat_topology() {
	const unsigned n = logical_cpu_count();
	std::vector<std::vector<CoreId>> groups;
	groups.reserve(n);
	for (unsigned i = 0; i < n; ++i) groups.push_back({CoreId{i}});
	return from_sibling_groups(std::move(groups));
}

#ifdef _WIN32
[[nodiscard]] inline Topology discover_impl() {
	DWORD len = 0;
	// First call sizes the buffer (fails with ERROR_INSUFFICIENT_BUFFER).
	GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
	if (len == 0) return flat_topology();

	std::vector<std::byte> buffer(len);
	if (!GetLogicalProcessorInformationEx(
			RelationProcessorCore,
			reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
				buffer.data()),
			&len))
		return flat_topology();

	std::vector<std::vector<CoreId>> groups;
	std::byte *ptr       = buffer.data();
	std::byte *const end = buffer.data() + len;
	while (ptr < end) {
		auto *info =
			reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(ptr);
		if (info->Relationship == RelationProcessorCore) {
			// One record per physical core; its mask holds the SMT siblings.
			// Group 0 only — the 64-CPU ceiling matches the affinity mask.
			const KAFFINITY mask = info->Processor.GroupMask[0].Mask;
			std::vector<CoreId> siblings;
			for (unsigned cpu = 0; cpu < 64U; ++cpu)
				if ((mask >> cpu) & 1U) siblings.push_back(CoreId{cpu});
			if (!siblings.empty()) groups.push_back(std::move(siblings));
		}
		ptr += info->Size;
	}
	return groups.empty() ? flat_topology()
						  : from_sibling_groups(std::move(groups));
}

#elifdef __linux__
[[nodiscard]] inline Topology discover_impl() {
	// Group logical CPUs by (physical_package_id, core_id) read from sysfs.
	// Keying on the pair distinguishes same-numbered cores on different
	// sockets.
	std::map<std::pair<int, int>, std::vector<CoreId>> groups;
	const auto read_int = [](const std::string &path, int &out) -> bool {
		std::ifstream in(path);
		return static_cast<bool>(in >> out);
	};

	const unsigned n = logical_cpu_count();
	for (unsigned cpu = 0; cpu < n; ++cpu) {
		const auto base =
			fmt::format("/sys/devices/system/cpu/cpu{}/topology/", cpu);
		int core_id = 0;
		int pkg_id  = 0;
		if (!read_int(base + "core_id", core_id) ||
			!read_int(base + "physical_package_id", pkg_id))
			return flat_topology(); // sysfs absent/partial — bail to flat
									// model.
		groups[{pkg_id, core_id}].push_back(CoreId{cpu});
	}
	if (groups.empty()) return flat_topology();

	std::vector<std::vector<CoreId>> sibling_groups;
	sibling_groups.reserve(groups.size());
	for (auto &[key, siblings] : groups)
		sibling_groups.push_back(std::move(siblings));
	return from_sibling_groups(std::move(sibling_groups));
}

#else
[[nodiscard]] inline Topology discover_impl() { return flat_topology(); }
#endif

// --- last-level-cache sharing ------------------------------------------------
// Each returned group is the set of logical CPUs sharing one last-level cache.
// Empty result means "unknown" — the caller then assumes a single shared LLC.

#ifdef _WIN32
[[nodiscard]] inline std::vector<std::vector<CoreId>> llc_groups_impl() {
	DWORD len = 0;
	GetLogicalProcessorInformationEx(RelationCache, nullptr, &len);
	if (len == 0) return {};
	std::vector<std::byte> buffer(len);
	if (!GetLogicalProcessorInformationEx(
			RelationCache,
			reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
				buffer.data()),
			&len))
		return {};

	std::byte *const begin = buffer.data();
	std::byte *const end   = buffer.data() + len;

	// Data/unified caches only (an instruction cache never holds the shared
	// line); the deepest level present is the LLC — usually L3, sometimes L2.
	const auto is_data = [](const CACHE_RELATIONSHIP &c) {
		return c.Type == CacheUnified || c.Type == CacheData;
	};
	BYTE max_level = 0;
	for (std::byte *ptr = begin; ptr < end;) {
		auto *info =
			reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(ptr);
		if (info->Relationship == RelationCache && is_data(info->Cache) &&
			info->Cache.Level > max_level)
			max_level = info->Cache.Level;
		ptr += info->Size;
	}
	if (max_level == 0) return {};

	std::vector<std::vector<CoreId>> groups;
	for (std::byte *ptr = begin; ptr < end;) {
		auto *info =
			reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(ptr);
		if (info->Relationship == RelationCache && is_data(info->Cache) &&
			info->Cache.Level == max_level) {
			// GroupMask (group 0) — the 64-CPU ceiling matches the affinity
			// mask.
			const KAFFINITY mask = info->Cache.GroupMask.Mask;
			std::vector<CoreId> cpus;
			for (unsigned cpu = 0; cpu < 64U; ++cpu)
				if ((mask >> cpu) & 1U) cpus.push_back(CoreId{cpu});
			if (!cpus.empty()) groups.push_back(std::move(cpus));
		}
		ptr += info->Size;
	}
	return groups;
}

#elifdef __linux__
[[nodiscard]] inline std::vector<std::vector<CoreId>> llc_groups_impl() {
	// CPUs sharing a cache report an identical shared_cpu_list, so group on
	// that string for each CPU's deepest data/unified cache index.
	std::map<std::string, std::vector<CoreId>> groups;
	const unsigned n = logical_cpu_count();
	for (unsigned cpu = 0; cpu < n; ++cpu) {
		const auto base = fmt::format("/sys/devices/system/cpu/cpu{}/cache/");
		int best_level  = -1;
		std::string best_shared;
		for (unsigned idx = 0;; ++idx) {
			const auto dir = fmt::format("{}index{}/", base, idx);
			int level      = 0;
			if (std::ifstream lvl(dir + "level"); !(lvl >> level))
				break;                           // no more
			std::string type;
			std::ifstream(dir + "type") >> type; // Data | Instruction | Unified
			if (type == "Instruction") continue;
			if (level > best_level)
				if (std::string list;
					std::ifstream(dir + "shared_cpu_list") >> list) {
					best_level  = level;
					best_shared = std::move(list);
				}
		}
		if (best_level < 0) return {}; // no cache info — leave LLC unknown
		groups[best_shared].push_back(CoreId{cpu});
	}
	std::vector<std::vector<CoreId>> out;
	out.reserve(groups.size());
	for (auto &[key, cpus] : groups) out.push_back(std::move(cpus));
	return out;
}

#else
[[nodiscard]] inline std::vector<std::vector<CoreId>> llc_groups_impl() {
	return {};
}
#endif

/// Overlay LLC-sharing onto @p topo. With no cache info, assume a single shared
/// last-level cache (the common single-socket case) so share_llc stays usable.
inline void assign_llc(Topology &topo,
					   const std::vector<std::vector<CoreId>> &groups) {
	if (groups.empty()) {
		topo.llc_count = 1;
		for (Core &c : topo.cores) c.llc_group = 0;
		return;
	}
	topo.llc_count = static_cast<unsigned>(groups.size());
	for (Core &c : topo.cores)
		for (unsigned g = 0; g < groups.size(); ++g)
			if (std::ranges::find(groups[g], c.id) != groups[g].end()) {
				c.llc_group = g;
				break;
			}
}

} // namespace detail

/// Discover the host CPU topology — SMT siblings and last-level-cache sharing.
/// Never throws for platform reasons: an unavailable or partial OS query
/// degrades gracefully (flat physical-core model; single shared LLC) so callers
/// always get a usable, pinnable layout.
[[nodiscard]] inline Topology discover() {
	Topology topo = detail::discover_impl();
	detail::assign_llc(topo, detail::llc_groups_impl());
	return topo;
}

} // namespace exchange::core::concurrency::affinity
