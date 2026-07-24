/**
 * @file kernel/fs/inode_ref.hpp
 * @brief InodeRef: RAII handle for a ref'd Inode* from lookup / create
 *
 * FileSystem::lookup(), lookup_child(), and InodeOps::create() return an Inode*
 * with ONE reference already taken -- the Linux inode model: the caller owns
 * that ref and must inode_unref() when done.  InodeRef owns that ref for a
 * scope: construction adopts the ref, destruction drops it, move transfers it.
 *
 * Use it for the short-lived, cross-function ownership pattern at every
 * vfs_lookup caller so ref/unref pairing is enforced by the type system rather
 * than discipline.  The cache-slot aliasing UAF (ld writing a.out into
 * crt1.o's inode) was ultimately a missing unref on a lookup result; an
 * InodeRef makes that category of leak impossible to write.
 *
 * For long-lived holders (File::inode, a VMA's backing inode) keep a raw
 * Inode* with an explicit inode_ref paired to the holder's lifetime -- those
 * outlive any single scope.
 *
 * Namespace: cinux::fs
 */

#pragma once

#include "kernel/fs/file.hpp"  // inode_ref / inode_unref
#include "kernel/fs/inode.hpp"

namespace cinux::fs {

class InodeRef {
public:
    InodeRef() = default;

    /// Adopt an already-ref'd inode (takes ownership of the caller's ref).
    /// Pass the result of lookup / lookup_child / create directly.
    explicit InodeRef(Inode* inode) : inode_(inode) {}

    ~InodeRef() { inode_unref(inode_); }

    InodeRef(const InodeRef&)            = delete;
    InodeRef& operator=(const InodeRef&) = delete;

    InodeRef(InodeRef&& other) noexcept : inode_(other.inode_) { other.inode_ = nullptr; }

    InodeRef& operator=(InodeRef&& other) noexcept {
        if (this != &other) {
            inode_unref(inode_);
            inode_       = other.inode_;
            other.inode_ = nullptr;
        }
        return *this;
    }

    /// Borrow the raw pointer (the InodeRef still owns the ref).
    Inode* get() const { return inode_; }

    /// Transfer ownership out: the caller becomes responsible for inode_unref.
    Inode* release() {
        Inode* i = inode_;
        inode_   = nullptr;
        return i;
    }

    Inode& operator*() const { return *inode_; }
    Inode* operator->() const { return inode_; }

    explicit operator bool() const { return inode_ != nullptr; }

private:
    Inode* inode_{nullptr};
};

}  // namespace cinux::fs
