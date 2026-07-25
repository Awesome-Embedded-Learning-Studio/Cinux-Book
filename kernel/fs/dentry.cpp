/**
 * @file kernel/fs/dentry.cpp
 * @brief DentryCache -- VFS directory-entry cache (F6-M1 B3)
 *
 * Separate-chaining hash (256 buckets) keyed on (parent Inode*, name). Each
 * entry pins its child Inode via inode_ref so the child outlives the cache hit;
 * invalidate() unpins and frees the entry. No LRU shrink: entries accumulate
 * over the boot (a bounded cache is a follow-up).
 */

#include "kernel/fs/dentry.hpp"

#include "kernel/fs/file.hpp"     // inode_ref / inode_unref
#include "kernel/proc/sync.hpp"   // Spinlock

namespace cinux::fs {

namespace {

struct Dentry {
    const Inode* parent;
    uint32_t     namelen;
    char         name[256];
    Inode*       inode;  // pinned via inode_ref while cached
    Dentry*      next;   // hash-chain link
};

constexpr uint32_t         kHashBuckets = 256;
Dentry*                    g_buckets[kHashBuckets]{};
uint32_t                   g_count{0};
cinux::proc::Spinlock      g_lock;

uint32_t hash_key(const Inode* parent, const char* name, uint32_t namelen) {
    uintptr_t h = reinterpret_cast<uintptr_t>(parent);
    for (uint32_t i = 0; i < namelen; ++i) {
        h = h * 131u + static_cast<uint8_t>(name[i]);
    }
    return static_cast<uint32_t>(h % kHashBuckets);
}

bool name_eq(const char* a, uint32_t alen, const char* b, uint32_t blen) {
    if (alen != blen) {
        return false;
    }
    for (uint32_t i = 0; i < alen; ++i) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

}  // namespace

Inode* DentryCache::lookup(const Inode* parent, const char* name, uint32_t namelen) {
    if (parent == nullptr || name == nullptr || namelen == 0) {
        return nullptr;
    }
    auto     g = g_lock.guard();
    uint32_t b = hash_key(parent, name, namelen);
    for (Dentry* d = g_buckets[b]; d != nullptr; d = d->next) {
        if (d->parent == parent && name_eq(d->name, d->namelen, name, namelen)) {
            inode_ref(d->inode);  // hand the caller its own ref
            return d->inode;
        }
    }
    return nullptr;
}

void DentryCache::add(const Inode* parent, const char* name, uint32_t namelen, Inode* child) {
    if (parent == nullptr || name == nullptr || namelen == 0 || namelen >= 256 || child == nullptr) {
        return;
    }
    auto     g = g_lock.guard();
    uint32_t b = hash_key(parent, name, namelen);
    for (Dentry* d = g_buckets[b]; d != nullptr; d = d->next) {
        if (d->parent == parent && name_eq(d->name, d->namelen, name, namelen)) {
            if (d->inode != child) {
                inode_unref(d->inode);
                d->inode = child;
                inode_ref(d->inode);
            }
            return;
        }
    }
    Dentry* d = new Dentry{parent, namelen, {}, child, g_buckets[b]};
    for (uint32_t i = 0; i < namelen; ++i) {
        d->name[i] = name[i];
    }
    d->name[namelen] = '\0';
    g_buckets[b]     = d;
    ++g_count;
    inode_ref(d->inode);  // pin while cached
}

void DentryCache::invalidate(const Inode* parent, const char* name, uint32_t namelen) {
    if (parent == nullptr || name == nullptr || namelen == 0) {
        return;
    }
    auto     g = g_lock.guard();
    uint32_t b = hash_key(parent, name, namelen);
    Dentry** pp = &g_buckets[b];
    while (*pp != nullptr) {
        Dentry* d = *pp;
        if (d->parent == parent && name_eq(d->name, d->namelen, name, namelen)) {
            *pp = d->next;
            inode_unref(d->inode);
            delete d;
            --g_count;
            return;
        }
        pp = &d->next;
    }
}

uint32_t DentryCache::count() {
    auto g = g_lock.guard();
    return g_count;
}

}  // namespace cinux::fs
