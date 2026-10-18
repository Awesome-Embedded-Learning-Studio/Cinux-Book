#include "kernel/syscall/sys_getdents.hpp"

#include "cinux/math.hpp"
#include "cinux/memory.hpp"
#include "cinux/result.hpp"
#include "kernel/fs/fs_config.hpp"
#include "kernel/fs/open_file.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

namespace {

/// The name's length with its NUL, capped where the entry's name field ends.
unsigned long name_run(const fs::FsDirent& entry) {
    unsigned long length = 0;
    while (length < fs::kFsNameMax && entry.name[length] != '\0') {
        ++length;
    }
    return length + 1;
}

/// Whole record length for one name: the nineteen-byte head, the name
/// with its NUL, rounded up to eight so records abut on the walk.
unsigned long record_bytes(const fs::FsDirent& entry) {
    return cinux::base::math::AlignUp(kDirent64NameAt + name_run(entry), 8);
}

/// Places one entry into the caller's buffer at base, by the byte plan.
void put_record(unsigned char* base, const fs::FsDirent& entry, unsigned long next_ordinal) {
    const unsigned long kLen = record_bytes(entry);
    base::CopyBytes(base + kDirent64InoAt, &entry.ino, sizeof(entry.ino));
    const auto kCookie  = static_cast<long long>(next_ordinal);
    const auto kLenWord = static_cast<unsigned short>(kLen);
    const auto kType    = static_cast<unsigned char>(
        entry.type == fs::InodeType::kDirectory ? kDirentTypeDirectory : kDirentTypeFile);
    base::CopyBytes(base + kDirent64OffAt, &kCookie, sizeof(kCookie));
    base::CopyBytes(base + kDirent64LenAt, &kLenWord, sizeof(kLenWord));
    base::CopyBytes(base + kDirent64TypeAt, &kType, sizeof(kType));
    base::CopyBytes(base + kDirent64NameAt, entry.name, name_run(entry));
}

}  // namespace

long long HandleGetdents(const SyscallFrame& frame) {
    if (frame.rdi < 3) {
        return -kEbadf;
    }
    if (frame.rsi >= kUserAddressLimit || frame.rdx == 0) {
        return -kEfault;
    }
    const base::Result<fs::OpenFile*> kSlot = LiveSlot(frame.rdi);
    if (!kSlot.ok()) {
        return -kEbadf;
    }
    auto* const   kDir   = kSlot.value();
    unsigned long filled = 0;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto* const   kOut   = reinterpret_cast<unsigned char*>(frame.rsi);
    fs::FsDirent  entry{};
    for (;;) {
        const base::Result<bool> kOne =
            kDir->ops->read_dir(kDir->backend, kDir->node, kDir->offset, entry);
        if (!kOne.ok()) {
            return -ErrnoFrom(kOne.error());
        }
        if (!kOne.value()) {
            break;
        }
        const unsigned long kNeed = record_bytes(entry);
        if (filled + kNeed > frame.rdx) {
            break;
        }
        put_record(kOut + filled, entry, kDir->offset + 1);
        filled += kNeed;
        ++kDir->offset;
    }
    if (filled == 0 && frame.rdx < kDirent64NameAt) {
        return -kEinval;
    }
    return static_cast<long long>(filled);
}

}  // namespace cinux::syscall
