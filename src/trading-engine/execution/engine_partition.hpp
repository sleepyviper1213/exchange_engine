#pragma once
// Engine partition (scaffold).
//
// A shard of the matching workload: a disjoint subset of symbols pinned to one
// matching engine (and its thread) so partitions run without cross-talk. Not
// implemented yet; this header only fixes the module's shape and namespace.

namespace exchange::engine::execution {

// TODO: define the engine partition.
// class EnginePartition { ... };

} // namespace exchange::engine::execution
