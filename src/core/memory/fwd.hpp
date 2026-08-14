#pragma once

#include "core_export.hpp"

namespace exchange::core::memory {
#ifdef ORDER_BOOK_WITH_NUMA
class numa_arena_allocator;
#endif

class slab;
class malloc_resource;
class arena_resource;

template <typename T>
class object_pool;

// No dllexport/dllimport on class templates: they are instantiated per-TU, so
// there is no single exported symbol, and marking them import turns an
// odr-used member (e.g. node_pool<T>::NO_NODE) into an __imp_ reference the DLL
// never provides. Matches object_pool above.
template <typename T>
class node_pool;

template <class T, class Resource>
class allocator;

class arena;
} // namespace exchange::core::memory
