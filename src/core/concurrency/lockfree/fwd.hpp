#pragma once

#include <cstddef> // std::size_t
#include <functional>

namespace concurrency::lockfree {

class FastQueue;

template <class T, std::size_t N>
class spsc_queue;

template <class T>
class freelist;

template <class Key, class Value, std::size_t Size, class Hash = std::hash<Key>>
class wait_free_hash_map;
} // namespace concurrency::lockfree
