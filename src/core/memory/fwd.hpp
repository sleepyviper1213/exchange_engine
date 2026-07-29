#pragma once

#include "core_export.hpp"

namespace exchange::core::memory {
#ifdef __linux__
class CORE_AUTOTEST_EXPORT numa_arena_allocator;
#endif
// slab and arena are not class-level exported: they carry STL/atomic data
// members, so exporting the whole class trips C4251 on MSVC. Instead each
// exports only the members that cross the DLL boundary (see slab.hpp/arena.hpp).
class slab;
class CORE_AUTOTEST_EXPORT malloc_resource;
class CORE_AUTOTEST_EXPORT arena_resource;

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

class arena; // per-member export (see arena.hpp); see slab note above
} // namespace exchange::core::memory
