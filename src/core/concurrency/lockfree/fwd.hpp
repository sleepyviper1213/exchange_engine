#pragma once
#include "core_export.hpp"

#include <cstddef> // std::size_t
#include <functional>

namespace exchange::core::concurrency::lockfree {

class CORE_EXPORT FastQueue;

template <class T, std::size_t N>
class CORE_EXPORT spsc_queue;

template <class Key, class Value, std::size_t Size, class Hash = std::hash<Key>>
class CORE_EXPORT wait_free_hash_map;

template <typename T>
class CORE_EXPORT stack; // namespace concurrency::lockfree
} // namespace concurrency::lockfree