/**
 * @file kernel/fs/vfs_lookup.cpp
 * @brief vfs_lookup: component walk + symlink follow + loop detection
 *
 * F-USABILITY batch 1b. The resolver keeps an absolute canonical "remaining
 * path" buffer and restarts resolution whenever a symlink is followed: the link
 * target is spliced in (an absolute target replaces from the root; a relative
 * target is appended to the link's parent-directory path), the buffer is
 * re-canonicalised, and resolution restarts -- which naturally handles a
 * symlink target that crosses a mount point. A depth counter caps total symlink
 * follows at 40 (Linux MAXSYMLINKS) to catch cycles.
 */

#include "kernel/fs/vfs_lookup.hpp"

#include <cstring>  // memcpy, strlen

#include "kernel/fs/dentry.hpp"  // DentryCache (F6-M1 B3)
#include "kernel/fs/file.hpp"   // inode_ref / inode_unref (refcount transfer)
#include "kernel/fs/path.hpp"
#include "kernel/fs/vfs_filesystem.hpp"
#include "kernel/fs/vfs_mount.hpp"

namespace cinux::fs {

namespace {

constexpr uint32_t kMaxSymlinks = 40;  // Linux MAXSYMLINKS

inline uint32_t u32len(const char* s) { return static_cast<uint32_t>(strlen(s)); }

// Append ('/' + comp[0..len)) to dst. Returns false on PATH_MAX overflow.
bool append_comp(char* dst, const char* comp, uint32_t len) {
    const uint32_t dlen = u32len(dst);
    if (static_cast<uint64_t>(dlen) + 1 + len + 1 > PATH_MAX) return false;
    dst[dlen] = '/';
    memcpy(dst + dlen + 1, comp, len);
    dst[dlen + 1 + len] = '\0';
    return true;
}

// Append a separator + suffix unless suffix is empty or already slash-led.
// Returns false on overflow. Used to append the unresolved tail after a spliced
// symlink target.
bool append_tail(char* dst, const char* suffix) {
    const uint32_t dlen    = u32len(dst);
    const uint32_t slen    = u32len(suffix);
    if (slen == 0) return true;
    const uint32_t sep = (suffix[0] != '/') ? 1u : 0u;
    if (static_cast<uint64_t>(dlen) + sep + slen + 1 > PATH_MAX) return false;
    if (sep != 0) dst[dlen] = '/';
    memcpy(dst + dlen + sep, suffix, slen);
    dst[dlen + sep + slen] = '\0';
    return true;
}

}  // namespace

cinux::lib::ErrorOr<LookupResult> vfs_lookup(const char* path, uint32_t flags,
                                             const char* cwd) {
    if (path == nullptr || cwd == nullptr) {
        return cinux::lib::Error::InvalidArgument;
    }

    PathBuf resolved;
    if (!path_resolve(cwd, path, resolved)) {
        return cinux::lib::Error::InvalidArgument;
    }

    const bool want_parent = (flags & static_cast<uint32_t>(LookupFlag::Parent)) != 0;
    const bool follow_end  = (flags & static_cast<uint32_t>(LookupFlag::Follow)) != 0;
    const bool nofollow    = (flags & static_cast<uint32_t>(LookupFlag::NoFollow)) != 0;
    const bool want_dir    = (flags & static_cast<uint32_t>(LookupFlag::Directory)) != 0;

    uint32_t depth = 0;

    for (;;) {  // restart after every symlink follow
        const char* rel = nullptr;
        FileSystem* fs  = vfs_resolve(resolved, &rel);
        if (fs == nullptr) {
            return cinux::lib::Error::NotFound;
        }
        const uint32_t mount_prefix_len =
            static_cast<uint32_t>(rel - static_cast<const char*>(resolved));

        auto root_r = fs->lookup("");  // ref'd: lookup returns an inode with one ref
        if (!root_r.ok()) {
            return root_r.error();
        }
        Inode* cur = root_r.value();  // cur owns the root ref until replaced or returned

        // walked_full: absolute path of `cur` (starts at the mount prefix). Used
        // as the base when a relative symlink target is spliced in.
        PathBuf walked_full;
        memcpy(walked_full, static_cast<const char*>(resolved), mount_prefix_len);
        walked_full[mount_prefix_len] = '\0';

        const char* p         = rel;
        bool        restarted = false;

        while (*p != '\0') {
            while (*p == '/') ++p;
            if (*p == '\0') break;

            uint32_t comp_len = 0;
            while (p[comp_len] != '\0' && p[comp_len] != '/') ++comp_len;
            const bool is_last = (p[comp_len] == '\0');

            // PARENT mode: the final component is the leaf name -- do not resolve it.
            if (want_parent && is_last) {
                if (comp_len > kLookupNameMax) {
                    inode_unref(cur);
                    return cinux::lib::Error::InvalidArgument;  // name too long
                }
                LookupResult r;
                r.parent = cur;  // transfer the ref to the caller
                memcpy(r.leaf_name, p, comp_len);  // copy into stable storage
                r.leaf_name[comp_len] = '\0';
                r.leaf_len            = comp_len;
                return r;
            }

            // canonical rel carries no "." / ".."; guard anyway.
            if (comp_len == 1 && p[0] == '.') { p += comp_len; continue; }
            if (comp_len == 2 && p[0] == '.' && p[1] == '.') {
                inode_unref(cur);
                return cinux::lib::Error::NotFound;
            }

            Inode* child = DentryCache::lookup(cur, p, comp_len);  // hit -> ref'd
            if (child == nullptr) {
                auto child_r = fs->lookup_child(cur, p, comp_len);  // ref'd
                if (!child_r.ok()) {
                    inode_unref(cur);
                    return child_r.error();
                }
                child = child_r.value();  // child owns its own ref
                // Skip the DentryCache for filesystems whose lookups are dynamic
                // (DevFs: /dev/tty resolves per-caller).  Caching the first result
                // would make every later opener see the first opener's terminal.
                if (fs->dcache_enabled()) {
                    DentryCache::add(cur, p, comp_len, child);
                }
            }

            // Follow symlink unless it is the trailing component and Follow is
            // off, or NoFollow is set on the trailing component.
            if (child->type == InodeType::Symlink) {
                const bool follow = !is_last || follow_end;
                if (follow && !(is_last && nofollow)) {
                    PathBuf target;
                    auto rl_r = child->ops->readlink(child, target, PATH_MAX - 1);
                    if (!rl_r.ok()) {
                        inode_unref(child);
                        inode_unref(cur);
                        return rl_r.error();
                    }
                    target[static_cast<uint32_t>(rl_r.value())] = '\0';

                    const char* tail = p + comp_len;  // unresolved path after this component
                    PathBuf     next;
                    if (target[0] == '/') {
                        // absolute target: resolve from the root
                        memcpy(next, static_cast<const char*>(target), u32len(target) + 1);
                    } else {
                        // relative target: splice after the link's parent path
                        memcpy(next, static_cast<const char*>(walked_full), u32len(walked_full) + 1);
                        if (!append_comp(next, target, u32len(target))) {
                            inode_unref(child);
                            inode_unref(cur);
                            return cinux::lib::Error::InvalidArgument;
                        }
                    }
                    if (!append_tail(next, tail)) {
                        inode_unref(child);
                        inode_unref(cur);
                        return cinux::lib::Error::InvalidArgument;
                    }
                    path_canonicalize(next);

                    if (++depth > kMaxSymlinks) {
                        inode_unref(child);
                        inode_unref(cur);
                        return cinux::lib::Error::Loop;
                    }
                    memcpy(static_cast<char*>(resolved), static_cast<const char*>(next), u32len(next) + 1);
                    restarted = true;
                    inode_unref(child);  // symlink resolved; child no longer needed
                    break;
                }
            }

            // child is the next component: drop the old cur, adopt child.
            inode_unref(cur);
            cur = child;
            if (!append_comp(walked_full, p, comp_len)) {
                inode_unref(cur);
                return cinux::lib::Error::InvalidArgument;
            }
            p += comp_len;
        }

        if (restarted) {
            inode_unref(cur);  // drop this round's cur; outer loop re-lookups root
            continue;
        }

        if (want_dir && cur->type != InodeType::Directory) {
            inode_unref(cur);
            return cinux::lib::Error::NotADirectory;
        }

        LookupResult r;
        r.target = cur;  // transfer the ref to the caller
        return r;
    }
}

}  // namespace cinux::fs
