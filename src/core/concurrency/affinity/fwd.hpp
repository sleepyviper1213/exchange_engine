#pragma once

#include <limits>

// CPU-affinity vocabulary shared across the affinity framework. Kept dependency
// -free so any header can name a core without pulling in the platform layer.
namespace exchange::core::concurrency::affinity {

/// Logical CPU index in the operating system's own numbering (what
/// sched_setaffinity / SetThreadAffinityMask address). One per hardware thread;
/// SMT siblings are distinct core_ids that share a physical core.
using core_id = unsigned;

/// Sentinel meaning "no specific core" — an unset reservation or a request to
/// leave affinity untouched.
inline constexpr core_id kNoCore = std::numeric_limits<core_id>::max();

} // namespace exchange::core::concurrency::affinity
