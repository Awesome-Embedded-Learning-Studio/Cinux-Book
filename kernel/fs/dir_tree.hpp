/**
 * @file    dir_tree.hpp
 * @brief   The named tree every file-system backend climbs.
 *
 * One structural half of a backend node, as a CRTP base in the shape
 * of SelfNode one floor down in base: the name, the type, the inode
 * number, and the sibling/child links. Navigation over those links
 * lives here too — find a child by name, adopt one at the front,
 * disown one by name, hand out children by ordinal, walk a path one
 * component at a time. The payload half stays with the backend: ramfs
 * hangs its byte buffer off the derived node, ext2 hangs its
 * on-disk references there instead, and neither re-implements this
 * climb.
 *
 * @author  Charliechen114514
 * @date    2026-10-07
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_fs
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <string_view>

#include "cinux/memory.hpp"
#include "cinux/path.hpp"
#include "cinux/result.hpp"
#include "kernel/fs/fs_config.hpp"
#include "kernel/fs/vfs.hpp"

namespace cinux::fs {

/**
 * @brief   The structural half of a backend node: name, links, identity.
 * @tparam  Node   The backend's full node type (CRTP).
 * @note    Children live on a sibling chain, newest first. The derived
 *          type adds its payload; the tree never asks what that is.
 * @since   0.1.0
 * @ingroup kernel_fs
 */
template <typename Node>
// The implicit public default constructor is deliberate: backends build
// nodes as aggregates and value-initialize them on the heap.
// NOLINTNEXTLINE(bugprone-crtp-constructor-accessibility)
struct DirNode {
    char          name[kFsNameMax] = {};
    InodeType     type             = InodeType::kFile;
    unsigned long ino              = 0;
    Node*         first_child      = nullptr;
    Node*         next_sibling     = nullptr;

    /**
     * @brief         Whether this node's name is exactly the given one.
     *
     * @param[in]     wanted   The name to compare against.
     * @return        True when both the bytes and the length agree.
     * @since         0.1.0
     * @ingroup       kernel_fs
     */
    [[nodiscard]] bool named(std::string_view wanted) const {
        return wanted.size() < kFsNameMax && base::EqualBytes(name, wanted.data(), wanted.size()) &&
               name[wanted.size()] == '\0';
    }

    /**
     * @brief         Finds a direct child by name.
     *
     * @param[in]     wanted   The child's name.
     * @return        The child, or nullptr when this directory holds no
     *                such name.
     * @since         0.1.0
     * @ingroup       kernel_fs
     */
    [[nodiscard]] Node* find(std::string_view wanted) {
        for (Node* child = first_child; child != nullptr; child = child->next_sibling) {
            if (child->named(wanted)) {
                return child;
            }
        }
        return nullptr;
    }

    /**
     * @brief         Takes a freshly built node as the newest child.
     *
     * @param[in,out] child   The node to adopt; the caller has already
     *                        filled its name.
     * @return        None
     * @since         0.1.0
     * @ingroup       kernel_fs
     */
    void adopt(Node& child) {
        child.next_sibling = first_child;
        first_child        = &child;
    }

    /**
     * @brief         Unlinks one child by name, without destroying it.
     *
     * @param[in]     wanted   The child's name.
     * @return        The detached child, ready for the caller's delete;
     *                nullptr when no child carries that name.
     * @since         0.1.0
     * @ingroup       kernel_fs
     */
    [[nodiscard]] Node* disown(std::string_view wanted) {
        Node* previous = nullptr;
        Node* scan     = first_child;
        while (scan != nullptr && !scan->named(wanted)) {
            previous = scan;
            scan     = scan->next_sibling;
        }
        if (scan == nullptr) {
            return nullptr;
        }
        if (previous == nullptr) {
            first_child = scan->next_sibling;
        } else {
            previous->next_sibling = scan->next_sibling;
        }
        scan->next_sibling = nullptr;
        return scan;
    }

    /**
     * @brief         Hands out a child by ordinal, newest first.
     *
     * @param[in]     index   Which child, zero-based.
     * @return        The index-th child, or nullptr past the last.
     * @since         0.1.0
     * @ingroup       kernel_fs
     */
    [[nodiscard]] Node* child_at(unsigned long index) {
        Node* child = first_child;
        for (unsigned long step = 0; step < index && child != nullptr; ++step) {
            child = child->next_sibling;
        }
        return child;
    }
};

/**
 * @brief         Walks a mount-relative path down a named tree.
 *
 * @param[in,out] root   The tree's root directory.
 * @param[in]     path   Path without leading slash; empty addresses the
 *                      root itself.
 * @return        The node the path names, or kNotFound (no such name),
 *                kNotADirectory (a file sits mid-path).
 * @tparam        Node   The backend's node type, deriving DirNode<Node>.
 * @since         0.1.0
 * @ingroup       kernel_fs
 */
template <typename Node>
[[nodiscard]] base::Result<Node*> WalkPath(Node& root, std::string_view path) {
    Node*            current = &root;
    base::PathCursor walk{path};
    std::string_view component;
    while (base::NextComponent(walk, component)) {
        if (current->type != InodeType::kDirectory) {
            return base::KernelError::kNotADirectory;
        }
        current = current->find(component);
        if (current == nullptr) {
            return base::KernelError::kNotFound;
        }
    }
    return current;
}

}  // namespace cinux::fs
