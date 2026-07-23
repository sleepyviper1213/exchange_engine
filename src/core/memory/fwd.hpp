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

template <typename T>
class ObjectPool;

template <typename T>
class NodePool;

template <class T, class Resource>
class Allocator;

} // namespace memory
