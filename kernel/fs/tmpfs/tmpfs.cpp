/**
 * @file kernel/fs/tmpfs/tmpfs.cpp
 * @brief TmpFs implementation: TmpNode tree, InodeOps, and backend.
 */

#include "tmpfs.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/fs/file.hpp"     // inode_ref (lookup returns a ref'd Inode*)
#include "kernel/lib/string.hpp"  // memcpy / memset / strlen

namespace cinux::fs {

using cinux::lib::Error;
using cinux::lib::ErrorOr;

struct TmpNode {
    Inode    inode;                ///< Embedded; ops + fs_private set by TmpFs
    TmpFs*   fs;                   ///< Owning filesystem (lock + ino allocator)
    TmpNode* parent;               ///< Parent directory (nullptr for the root)
    TmpNode* next_sibling;         ///< Next entry in parent->first_child chain
    TmpNode* first_child;          ///< (dirs) head of the child list
    char     name[kTmpfsNameMax];  ///< NUL-terminated entry name
    uint8_t* data;                 ///< (files) heap byte buffer
    uint64_t capacity;             ///< (files) allocated bytes in data
    uint64_t size;                 ///< (files) logical content length in bytes
};

namespace {

/// Round @p need up to a whole number of kTmpfsGrowthAlign units (at least one
/// unit), so a stream of small writes into one file reallocates O(log) times.
uint64_t round_up_capacity(uint64_t need) {
    constexpr uint64_t kAlign = kTmpfsGrowthAlign;
    if (need == 0) {
        return kAlign;
    }
    return ((need + kAlign - 1) / kAlign) * kAlign;
}

/// Does @p entry's NUL-terminated name equal @p name[0 .. namelen)?
bool name_matches(const TmpNode* entry, const char* name, uint32_t namelen) {
    for (uint32_t i = 0; i < namelen; ++i) {
        if (entry->name[i] == '\0' || entry->name[i] != name[i]) {
            return false;
        }
    }
    return entry->name[namelen] == '\0';  // entry is not longer than namelen
}

/// Walk @p dir's child list for an entry matching @p name[0 .. namelen).
/// Returns nullptr when no child matches.
TmpNode* find_child(const TmpNode* dir, const char* name, uint32_t namelen) {
    for (TmpNode* c = dir->first_child; c != nullptr; c = c->next_sibling) {
        if (name_matches(c, name, namelen)) {
            return c;
        }
    }
    return nullptr;
}

/// Fill a struct stat for a regular-file TmpNode.  Zeroed first so no
/// kernel-stack bytes leak through the Linux-ABI padding / _nsec fields.
void fill_reg_stat(const TmpNode* node, struct stat* st) {
    memset(st, 0, sizeof(*st));
    st->st_ino     = node->inode.ino;
    st->st_nlink   = 1;
    st->st_mode    = node->inode.mode;
    st->st_size    = node->size;
    st->st_blksize = 4096;
    st->st_blocks  = (node->size + 511) / 512;
}

/// Fill a struct stat for a directory TmpNode.
void fill_dir_stat(const TmpNode* node, struct stat* st) {
    memset(st, 0, sizeof(*st));
    st->st_ino     = node->inode.ino;
    st->st_nlink   = 2;
    st->st_mode    = node->inode.mode;
    st->st_blksize = 4096;
}

/// Emit the "." (index 0) or ".." (index 1) readdir entry into @p name.
/// Returns 1 on a match, InvalidArgument when @p name_max is too small, or
/// leaves index >= 2 to the caller by returning 0.
ErrorOr<int64_t> fill_dot_entry(uint64_t index, char* name, uint64_t name_max) {
    if (index == 0) {
        if (name_max < 2) {
            return Error::InvalidArgument;
        }
        name[0] = '.';
        name[1] = '\0';
        return 1;
    }
    if (index == 1) {
        if (name_max < 3) {
            return Error::InvalidArgument;
        }
        name[0] = '.';
        name[1] = '.';
        name[2] = '\0';
        return 1;
    }
    return 0;
}

class TmpFileOps : public InodeOps {
public:
    ErrorOr<int64_t> read(const Inode* inode, uint64_t offset, void* buf, uint64_t count) override {
        if (inode == nullptr || buf == nullptr) {
            return Error::InvalidArgument;
        }
        auto* node = static_cast<TmpNode*>(inode->fs_private);
        auto  g    = node->fs->lock_.guard();
        if (count == 0 || offset >= node->size) {
            return 0;  // nothing requested, or past EOF
        }
        uint64_t avail = node->size - offset;
        uint64_t n     = count < avail ? count : avail;
        memcpy(buf, node->data + offset, n);
        return static_cast<int64_t>(n);
    }

    ErrorOr<int64_t> write(Inode* inode, uint64_t offset, const void* buf,
                           uint64_t count) override {
        if (inode == nullptr || buf == nullptr) {
            return Error::InvalidArgument;
        }
        auto* node = static_cast<TmpNode*>(inode->fs_private);
        auto  g    = node->fs->lock_.guard();
        if (count == 0) {
            return 0;
        }
        uint64_t need = offset + count;
        if (need > node->capacity) {
            // Grow: allocate a fresh page-aligned buffer, copy the live prefix,
            // zero-fill any gap between the old EOF and the write offset, then
            // release the old buffer.
            uint64_t newcap = round_up_capacity(need);
            uint8_t* nd     = new uint8_t[newcap];
            if (node->size > 0) {
                memcpy(nd, node->data, node->size);
            }
            if (offset > node->size) {
                memset(nd + node->size, 0, offset - node->size);
            }
            delete[] node->data;
            node->data     = nd;
            node->capacity = newcap;
        } else if (offset > node->size) {
            // Gap inside the existing buffer: zero it so the file is not left
            // with stale heap bytes between the old EOF and the write.
            memset(node->data + node->size, 0, offset - node->size);
        }
        memcpy(node->data + offset, buf, count);
        if (need > node->size) {
            node->size = need;
        }
        node->inode.size = node->size;
        return static_cast<int64_t>(count);
    }

    ErrorOr<void> stat(const Inode* inode, struct stat* st) override {
        if (inode == nullptr || st == nullptr) {
            return Error::InvalidArgument;
        }
        auto* node = static_cast<TmpNode*>(inode->fs_private);
        auto  g    = node->fs->lock_.guard();
        fill_reg_stat(node, st);
        return {};
    }

    ErrorOr<void> truncate(Inode* inode, uint64_t length) override {
        if (inode == nullptr) {
            return Error::InvalidArgument;
        }
        auto* node = static_cast<TmpNode*>(inode->fs_private);
        auto  g    = node->fs->lock_.guard();
        (void)g;

        if (length > node->capacity) {
            uint64_t newcap = round_up_capacity(length);
            uint8_t* nd     = new uint8_t[newcap];
            if (node->size > 0) {
                memcpy(nd, node->data, node->size);
            }
            memset(nd + node->size, 0, length - node->size);
            delete[] node->data;
            node->data     = nd;
            node->capacity = newcap;
        } else if (length > node->size) {
            memset(node->data + node->size, 0, length - node->size);
        }

        node->size       = length;
        node->inode.size = length;
        return {};
    }

    ErrorOr<void> chmod(Inode* inode, uint32_t mode) override {
        if (inode == nullptr) {
            return Error::InvalidArgument;
        }
        auto* node = static_cast<TmpNode*>(inode->fs_private);
        auto  g    = node->fs->lock_.guard();
        (void)g;
        node->inode.mode = kTmpfsSIfReg | (mode & 07777);
        return {};
    }
};

class TmpDirOps : public InodeOps {
public:
    ErrorOr<int64_t> readdir(const Inode* inode, uint64_t index, char* name,
                             uint64_t name_max) override {
        if (inode == nullptr || name == nullptr || name_max == 0) {
            return Error::InvalidArgument;
        }
        auto* dir = static_cast<TmpNode*>(inode->fs_private);
        auto  g   = dir->fs->lock_.guard();

        auto dot = fill_dot_entry(index, name, name_max);
        if (!dot.ok() || dot.value() == 1) {
            return dot;
        }

        // index >= 2 -> the (index-2)-th child in the sibling list.
        uint32_t skip = static_cast<uint32_t>(index - 2);
        TmpNode* c    = dir->first_child;
        while (skip > 0 && c != nullptr) {
            c = c->next_sibling;
            --skip;
        }
        if (c == nullptr) {
            return 0;  // past the last entry
        }
        uint32_t i = 0;
        while (i + 1 < name_max && c->name[i] != '\0') {
            name[i] = c->name[i];
            ++i;
        }
        name[i] = '\0';
        return 1;
    }

    ErrorOr<Inode*> create(Inode* dir_inode, const char* name, uint32_t namelen) override {
        return make_node(dir_inode, name, namelen, InodeType::Regular);
    }

    ErrorOr<Inode*> mkdir(Inode* dir_inode, const char* name, uint32_t namelen) override {
        return make_node(dir_inode, name, namelen, InodeType::Directory);
    }

    ErrorOr<void> unlink(Inode* dir_inode, const char* name, uint32_t namelen) override {
        if (dir_inode == nullptr || name == nullptr || namelen == 0) {
            return Error::InvalidArgument;
        }
        auto* dir = static_cast<TmpNode*>(dir_inode->fs_private);
        auto  g   = dir->fs->lock_.guard();

        // Walk the child list keeping a prev pointer so we can splice the
        // matching entry out.
        TmpNode* prev = nullptr;
        TmpNode* c    = dir->first_child;
        while (c != nullptr && !name_matches(c, name, namelen)) {
            prev = c;
            c    = c->next_sibling;
        }
        if (c == nullptr) {
            return Error::NotFound;
        }
        // Removing a non-empty directory is rejected.  The Error enum has no
        // DirectoryNotEmpty, so this maps to EIO at the syscall boundary; the
        // contract (the op simply fails) is what callers and tests rely on.
        if (c->inode.type == InodeType::Directory && c->first_child != nullptr) {
            return Error::IOError;
        }
        if (prev == nullptr) {
            dir->first_child = c->next_sibling;
        } else {
            prev->next_sibling = c->next_sibling;
        }
        delete[] c->data;  // delete[] nullptr is a safe no-op
        delete c;
        return {};
    }

    ErrorOr<void> stat(const Inode* inode, struct stat* st) override {
        if (inode == nullptr || st == nullptr) {
            return Error::InvalidArgument;
        }
        auto* node = static_cast<TmpNode*>(inode->fs_private);
        auto  g    = node->fs->lock_.guard();
        fill_dir_stat(node, st);
        return {};
    }

    ErrorOr<void> chmod(Inode* inode, uint32_t mode) override {
        if (inode == nullptr) {
            return Error::InvalidArgument;
        }
        auto* node = static_cast<TmpNode*>(inode->fs_private);
        auto  g    = node->fs->lock_.guard();
        (void)g;
        node->inode.mode = kTmpfsSIfDir | (mode & 07777);
        return {};
    }

private:
    /// Shared body of create() / mkdir(): validate the name, reject a duplicate,
    /// allocate + link a new TmpNode of @p type, and return its Inode.
    ErrorOr<Inode*> make_node(Inode* dir_inode, const char* name, uint32_t namelen,
                              InodeType type) {
        if (dir_inode == nullptr || name == nullptr || namelen == 0) {
            return Error::InvalidArgument;
        }
        if (namelen + 1 > kTmpfsNameMax) {
            return Error::InvalidArgument;  // name too long
        }
        auto* dir = static_cast<TmpNode*>(dir_inode->fs_private);
        auto  g   = dir->fs->lock_.guard();

        if (find_child(dir, name, namelen) != nullptr) {
            return Error::AlreadyExists;
        }

        auto* node = new TmpNode{};  // value-initialised: pointers null, sizes 0
        memcpy(node->name, name, namelen);
        node->name[namelen] = '\0';
        node->fs            = dir->fs;
        node->parent        = dir;
        node->inode.ino     = dir->fs->alloc_ino();
        node->inode.type    = type;
        node->inode.ops = (type == InodeType::Directory) ? dir->fs->dir_ops() : dir->fs->file_ops();
        node->inode.fs_private = node;
        node->inode.mode =
            (type == InodeType::Directory) ? (kTmpfsSIfDir | 0755) : (kTmpfsSIfReg | 0644);
        node->inode.nlink = (type == InodeType::Directory) ? 2 : 1;

        // Prepend to the parent's child list.
        node->next_sibling = dir->first_child;
        dir->first_child   = node;
        return &node->inode;
    }
};

/// Recursively free @p node and its descendants (depth-first).  Used by the
/// TmpFs destructor so a stack-local TmpFs (unit tests) leaks nothing.
void free_tree(TmpNode* node) {
    if (node == nullptr) {
        return;
    }
    TmpNode* c = node->first_child;
    while (c != nullptr) {
        TmpNode* next = c->next_sibling;
        free_tree(c);
        c = next;
    }
    delete[] node->data;  // delete[] nullptr is safe
    delete node;
}

}  // anonymous namespace

TmpFs::TmpFs() = default;

TmpFs::~TmpFs() {
    free_tree(root_);
    delete file_ops_;
    delete dir_ops_;
}

ErrorOr<void> TmpFs::mount() {
    // Idempotent: a second mount() is a no-op (mirrors DevFs / ProcFs).
    if (mounted_) {
        return {};
    }

    file_ops_ = new TmpFileOps();
    dir_ops_  = new TmpDirOps();

    root_               = new TmpNode{};
    root_->fs           = this;
    root_->parent       = nullptr;
    root_->next_sibling = nullptr;
    root_->first_child  = nullptr;
    root_->data         = nullptr;
    root_->capacity     = 0;
    root_->size         = 0;
    root_->name[0]      = '\0';

    root_->inode.ino        = 1;
    root_->inode.type       = InodeType::Directory;
    root_->inode.ops        = dir_ops_;
    root_->inode.fs_private = root_;
    root_->inode.mode       = kTmpfsSIfDir | 0755;
    root_->inode.nlink      = 2;

    mounted_ = true;
    return {};
}

ErrorOr<Inode*> TmpFs::lookup(const char* path) {
    if (path == nullptr) {
        return Error::InvalidArgument;
    }
    auto g = lock_.guard();

    if (root_ == nullptr) {
        return Error::IOError;  // not mounted
    }

    // Root directory: empty path or "/".
    if (path[0] == '\0' || (path[0] == '/' && path[1] == '\0')) {
        inode_ref(&root_->inode);
        return &root_->inode;
    }
    const char* p = path;
    if (p[0] == '/') {
        ++p;
    }

    // Walk component by component.  Each intermediate component must resolve to
    // a directory; a missing component or a non-directory intermediate yields
    // NotFound (mirrors ext2::lookup's contract).
    TmpNode* cur = root_;
    while (p[0] != '\0') {
        uint32_t comp_len = 0;
        while (p[comp_len] != '\0' && p[comp_len] != '/') {
            ++comp_len;
        }
        if (comp_len == 0) {
            ++p;  // collapse a duplicate slash
            continue;
        }
        if (cur->inode.type != InodeType::Directory) {
            return Error::NotFound;  // cannot descend through a file
        }
        TmpNode* child = find_child(cur, p, comp_len);
        if (child == nullptr) {
            return Error::NotFound;
        }
        cur = child;
        p += comp_len;
        if (p[0] == '/') {
            ++p;
        }
    }
    inode_ref(&cur->inode);
    return &cur->inode;
}

// F-USABILITY batch 1a: single-component lookup for the vfs_lookup layer.
// Recovers the parent TmpNode via fs_private, then delegates to find_child
// (the same primitive lookup() walks with) under the fs lock.
ErrorOr<Inode*> TmpFs::lookup_child(const Inode* parent, const char* name, uint32_t namelen) {
    if (parent == nullptr || name == nullptr || namelen == 0) {
        return Error::InvalidArgument;
    }
    if (parent->type != InodeType::Directory || parent->fs_private == nullptr) {
        return Error::NotFound;
    }
    auto g = lock_.guard();
    (void)g;
    TmpNode* parent_node = static_cast<TmpNode*>(parent->fs_private);
    TmpNode* child       = find_child(parent_node, name, namelen);
    if (child == nullptr) {
        return Error::NotFound;
    }
    inode_ref(&child->inode);
    return &child->inode;
}

}  // namespace cinux::fs
