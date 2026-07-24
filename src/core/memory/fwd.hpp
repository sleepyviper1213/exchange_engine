#pragma once
// Forward declarations for the allocator submodule. Include this instead of the
// full headers when only a name (pointer/reference/return type) is needed.

namespace memory {

struct Arena;
class FreeList;
class NumaArenaAllocator;
class Slab;
class MallocResource;
class ArenaResource;

namespace pool {
/// Hazard-pointer object pool (memory/freelist.hpp). Distinct from free_list:
/// this constructs/destroys a T, that recycles raw same-size blocks.
template <class T>
class freelist;
} // namespace pool

template <typename T>
class object_pool;

template <typename T>
class NodePool;

template <class T, class Resource>
class Allocator;

} // namespace memory
