#include "kernel/syscall/sys_getdents.hpp"

#include "cinux/math.hpp"
#include "cinux/memory.hpp"
#include "cinux/result.hpp"
#include "kernel/fs/fs_config.hpp"
#include "kernel/fs/open_file.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"
#include "kernel/syscall/uaccess.hpp"

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

/// Translates one entry's shape into the wire's type word.
unsigned char dirent_type_word(fs::InodeType type) {
    switch (type) {
    case fs::InodeType::kDirectory:
        return kDirentTypeDirectory;
    case fs::InodeType::kCharDevice:
        return kDirentTypeChar;
    default:
        return kDirentTypeFile;
    }
}

/// The widest record one entry can produce: the head plus the longest
/// name with its NUL, rounded up to the record grain.
constexpr unsigned long kRecordBytes = kDirent64NameAt + fs::kFsNameMax + 8;

/// Places one entry into a record buffer at base, by the byte plan.
void put_record(unsigned char* base, const fs::FsDirent& entry, unsigned long next_ordinal) {
    const unsigned long kLen = record_bytes(entry);
    base::CopyBytes(base + kDirent64InoAt, &entry.ino, sizeof(entry.ino));
    const auto kCookie  = static_cast<long long>(next_ordinal);
    const auto kLenWord = static_cast<unsigned short>(kLen);
    const auto kType    = dirent_type_word(entry.type);
    base::CopyBytes(base + kDirent64OffAt, &kCookie, sizeof(kCookie));
    base::CopyBytes(base + kDirent64LenAt, &kLenWord, sizeof(kLenWord));
    base::CopyBytes(base + kDirent64TypeAt, &kType, sizeof(kType));
    base::CopyBytes(base + kDirent64NameAt, entry.name, name_run(entry));
}

}  // namespace

long long HandleGetdents(const SyscallFrame& frame) {
    if (frame.rdx == 0) {
        return -kEfault;
    }
    const base::Result<fs::OpenFile*> kSlot = LiveSlot(frame.rdi);
    if (!kSlot.ok()) {
        return -kEbadf;
    }
    auto* const   kDir   = kSlot.value();
    unsigned long filled = 0;
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
        unsigned char record[kRecordBytes];
        put_record(record, entry, kDir->offset + 1);
        if (!uaccess::CopyToUser(frame.rsi + filled, record, kNeed)) {
            return -kEfault;
        }
        filled += kNeed;
        ++kDir->offset;
    }
    if (filled == 0 && frame.rdx < kDirent64NameAt) {
        return -kEinval;
    }
    return static_cast<long long>(filled);
}

}  // namespace cinux::syscall
