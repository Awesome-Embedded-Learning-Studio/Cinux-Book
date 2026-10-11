/**
 * @file    path.cpp
 * @brief   The fold: laying a path onto a working directory.
 *
 * @author  Charliechen114514
 * @date    2026-10-11
 * @version 0.1
 * @since   0.2.0
 * @ingroup base_path
 * @copyright Copyright (c) 2026
 */

#include "cinux/path.hpp"

#include <string_view>

#include "cinux/memory.hpp"

namespace cinux::base {

namespace {

struct Fold {
    char*         out      = nullptr;
    unsigned long capacity = 0;
    unsigned long used     = 0;
    unsigned long depth    = 0;

    bool push(std::string_view component) {
        if ((used + component.size()) + 2UL > capacity) {
            return false;
        }
        out[used] = '/';
        CopyBytes(out + used + 1, component.data(), component.size());
        used += 1 + component.size();
        out[used] = '\0';
        ++depth;
        return true;
    }

    void pop() {
        if (depth == 0) {
            return;
        }
        --depth;
        while (used > 0 && out[used - 1] != '/') {
            --used;
        }
        if (used > 0) {
            --used;
        }
        out[used] = '\0';
    }

    bool walk(std::string_view view) {
        PathCursor       cursor{view};
        std::string_view part;
        while (NextComponent(cursor, part)) {
            if (part == ".") {
                continue;
            }
            if (part == "..") {
                pop();
                continue;
            }
            if (!push(part)) {
                return false;
            }
        }
        return true;
    }
};

}  // namespace

Folded FoldOntoPath(std::string_view base, std::string_view addend, char* out,
                    unsigned long capacity) {
    if (capacity < 2UL) {
        return Folded{.ok = false, .length = 0};
    }
    Fold                   fold{.out = out, .capacity = capacity};
    const std::string_view kBaseView =
        addend.empty() || addend.front() != '/' ? base : std::string_view{};
    if (!fold.walk(kBaseView) || !fold.walk(addend)) {
        return Folded{.ok = false, .length = 0};
    }
    if (fold.used == 0) {
        out[0] = '/';
        out[1] = '\0';
    }
    return Folded{.ok = true, .length = fold.used == 0 ? 1UL : fold.used};
}

}  // namespace cinux::base
