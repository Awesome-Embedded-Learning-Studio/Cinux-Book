/**
 * @file    self_list.hpp
 * @brief   An intrusive singly-linked list: the node rides the object.
 *
 * Borrowed from ZerOS's self_list.hpp and slimmed to the FIFO face
 * this kernel needs today: append, prepend, pop head, unlink. The
 * intrusive shape is the point — a Task (or any wait-queue member)
 * carries its own link, so parking on a queue costs no allocation
 * and the object cannot be on two lists at once, which is exactly
 * the invariant a scheduler's wait queues want. The link is private
 * and only the list walks it: a stray hand wiring nodes behind the
 * list's back is how ready queues die quietly. The sorted-insert
 * companion (a Less comparator, deadline queues) stays in ZerOS
 * until a consumer here asks for it.
 *
 * @author  Charliechen114514
 * @date    2026-10-07
 * @version 0.1
 * @since   0.1.0
 * @ingroup base_container
 * @copyright Copyright (c) 2026
 */

#pragma once


namespace cinux::base::container {

/**
 * @brief         The link a type carries to ride a SelfList.
 * @tparam        Stuff   The type embedding this node (CRTP).
 * @note          Private next with the list as its only friend: the
 *                owning type cannot even see its own link, let alone
 *                rewire it.
 * @since         0.1.0
 * @ingroup       base_container
 */
template <typename Stuff>
// The implicit public default constructor is deliberate: hosts and the
// kernel build link targets as aggregates, which needs it reachable.
// NOLINTNEXTLINE(bugprone-crtp-constructor-accessibility)
struct SelfNode {
private:
    template <typename, auto>
    friend struct SelfList;

    Stuff* next_ = nullptr;
};

/**
 * @brief         FIFO intrusive list over SelfNode-carrying objects.
 * @tparam        Stuff   The linked type; must embed SelfNode<Stuff>.
 * @tparam        member  Optional member-link selector: pass
 *                        &Stuff::link_field to ride a member node
 *                        instead of the base-class one, so one object
 *                        can sit on several lists at once — the base
 *                        link and each member link are separate wires.
 * @note          The sharing hazard this solves: two lists both riding
 *                the base link of the same object would corrupt each
 *                other, because one link field can hold one successor
 *                only.
 * @note          Head plus tail pointers, so append is constant time
 *                and pop is constant time. Trivially destructible
 *                and constant-initializable: safe inside freestanding
 *                singletons. Removal walks (singly-linked), which the
 *                wait queues tolerate because their usual exit is
 *                pop-the-head.
 * @since         0.1.0
 * @ingroup       base_container
 */
template <typename, auto>
struct MemberNode;

template <typename Stuff, SelfNode<Stuff>(Stuff::* LinkField)>
struct MemberNode<Stuff, LinkField> {
    /// Reads the link field the list rides.
    static SelfNode<Stuff>& node_of(Stuff& item) { return item.*LinkField; }
};

template <typename Stuff>
struct MemberNode<Stuff, nullptr> {
    /// Reads the base-class link, the default ride.
    static SelfNode<Stuff>& node_of(Stuff& item) { return item; }
};

template <typename Stuff, auto LinkField = nullptr>
struct SelfList {
    using NodeOf = MemberNode<Stuff, LinkField>;

    SelfList() = default;

    SelfList(const SelfList&)            = delete;
    SelfList& operator=(const SelfList&) = delete;

    /**
     * @brief         The first linked object, or nullptr when empty.
     * @return        Head pointer for inspection, not for unlinking.
     * @since         0.1.0
     * @ingroup       base_container
     */
    [[nodiscard]] Stuff* head() const { return stuff_; }

    /**
     * @brief         Append at the tail; arrival order is pop order.
     * @param[in,out] item   Object to link; must not be linked here
     *                      already.
     * @return        None.
     * @since         0.1.0
     * @ingroup       base_container
     */
    void push_back(Stuff& item) {
        NodeOf::node_of(item).next_ = nullptr;
        if (tail_ == nullptr) {
            stuff_ = &item;
        } else {
            NodeOf::node_of(*tail_).next_ = &item;
        }
        tail_ = &item;
    }

    /**
     * @brief         Prepend at the head; jumps the arrival order.
     * @param[in,out] item   Object to link at the front.
     * @return        None.
     * @since         0.1.0
     * @ingroup       base_container
     */
    void push_front(Stuff& item) {
        NodeOf::node_of(item).next_ = stuff_;
        stuff_                      = &item;
        if (tail_ == nullptr) {
            tail_ = &item;
        }
    }

    /**
     * @brief         Detach and return the first object.
     * @return        The former head, detached and ready to link
     *                elsewhere, or nullptr when empty.
     * @since         0.1.0
     * @ingroup       base_container
     */
    [[nodiscard]] Stuff* pop_head() {
        Stuff* out = stuff_;
        if (out != nullptr) {
            stuff_ = NodeOf::node_of(*out).next_;
            if (stuff_ == nullptr) {
                tail_ = nullptr;
            }
            NodeOf::node_of(*out).next_ = nullptr;
        }
        return out;
    }

    /**
     * @brief         Unlink one object from anywhere in the list.
     * @param[in,out] item   The object to remove.
     * @return        True when it was found and unlinked; false when
     *                it was never linked here (or already removed).
     * @since         0.1.0
     * @ingroup       base_container
     */
    bool remove(Stuff& item) {
        Stuff* prev = nullptr;
        Stuff* scan = stuff_;
        while (scan != nullptr) {
            if (scan == &item) {
                if (prev == nullptr) {
                    stuff_ = NodeOf::node_of(*scan).next_;
                } else {
                    NodeOf::node_of(*prev).next_ = NodeOf::node_of(*scan).next_;
                }
                // NOLINTNEXTLINE(bugprone-branch-clone)
                if (tail_ == &item) {
                    tail_ = prev;
                }
                NodeOf::node_of(item).next_ = nullptr;
                return true;
            }
            prev = scan;
            scan = NodeOf::node_of(*scan).next_;
        }
        return false;
    }

    /**
     * @brief         Whether anything is linked.
     * @return        True when the list holds no object.
     * @since         0.1.0
     * @ingroup       base_container
     */
    [[nodiscard]] bool empty() const { return stuff_ == nullptr; }

private:
    Stuff* stuff_ = nullptr;
    Stuff* tail_  = nullptr;
};

}  // namespace cinux::base::container