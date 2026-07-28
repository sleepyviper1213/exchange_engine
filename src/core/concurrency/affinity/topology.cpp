#include "topology.hpp"

#include "affinity.hpp" // logical_cpu_count

#include <algorithm>
#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

// The OS probes live behind this include wall. affinity.hpp no longer carries
// <windows.h>, so this TU pulls it in for itself.
#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#elifdef __linux__
#include <fmt/format.h>

#include <fstream>
#include <map>
#include <string>
#endif

namespace exchange::core::concurrency::affinity {

// --- Topology queries --------------------------------------------------------

std::vector<CoreId> Topology::primary_core_ids() const {
	std::vector<CoreId> ids;
	ids.reserve(physical_cores);
	for (const Core &c : cores)
		if (c.primary_sibling) ids.push_back(c.id);
	return ids;
}

bool Topology::share_llc(CoreId a, CoreId b) const {
	const int g = llc_group_of(a);
	return g >= 0 && g == llc_group_of(b);
}

std::vector<CoreId> Topology::llc_peers(CoreId core) const {
	std::vector<CoreId> peers;
	const int g = llc_group_of(core);
	if (g < 0) return peers;
	for (const Core &c : cores)
		if (static_cast<int>(c.llc_group) == g) peers.push_back(c.id);
	return peers;
}

int Topology::llc_group_of(CoreId id) const {
	for (const Core &c : cores)
		if (c.id == id) return static_cast<int>(c.llc_group);
	return -1;
}

// --- test-visible builders ---------------------------------------------------

namespace detail {

Topology from_sibling_groups(std::vector<std::vector<CoreId>> groups) {
	Topology topo;
	topo.physical_cores = static_cast<unsigned>(groups.size());
	unsigned logical    = 0;
	for (unsigned phys = 0; phys < groups.size(); ++phys) {
		std::vector<CoreId> &siblings = groups[phys];
		std::ranges::sort(siblings);
		for (std::size_t i = 0; i < siblings.size(); ++i) {
			topo.cores.emplace_back(siblings[i], phys, (i == 0));
			++logical;
		}
	}
	std::ranges::sort(topo.cores, std::less<>{}, &Core::id);
	topo.logical_cpus = logical;
	topo.smt          = topo.logical_cpus > topo.physical_cores;
	return topo;
}

void assign_llc(Topology &topo, const std::vector<std::vector<CoreId>> &groups) {
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

namespace {

/// Fallback used when the OS query is unavailable: every logical CPU is treated
/// as its own physical core (no SMT knowledge, but still safe to pin to).
[[nodiscard]] Topology flat_topology() {
	const unsigned n = logical_cpu_count();
	std::vector<std::vector<CoreId>> groups;
	groups.reserve(n);
	for (unsigned i = 0; i < n; ++i) groups.emplace_back(i);
	return detail::from_sibling_groups(std::move(groups));
}

#ifdef _WIN32
[[nodiscard]] Topology discover_impl() {
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
						  : detail::from_sibling_groups(std::move(groups));
}

#elifdef __linux__
[[nodiscard]] Topology discover_impl() {
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
		if (!read_int(fmt::format("{}core_id", base), core_id) ||
			!read_int(fmt::format("{}physical_package_id", base), pkg_id))
			return flat_topology(); // sysfs absent/partial — bail to flat
									// model.
		groups[{pkg_id, core_id}].emplace_back(cpu);
	}
	if (groups.empty()) return flat_topology();

	std::vector<std::vector<CoreId>> sibling_groups;
	sibling_groups.reserve(groups.size());
	for (auto &[key, siblings] : groups)
		sibling_groups.push_back(std::move(siblings));
	return detail::from_sibling_groups(std::move(sibling_groups));
}

#else
[[nodiscard]] Topology discover_impl() { return flat_topology(); }
#endif

// --- last-level-cache sharing ------------------------------------------------
// Each returned group is the set of logical CPUs sharing one last-level cache.
// Empty result means "unknown" — the caller then assumes a single shared LLC.

#ifdef _WIN32
[[nodiscard]] std::vector<std::vector<CoreId>> llc_groups_impl() {
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
[[nodiscard]] std::vector<std::vector<CoreId>> llc_groups_impl() {
	// CPUs sharing a cache report an identical shared_cpu_list, so group on
	// that string for each CPU's deepest data/unified cache index.
	std::map<std::string, std::vector<CoreId>> groups;
	const unsigned n = logical_cpu_count();
	for (unsigned cpu = 0; cpu < n; ++cpu) {
		const auto base =
			fmt::format("/sys/devices/system/cpu/cpu{}/cache/", cpu);
		int best_level = -1;
		std::string best_shared;
		for (unsigned idx = 0;; ++idx) {
			const auto dir = fmt::format("{}index{}/", base, idx);
			int level      = 0;
			if (std::ifstream lvl(fmt::format("{}level", dir)); !(lvl >> level))
				break; // no more
			std::string type;
			// Data | Instruction | Unified
			std::ifstream(fmt::format("{}type", dir)) >> type;
			if (type == "Instruction") continue;
			if (level > best_level)
				if (std::string list;
					std::ifstream(fmt::format("{}shared_cpu_list", dir)) >>
					list) {
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
[[nodiscard]] std::vector<std::vector<CoreId>> llc_groups_impl() { return {}; }
#endif

} // namespace

Topology discover() {
	Topology topo = discover_impl();
	detail::assign_llc(topo, llc_groups_impl());
	return topo;
}

} // namespace exchange::core::concurrency::affinity
