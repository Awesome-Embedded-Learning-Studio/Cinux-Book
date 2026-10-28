#include <algorithm>
#include <cstdint>

#include "cinux/own.hpp"
#include "cinux/path.hpp"
#include "kernel/arch/x86_64/page.hpp"
#include "kernel/fs/fs_config.hpp"
#include "kernel/fs/open_file.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"
#include "kernel/syscall/uaccess.hpp"

namespace cinux::syscall {

long long CopyUserPath(unsigned long long source, char* out, unsigned long cap) {
    if (source == 0 || source >= kUserAddressLimit) {
        return -kEfault;
    }
    constexpr unsigned long kChunkBytes = 64;
    unsigned long           done        = 0;
    while (done < cap) {
        const unsigned long kInPage =
            cinux::arch::page::kSize - ((source + done) % cinux::arch::page::kSize);
        const unsigned long kChunk = std::min({kChunkBytes, cap - done, kInPage});
        if (!uaccess::CopyFromUser(out + done, source + done, kChunk)) {
            return -kEfault;
        }
        for (unsigned long index = done; index < done + kChunk; ++index) {
            if (out[index] == '\0') {
                return index == 0 ? -kEinval : static_cast<long long>(index);
            }
        }
        done += kChunk;
    }
    return -kEinval;
}

long long FoldUserPath(unsigned long long source, char* out, unsigned long cap) {
    const base::Own<char[]> kRawPath(new char[fs::kUserPathMax]);
    if (!kRawPath.holds()) {
        return -kEnomem;
    }
    const long long kRaw = CopyUserPath(source, kRawPath.get(), fs::kUserPathMax);
    if (kRaw < 0) {
        return kRaw;
    }
    const char* const  kBase = kRawPath[0] == '/' ? "/" : CurrentTask()->cwd;
    const base::Folded kFold = base::FoldOntoPath(kBase, kRawPath.get(), out, cap);
    if (!kFold.ok) {
        return -kEnametoolong;
    }
    return static_cast<long long>(kFold.length);
}

long long FoldUserPathAt(Word descriptor, Word address, char* output) {
    char path[fs::kFsPathMax]{};
    if (const auto kBad = CopyUserPath(address, path, sizeof(path)); kBad < 0) {
        return kBad;
    }
    if (path[0] == '/' || static_cast<std::int32_t>(descriptor) == kAtFdcwd) {
        return FoldUserPath(address, output, fs::kFsPathMax);
    }
    const auto kSlot = LiveSlot(descriptor);
    if (!kSlot.ok()) {
        return -kEbadf;
    }
    if (kSlot.value()->node.type != fs::InodeType::kDirectory) {
        return -kEnotdir;
    }
    return -kEopnotsupp;
}

}
