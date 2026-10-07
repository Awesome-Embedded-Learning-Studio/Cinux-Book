#pragma once

#include "kernel/fs/vfs.hpp"

namespace cinux::test {

struct FileRefs {
    unsigned long retained = 0;
    unsigned long released = 0;

    static const fs::FsOps kOps;
};

inline constinit const fs::FsOps FileRefs::kOps = []() noexcept {
    fs::FsOps ops{};
    ops.retain = [](void* self, [[maybe_unused]] fs::FsNode node) {
        static_cast<FileRefs*>(self)->retained++;
    };
    ops.release = [](void* self, [[maybe_unused]] fs::FsNode node) {
        static_cast<FileRefs*>(self)->released++;
    };
    return ops;
}();

}  // namespace cinux::test
