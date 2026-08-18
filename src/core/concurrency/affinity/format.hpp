#pragma once
// fmt formatters for the CPU-topology value types.
//
// Opt-in, like fmt's own fmt/std.h and fmt/ranges.h: only translation units
// that actually print a topology pay for <fmt/format.h>, so topology.hpp stays
// includable by the pinning path without it. This sits at the affinity
// subsystem rather than at core/, because core/ spans concurrency, memory and
// persistence, and a module-wide format.hpp there would make every one of those
// vocabularies a dependency of formatting any single one of them.
//
// core_id needs nothing: it is a plain `unsigned`.
//
// Both formatters derive from fmt::nested_formatter<std::string_view> and write
// through write_padded(), so standard fill/align/width apply to the whole
// record. The two go together: nested_formatter's parse() consumes the width,
// so a format() that wrote straight to ctx.out() would swallow `{:>60}` and
// emit an unpadded record. test/format.test.cpp pins that contract.

#include "topology.hpp"

#include <fmt/format.h>

#include <string_view>

/// @brief One logical CPU as @c "core[cpu=3 core=1 llc=0 primary]" - its OS
///        index, the physical core and last-level cache it belongs to, and
///        whether it is the core's primary sibling (the one to pin to when you
///        want one thread per physical core).
template <>
struct fmt::formatter<exchange::core::concurrency::affinity::core>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::core::concurrency::affinity::core &core,
				format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			return fmt::format_to(out,
								  "core[cpu={} core={} llc={} {}]",
								  core.id,
								  core.physical_core,
								  core.llc_group,
								  core.primary_sibling ? "primary" : "sibling");
		});
	}
};

/// @brief A host layout as
///        @c "topology[16 logical / 8 physical cores, SMT, 2 LLCs]".
///
/// The summary only - the per-CPU detail is the @c cores vector, which prints
/// element-wise through the core formatter above once <fmt/ranges.h> is in
/// scope: @c fmt::format("{}", topo.cores).
template <>
struct fmt::formatter<exchange::core::concurrency::affinity::topology>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::core::concurrency::affinity::topology &topo,
				format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			return fmt::format_to(out,
								  "topology[{} logical / {} physical cores, "
								  "{}, {} LLC{}]",
								  topo.logical_cpus,
								  topo.physical_cores,
								  topo.smt ? "SMT" : "no SMT",
								  topo.llc_count,
								  topo.llc_count == 1 ? "" : "s");
		});
	}
};
