/**
 * @file file_table.cpp
 * @brief Dynamically grown descriptor chunks with shared open ownership.
 * @author Charliechen114514
 * @date 2026-10-26
 * @version 0.2
 * @since 0.8.0
 * @ingroup kernel_fs
 * @copyright Copyright (c) 2026
 */
#include "kernel/fs/file_table.hpp"

#include <utility>

#include "cinux/assert.hpp"
#include "cinux/bit_ops/bitmap.hpp"
#include "cinux/container/slot_table.hpp"
#include "cinux/math.hpp"
#include "cinux/own.hpp"
#include "cinux/result.hpp"
#include "cinux/shared.hpp"
#include "kernel/arch/x86_64/irq_guard.hpp"
#include "kernel/fs/open_file.hpp"
#include "kernel/fs/vfs.hpp"

namespace cinux::fs {
namespace {
constexpr unsigned long kChunkSlots = sizeof(base::bit::WordMask) * 8;
}

struct FileTable::Chunk {
    Chunk() { cloexec.init(&word, kChunkSlots); }
    base::container::SlotTable<base::Shared<OpenFile>, kChunkSlots> slots;
    base::bit::WordMask                                             word;
    base::bit::Bitmap                                               cloexec;
};

void FileTable::TablePolicy::enter(State& state) {
    state.irq = arch::SaveAndDisableIrq();
    state.table.gate_.lock();
}

void FileTable::TablePolicy::exit(State& state) {
    state.table.gate_.unlock();
    arch::RestoreIrq(state.irq);
}

FileTable::FileTable() = default;
FileTable::~FileTable() {
    close_all();
}

FileTable::Chunk* FileTable::chunk_locked(unsigned long descriptor) const {
    const auto kIndex = descriptor / kChunkSlots;
    return kIndex < chunk_count_ ? chunks_[kIndex] : nullptr;
}

FileTable::Chunk* FileTable::ensure_chunk_locked(unsigned long descriptor) {
    if (descriptor >= kFileTableMax) {
        return nullptr;
    }
    const auto kIndex = descriptor / kChunkSlots;
    if (kIndex >= chunk_count_) {
        const auto          kCount = base::math::Min(base::math::Max(kIndex + 1, chunk_count_ * 2),
                                                     kFileTableMax / kChunkSlots);
        base::Own<Chunk*[]> fresh(new Chunk*[kCount]);
        for (unsigned long index = 0; index < kCount; ++index) {
            fresh[index] = index < chunk_count_ ? chunks_[index] : nullptr;
        }
        chunks_      = std::move(fresh);
        chunk_count_ = kCount;
    }
    if (chunks_[kIndex] == nullptr) {
        chunks_[kIndex] = new Chunk();
    }
    return chunks_[kIndex];
}

base::Result<unsigned long> FileTable::vacant_locked(unsigned long floor,
                                                     unsigned long limit) const {
    const auto kLimit = base::math::Min(limit, kFileTableMax);
    for (unsigned long descriptor = base::math::Max(floor, next_fd_); descriptor < kLimit;) {
        const auto* const kChunk = chunk_locked(descriptor);
        if (kChunk == nullptr) {
            return descriptor;
        }
        const auto kLocal = kChunk->slots.vacant(descriptor % kChunkSlots);
        const auto kBase  = base::math::AlignDown(descriptor, kChunkSlots);
        if (kLocal < kChunkSlots) {
            return kBase + kLocal < kLimit
                       ? base::Result<unsigned long>{kBase + kLocal}
                       : base::Result<unsigned long>{base::KernelError::kTooManyOpenFiles};
        }
        descriptor = kBase + kChunkSlots;
    }
    return base::KernelError::kTooManyOpenFiles;
}

base::Result<unsigned long> FileTable::alloc(const FsOps* ops, void* backend, FsNode node,
                                             OpenOptions options) {
    {
        const TableGuard kGuard(*this);
        const auto       kFree = vacant_locked(0, options.limit);
        if (!kFree.ok()) {
            return kFree.error();
        }
    }
    const base::Shared<OpenFile> kFresh{new OpenFile(ops, backend, node)};
    kFresh->lifetime = std::move(options.lifetime);
    if (ops != nullptr) {
        ops->retain(backend, node);
    }
    kFresh->status_flags = options.status;
    return adopt_from(kFresh, 0, options.cloexec, options.limit);
}

base::Result<unsigned long> FileTable::adopt(const base::Shared<OpenFile>& file,
                                             unsigned long                 limit) {
    return adopt_from(file, 0, false, limit);
}

base::Result<unsigned long> FileTable::adopt_at(const base::Shared<OpenFile>& file,
                                                unsigned long                 descriptor) {
    const TableGuard kGuard(*this);
    if (!file.holds() || descriptor >= kFileTableMax) {
        return base::KernelError::kInvalidArgument;
    }
    auto* const kChunk = ensure_chunk_locked(descriptor);
    const auto  kLocal = descriptor % kChunkSlots;
    if (!kChunk->slots.place(kLocal, file)) {
        return base::KernelError::kAlreadyExists;
    }
    kChunk->cloexec.clear(kLocal);
    return descriptor;
}

base::Result<unsigned long> FileTable::adopt_from(const base::Shared<OpenFile>& file,
                                                  unsigned long floor, bool cloexec,
                                                  unsigned long limit) {
    const TableGuard kGuard(*this);
    if (!file.holds()) {
        return base::KernelError::kInvalidArgument;
    }
    const auto kFree = vacant_locked(floor, limit);
    if (!kFree.ok()) {
        return kFree.error();
    }
    const auto  kFd    = kFree.value();
    auto* const kChunk = ensure_chunk_locked(kFd);
    const auto  kLocal = kFd % kChunkSlots;
    base::safety::Check(kChunk->slots.place(kLocal, file), "free descriptor became occupied");
    if (cloexec) {
        kChunk->cloexec.set(kLocal);
    } else {
        kChunk->cloexec.clear(kLocal);
    }
    if (floor <= next_fd_) {
        next_fd_ = kFd + 1;
    }
    return kFd;
}

base::Result<base::Shared<OpenFile>> FileTable::shared_locked(unsigned long descriptor) {
    if (descriptor >= kFileTableMax) {
        return base::KernelError::kInvalidArgument;
    }
    auto* const       kChunk = chunk_locked(descriptor);
    const auto* const kSlot =
        kChunk != nullptr ? kChunk->slots.get(descriptor % kChunkSlots) : nullptr;
    return kSlot != nullptr ? base::Result<base::Shared<OpenFile>>{*kSlot}
                            : base::Result<base::Shared<OpenFile>>{base::KernelError::kNotFound};
}

base::Result<base::Shared<OpenFile>> FileTable::get(unsigned long descriptor) {
    return shared(descriptor);
}

base::Result<base::Shared<OpenFile>> FileTable::shared(unsigned long descriptor) {
    const TableGuard kGuard(*this);
    return shared_locked(descriptor);
}

base::Shared<OpenFile> FileTable::detach_locked(unsigned long descriptor) {
    base::Shared<OpenFile> held;
    auto* const            kChunk = chunk_locked(descriptor);
    if (kChunk == nullptr) {
        return held;
    }
    const auto kLocal = descriptor % kChunkSlots;
    if (auto* slot = kChunk->slots.get(kLocal); slot != nullptr) {
        held = std::move(*slot);
        base::safety::Check(kChunk->slots.erase(kLocal), "live descriptor lost its slot");
        next_fd_ = base::math::Min(next_fd_, descriptor);
    }
    kChunk->cloexec.clear(kLocal);
    return held;
}

base::Result<void> FileTable::free(unsigned long descriptor) {
    base::Shared<OpenFile> held;
    {
        const TableGuard kGuard(*this);
        const auto       kSlot = shared_locked(descriptor);
        if (!kSlot.ok()) {
            return kSlot.error();
        }
        held = detach_locked(descriptor);
    }
    return {};
}

void FileTable::close_all() {
    base::Own<Chunk*[]> held;
    unsigned long       count = 0;
    {
        const TableGuard kGuard(*this);
        held         = std::move(chunks_);
        count        = chunk_count_;
        chunk_count_ = 0;
        next_fd_     = 0;
    }
    for (unsigned long index = 0; index < count; ++index) {
        delete held[index];
    }
}

base::Result<void> FileTable::set_cloexec(unsigned long descriptor, bool armed) {
    const TableGuard kGuard(*this);
    const auto       kSlot = shared_locked(descriptor);
    if (!kSlot.ok()) {
        return kSlot.error();
    }
    auto* const kChunk = chunk_locked(descriptor);
    if (armed) {
        kChunk->cloexec.set(descriptor % kChunkSlots);
    } else {
        kChunk->cloexec.clear(descriptor % kChunkSlots);
    }
    return {};
}

bool FileTable::cloexec(unsigned long descriptor) const {
    const TableGuard  kGuard(*this);
    const auto* const kChunk = chunk_locked(descriptor);
    return kChunk != nullptr && kChunk->cloexec.test(descriptor % kChunkSlots);
}

unsigned long FileTable::capacity() const {
    const TableGuard kGuard(*this);
    return chunk_count_ * kChunkSlots;
}

void FileTable::close_on_exec() {
    base::Own<base::Shared<OpenFile>[]> held;
    {
        const TableGuard kGuard(*this);
        const auto       kCapacity = chunk_count_ * kChunkSlots;
        if (kCapacity == 0) {
            return;
        }
        held.reset(new base::Shared<OpenFile>[kCapacity]);
        for (unsigned long descriptor = 0; descriptor < kCapacity; ++descriptor) {
            const auto* const kChunk = chunk_locked(descriptor);
            if (kChunk != nullptr && kChunk->cloexec.test(descriptor % kChunkSlots)) {
                held[descriptor] = detach_locked(descriptor);
            }
        }
    }
}

base::Shared<FileTable> FileTable::clone() {
    const base::Shared<FileTable> kCopy{new FileTable()};
    const TableGuard              kGuard(*this);
    for (unsigned long index = 0; index < chunk_count_; ++index) {
        const auto* const kChunk = chunks_[index];
        if (kChunk == nullptr) {
            continue;
        }
        auto* const kBorn = kCopy->ensure_chunk_locked(index * kChunkSlots);
        kChunk->slots.for_each([&](unsigned long local, const base::Shared<OpenFile>& file) {
            base::safety::Check(kBorn->slots.place(local, file), "fresh snapshot occupied");
            if (kChunk->cloexec.test(local)) {
                kBorn->cloexec.set(local);
            }
        });
    }
    kCopy->next_fd_ = next_fd_;
    return kCopy;
}

base::Result<unsigned long> FileTable::duplicate_at(unsigned long source, unsigned long destination,
                                                    unsigned long limit) {
    base::Shared<OpenFile> replaced;
    {
        const TableGuard kGuard(*this);
        const auto       kSource = shared_locked(source);
        if (!kSource.ok()) {
            return kSource.error();
        }
        if (source == destination) {
            return destination;
        }
        if (destination >= limit || destination >= kFileTableMax) {
            return base::KernelError::kInvalidArgument;
        }
        auto* const kChunk = ensure_chunk_locked(destination);
        replaced           = detach_locked(destination);
        base::safety::Check(kChunk->slots.place(destination % kChunkSlots, kSource.value()),
                            "duplicate destination remained occupied");
    }
    return destination;
}

}
