/**
 * @file    path.hpp
 * @brief   Slash-separated path views: walk, split, fold onto a base.
 *
 * Paths travel as string_view. The two view shapes — a cursor handing
 * out one component at a time (tolerating repeated slashes) and a leaf
 * split for the parent-plus-name question — live here as pure inline
 * arithmetic. The fold that lays a possibly-relative path onto an
 * absolute base keeps a mutable cursor of its own, so its bytes live in
 * the library build; the header carries the face.
 *
 * @author  Charliechen114514
 * @date    2026-10-11
 * @version 0.3
 * @since   0.1.0
 * @ingroup base_path
 * @copyright Copyright (c) 2026
 */

#pragma once

// misc-include-cleaner misjudges this include under the fallback compile
// a lone header gets; every declaration below spells std::string_view.
// NOLINTNEXTLINE(misc-include-cleaner)
#include <string_view>

namespace cinux::base {

/**
 * @brief   Cursor state over the components of a slash-separated path.
 * @note    Views only — the cursor walks the caller's bytes and hands
 *          out slices. Repeated slashes are tolerated: "a//b" yields
 *          "a" then "b", and a path of only slashes yields nothing.
 *          Seed it as PathCursor{path} and drive it with NextComponent.
 * @since   0.1.0
 * @ingroup base_path
 */
// The two member-init checks disagree with each other under the fallback
// compile a lone header gets: with a default member initializer the field
// is "redundant", without one it is "uninitialized". The aggregate shape
// is the point; silence the pair here.
// NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
struct PathCursor {
    // NOLINTNEXTLINE(readability-redundant-member-init)
    std::string_view rest = {};  ///< What has not been walked yet.
};

/**
 * @brief         Hands out the next component of a path walk.
 *
 * @param[in,out] walk       The cursor, advanced past the component.
 * @param[out]    component   The slice between slashes.
 * @return        True with component filled; false when the walk is
 *                over.
 * @since         0.1.0
 * @ingroup       base_path
 */
[[nodiscard]] inline bool NextComponent(PathCursor& walk, std::string_view& component) {
    while (!walk.rest.empty() && walk.rest.front() == '/') {
        walk.rest.remove_prefix(1);
    }
    if (walk.rest.empty()) {
        return false;
    }
    const unsigned long kCut = walk.rest.find('/');
    component                = walk.rest.substr(0, kCut);
    walk.rest.remove_prefix(kCut > walk.rest.size() ? walk.rest.size() : kCut);
    return true;
}

/**
 * @brief         Splits a path at its last slash into parent and leaf.
 *
 * @param[in]     path    The path to split, viewed in place.
 * @param[out]    parent  Everything before the last slash; empty when
 *                        the path carries no slash.
 * @param[out]    leaf    Everything after the last slash; empty when
 *                        the path ends on a slash.
 * @return        True when a slash was found, false for a bare name.
 * @note          "a/b/c" gives parent "a/b" and leaf "c"; "a" gives an
 *                empty parent and leaf "a"; "a/" gives leaf "".
 * @since         0.1.0
 * @ingroup       base_path
 */
[[nodiscard]] inline bool SplitLeaf(std::string_view path, std::string_view& parent,
                                    std::string_view& leaf) {
    const unsigned long kSlash = path.rfind('/');
    if (kSlash == std::string_view::npos) {
        parent = std::string_view{};
        leaf   = path;
        return false;
    }
    parent = path.substr(0, kSlash);
    leaf   = path.substr(kSlash + 1);
    return true;
}

/**
 * @brief         Whether a path starts at the root.
 *
 * @param[in]     path   The path to ask about.
 * @return        True when the first byte is the slash.
 * @since         0.3.0
 * @ingroup       base_path
 */
[[nodiscard]] inline bool IsAbsolute(std::string_view path) {
    return !path.empty() && path.front() == '/';
}

/**
 * @brief         The last component of a path.
 *
 * @param[in]     path   The path to ask about.
 * @return        Everything past the last slash; the whole path when
 *                it carries none; empty at the root.
 * @note          After std::filesystem::path::filename: the root's
 *                file name is the empty view.
 * @since         0.3.0
 * @ingroup       base_path
 */
[[nodiscard]] inline std::string_view FileName(std::string_view path) {
    const unsigned long kSlash = path.rfind('/');
    if (kSlash == std::string_view::npos) {
        return path;
    }
    return path.substr(kSlash + 1);
}

/**
 * @brief         The path one directory up.
 *
 * @param[in]     path   The path to ask about.
 * @return        Everything before the last slash; "/" when that slash
 *                is the root's own; empty for a bare name or the root.
 * @note          After std::filesystem::path::parent_path: the root's
 *                parent is the empty view.
 * @since         0.3.0
 * @ingroup       base_path
 */
[[nodiscard]] inline std::string_view ParentPath(std::string_view path) {
    const unsigned long kSlash = path.rfind('/');
    if (kSlash == std::string_view::npos) {
        return std::string_view{};
    }
    if (kSlash == 0) {
        return path.size() == 1 ? std::string_view{} : path.substr(0, 1);
    }
    return path.substr(0, kSlash);
}

/**
 * @brief         The file name without its extension.
 *
 * @param[in]     path   The path to ask about.
 * @return        FileName minus the extension; a dot file keeps its
 *                whole name (the leading dot is not an extension).
 * @since         0.3.0
 * @ingroup       base_path
 */
[[nodiscard]] inline std::string_view Stem(std::string_view path) {
    const std::string_view kName = FileName(path);
    const unsigned long    kDot  = kName.rfind('.');
    if (kDot == std::string_view::npos || kDot == 0) {
        return kName;
    }
    return kName.substr(0, kDot);
}

/**
 * @brief         The extension of the last component, dot included.
 *
 * @param[in]     path   The path to ask about.
 * @return        Everything from the last dot inside the file name;
 *                empty when there is none (dot files carry no
 *                extension).
 * @note          After std::filesystem::path::extension: ".gz" for
 *                "a.tar.gz", empty for "Makefile" and ".hidden".
 * @since         0.3.0
 * @ingroup       base_path
 */
[[nodiscard]] inline std::string_view Extension(std::string_view path) {
    const std::string_view kName = FileName(path);
    const unsigned long    kDot  = kName.rfind('.');
    if (kDot == std::string_view::npos || kDot == 0) {
        return std::string_view{};
    }
    return kName.substr(kDot);
}

/**
 * @brief   The fold's answer: whether it fit, and how long it is.
 * @since   0.3.0
 * @ingroup base_path
 */
struct Folded {
    bool          ok;      ///< False when the result would overflow out.
    unsigned long length;  ///< Bytes written without the NUL; 1 for the root.
};

/**
 * @brief         Lays a possibly-relative path onto an absolute base.
 *
 * @param[in]     base    The absolute starting point (a working
 *                        directory); unused when addend is absolute.
 * @param[in]     addend  The path to fold; absolute addends fold alone.
 * @param[out]    out     NUL-terminated absolute result.
 * @param[in]     capacity Bytes writable at out, including the NUL.
 * @return        ok with out written; !ok when the folded path
 *                overflows capacity, or capacity cannot hold even "/".
 * @note          Dot components fold away, dot-dot pops one level and
 *                never climbs past the root (".." at "/" stays "/").
 *                The buffer doubles as the component stack: the byte
 *                after the last slash is where a pop rewinds to.
 * @since         0.2.0
 * @ingroup       base_path
 */
[[nodiscard]] Folded FoldOntoPath(std::string_view base, std::string_view addend, char* out,
                                  unsigned long capacity);

}  // namespace cinux::base
