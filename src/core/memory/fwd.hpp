#pragma once

#include "core_export.hpp"

namespace exchange::core::memory {
#ifdef __linux__
class CORE_AUTOTEST_EXPORT numa_arena_allocator;
#endif
class CORE_AUTOTEST_EXPORT slab;
class CORE_AUTOTEST_EXPORT malloc_resource;
class CORE_AUTOTEST_EXPORT arena_resource;

template <typename T>
class object_pool;

template <typename T>
class CORE_AUTOTEST_EXPORT node_pool;

template <class T, class Resource>
class CORE_AUTOTEST_EXPORT allocator;

class CORE_AUTOTEST_EXPORT arena;
} // namespace memory
