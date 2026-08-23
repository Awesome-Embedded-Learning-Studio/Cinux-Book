/**
 * @file intrusive_list.hpp
 * @brief Intrusive doubly-linked list — zero-allocation, link pointers embedded in elements.
 */

#ifndef CINUX_INTRUSIVE_LIST_HPP
#define CINUX_INTRUSIVE_LIST_HPP

#include <cstddef>

namespace cinux::lib {

/**
 * @brief Link node to embed in your struct.
 *
 * @tparam T  The containing type.
 *
 * Usage:
 * @code
 *   struct VMA {
 *       uint64_t start, end;
 *       IntrusiveListNode<VMA> list_node;
 *   };
 * @endcode
 */
template <typename T>
struct IntrusiveListNode {
    T* prev = nullptr;
    T* next = nullptr;
};

/**
 * @brief Default traits — assumes member named `list_node`.
 */
template <typename T>
struct IntrusiveListTraits {
    static constexpr IntrusiveListNode<T>& node(T& obj) { return obj.list_node; }
};

/**
 * @brief Intrusive doubly-linked list.
 *
 * @tparam T       Element type.
 * @tparam Traits  Traits type mapping T& -> IntrusiveListNode<T>&.
 *
 * Zero allocation — link pointers live inside the elements.
 */
template <typename T, typename Traits = IntrusiveListTraits<T>>
class IntrusiveList {
public:
    constexpr IntrusiveList() = default;

    constexpr bool   empty() const { return size_ == 0; }
    constexpr size_t size() const { return size_; }
    constexpr T*     front() { return head_; }
    constexpr T*     back() { return tail_; }

    // ============================================================
    // Insertion
    // ============================================================

    constexpr void push_front(T& obj) {
        auto& n = Traits::node(obj);
        n.prev  = nullptr;
        n.next  = head_;
        if (head_) {
            Traits::node(*head_).prev = &obj;
        } else {
            tail_ = &obj;
        }
        head_ = &obj;
        ++size_;
    }

    constexpr void push_back(T& obj) {
        auto& n = Traits::node(obj);
        n.prev  = tail_;
        n.next  = nullptr;
        if (tail_) {
            Traits::node(*tail_).next = &obj;
        } else {
            head_ = &obj;
        }
        tail_ = &obj;
        ++size_;
    }

    constexpr void insert_before(T& before, T& obj) {
        auto& bn = Traits::node(before);
        auto& on = Traits::node(obj);

        on.next = &before;
        on.prev = bn.prev;

        if (bn.prev) {
            Traits::node(*bn.prev).next = &obj;
        } else {
            head_ = &obj;
        }
        bn.prev = &obj;
        ++size_;
    }

    constexpr void insert_after(T& after, T& obj) {
        auto& an = Traits::node(after);
        auto& on = Traits::node(obj);

        on.prev = &after;
        on.next = an.next;

        if (an.next) {
            Traits::node(*an.next).prev = &obj;
        } else {
            tail_ = &obj;
        }
        an.next = &obj;
        ++size_;
    }

    // ============================================================
    // Removal
    // ============================================================

    constexpr void remove(T& obj) {
        auto& n = Traits::node(obj);

        if (n.prev) {
            Traits::node(*n.prev).next = n.next;
        } else {
            head_ = n.next;
        }

        if (n.next) {
            Traits::node(*n.next).prev = n.prev;
        } else {
            tail_ = n.prev;
        }

        n.prev = nullptr;
        n.next = nullptr;
        --size_;
    }

    constexpr T* pop_front() {
        if (!head_) {
            return nullptr;
        }
        T* obj = head_;
        remove(*obj);
        return obj;
    }

    constexpr T* pop_back() {
        if (!tail_) {
            return nullptr;
        }
        T* obj = tail_;
        remove(*obj);
        return obj;
    }

    constexpr void clear() {
        head_ = nullptr;
        tail_ = nullptr;
        size_ = 0;
    }

    // ============================================================
    // Iteration (T* serves as iterator)
    // ============================================================

    constexpr T* begin() { return head_; }
    constexpr T* end() { return nullptr; }

private:
    T*     head_ = nullptr;
    T*     tail_ = nullptr;
    size_t size_ = 0;
};

}  // namespace cinux::lib

#endif  // CINUX_INTRUSIVE_LIST_HPP
