#pragma once
#include "core_export.hpp"

#include <cstddef> // std::size_t
#include <functional>

namespace exchange::core::concurrency::lockfree {

class CORE_EXPORT FastQueue;

template <class T, std::size_t N>
class spsc_queue;

template <class Key, class Value, std::size_t Size, class Hash = std::hash<Key>>
class wait_free_hash_map;

template <typename T>
class stack;
} // namespace exchange::core::concurrency::lockfree