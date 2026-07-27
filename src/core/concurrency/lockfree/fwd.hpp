#pragma once
#include "core_export.hpp"

#include <cstddef> // std::size_t
#include <functional>

namespace exchange::core::concurrency::lockfree {

// Header-only (all members inline in fast_queue.hpp; no fast_queue.cpp), so it
// must NOT be class-level exported — a dll-interface mark would make consumers'
// inline-member uses into __imp_ references the DLL never provides (LNK2019).
class FastQueue;

template <class T, std::size_t N>
class spsc_queue;

template <class Key, class Value, std::size_t Size, class Hash = std::hash<Key>>
class wait_free_hash_map;

template <typename T>
class stack;
} // namespace exchange::core::concurrency::lockfree