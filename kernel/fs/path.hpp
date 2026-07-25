/**
 * @file kernel/fs/path.hpp
 * @brief Path resolution and canonicalization utilities
 *
 * Provides helpers to canonicalise paths (collapse . .. //) and to
 * resolve relative paths against a per-process cwd before passing
 * them to the VFS mount layer.
 *
 * Namespace: cinux::fs
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cinux::fs {

/// Maximum absolute path length (including NUL terminator)
static constexpr uint32_t PATH_MAX = 4096;

/// Heap-allocated PATH_MAX scratch buffer (RAII).  Path resolution runs inside
/// syscall handlers; a `char resolved[PATH_MAX]` (4 KB) on the kernel stack --
/// twice over in handlers that also split a parent path, plus the canonicaliser's
/// own 4 KB scratch -- overflowed the 16 KB kernel stack on the first path
/// syscall (F5-M5 -smp shell).  Linux keeps 8 KB stacks by NOT putting path
/// buffers on them; this does the same: the buffer lives on the heap and is
/// freed at scope exit.  Implicitly converts to `char*` so call sites read like
/// a plain buffer.
class PathBuf {
public:
    PathBuf() : buf_(new char[PATH_MAX]) { buf_[0] = '\0'; }
    ~PathBuf() { delete[] buf_; }
    PathBuf(const PathBuf&)            = delete;
    PathBuf& operator=(const PathBuf&) = delete;

    char&       operator[](uint32_t i) noexcept { return buf_[i]; }
    char*       data() noexcept { return buf_; }
    const char* data() const noexcept { return buf_; }
    operator char*() noexcept { return buf_; }

private:
    char* buf_;
};

/**
 * @brief Canonicalise a path in-place
 *
 * Collapses duplicate slashes, resolves "." and ".." components.
 * The result is an absolute path with no trailing slash (except for "/").
 *
 * @param buf  Mutable buffer containing the path to canonicalise
 */
void path_canonicalize(char* buf);

/**
 * @brief Resolve a (possibly relative) path against a cwd
 *
 * If @p path starts with '/', it is treated as absolute and copied
 * directly to @p out.  Otherwise, @p cwd + "/" + @p path is
 * assembled and then canonicalised.
 *
 * @param cwd   Current working directory (absolute, NUL-terminated)
 * @param path  User-supplied path (may be relative)
 * @param out   Output buffer (at least PATH_MAX bytes)
 * @return true on success, false if the result would overflow
 */
bool path_resolve(const char* cwd, const char* path, char* out);

}  // namespace cinux::fs
