#pragma once

#include <cassert>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

template <typename T>
    requires(std::is_trivially_copyable_v<T>)
class ObjectPool {
public:
    using index_type = std::int32_t;
    static constexpr index_type null_index = -1;

    struct Node {
        T value{};
        index_type next = null_index;
        index_type prev = null_index;
    };

    explicit ObjectPool(std::size_t capacity = 524288) : nodes_(capacity) {
        free_list_.reserve(capacity);

        // LIFO allocation: 0,1,2,...
        for (index_type i = static_cast<index_type>(capacity) - 1; i >= 0; --i)
            free_list_.push_back(i);
    }

    template <typename... Args>
    [[nodiscard]]
    index_type allocate(Args&&... args) {
        assert(!free_list_.empty());

        const index_type idx = free_list_.back();
        free_list_.pop_back();

        auto& node = nodes_[idx];
        node.value = T{std::forward<Args>(args)...};
        node.next = null_index;
        node.prev = null_index;

        return idx;
    }

    void deallocate(index_type idx) noexcept { free_list_.push_back(idx); }

    template <class Self>
    [[nodiscard]]
    auto&& get(this Self&& self, index_type idx) noexcept {
        if (idx == null_index) std::abort();

        assert(idx >= 0);
        assert(static_cast<size_t>(idx) < self.nodes_.size());
        return std::forward<Self>(self).nodes_[idx];
    }

    [[nodiscard]]
    std::size_t size() const noexcept {
        return nodes_.size() - free_list_.size();
    }

    [[nodiscard]]
    std::size_t capacity() const noexcept {
        return nodes_.size();
    }

private:
    std::vector<Node> nodes_;
    std::vector<index_type> free_list_;
};
