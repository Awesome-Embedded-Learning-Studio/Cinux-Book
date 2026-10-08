/**
 * @file    path.hpp
 * @brief   Slash-separated path views: walk components, split the leaf.
 *
 * Paths travel as string_view and are never copied here. Two shapes
 * cover the callers so far: a cursor handing out one component at a
 * time (tolerating repeated slashes), and a leaf split for the
 * parent-plus-name question removals and renames ask.
 *
 * @author  Charliechen114514
 * @date    2026-10-07
 * @version 0.1
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

}  // namespace cinux::base
