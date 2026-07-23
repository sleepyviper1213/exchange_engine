#pragma once

#include <atomic>

namespace memory {

/**
 * @brief Intrusive, lock-free (Treiber) stack of free blocks.
 *
 * Each freed block is at least @c sizeof(Node) bytes, so we thread the "next"
 * pointer through the block's own storage instead of allocating node objects.
 * push/pop are lock-free via CAS on the head.
 *
 * @warning Classic Treiber pop is subject to the ABA problem: if a block is
 *          popped, freed to the OS, and a new block reuses the same address
 *          before a stalled popper resumes, the CAS can succeed against a stale
 *          @c next. This port never returns blocks to the OS (arenas live for
 *          the process), so pointers are stable and ABA cannot occur here. The
 *          Rust original leaned on lockfree::queue's hazard-pointer reclamation
 *          for the same guarantee.
 */
class FreeList {
public:
    FreeList() noexcept = default;
    FreeList(const FreeList&) = delete;
    FreeList& operator=(const FreeList&) = delete;

    /// @brief Return a block to the list (block must be >= sizeof(void*)).
    void push(void* block) noexcept {
        auto* node = static_cast<Node*>(block);
        Node* head = head_.load(std::memory_order_relaxed);
        do {
            node->next = head;
        } while (!head_.compare_exchange_weak(
            head, node, std::memory_order_release, std::memory_order_relaxed));
    }

    /// @brief Pop a block for reuse, or nullptr if the list is empty.
    [[nodiscard]] void* pop() noexcept {
        Node* head = head_.load(std::memory_order_acquire);
        while (head != nullptr &&
               !head_.compare_exchange_weak(head, head->next,
                                            std::memory_order_acquire,
                                            std::memory_order_relaxed)) {
            // head reloaded by compare_exchange_weak on failure.
        }
        return head;
    }

private:
    struct Node {
        Node* next;
    };
    std::atomic<Node*> head_{nullptr};
};

} // namespace memory
