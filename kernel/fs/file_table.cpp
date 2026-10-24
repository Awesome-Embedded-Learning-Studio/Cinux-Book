/**
 * @file    file_table.cpp
 * @brief   Descriptor table mechanics: seat opens, hand them out, let go.
 *
 * @author  Charliechen114514
 * @date    2026-10-17
 * @version 0.1
 * @since   0.8.0
 * @ingroup kernel_fs
 * @copyright Copyright (c) 2026
 */

#include "kernel/fs/file_table.hpp"

#include <utility>

#include "cinux/assert.hpp"
#include "cinux/result.hpp"
#include "cinux/shared.hpp"
#include "kernel/arch/x86_64/irq_guard.hpp"
#include "kernel/fs/open_file.hpp"
#include "kernel/fs/vfs.hpp"

namespace cinux::fs {

void FileTable::TablePolicy::enter(State& state) {
    state.irq = arch::SaveAndDisableIrq();
    state.table.gate_.lock();
}

void FileTable::TablePolicy::exit(State& state) {
    state.table.gate_.unlock();
    arch::RestoreIrq(state.irq);
}

FileTable::FileTable() {
    cloexec_map_.init(cloexec_words_, kFileTableMax);
}

FileTable::~FileTable() {
    close_all();
}

base::Result<unsigned long> FileTable::alloc(const FsOps* ops, void* backend, FsNode node,
                                             OpenOptions options) {
    {
        const TableGuard kGuard(*this);
        if (slots_.vacant() == kFileTableMax) {
            return base::KernelError::kOutOfMemory;
        }
    }
    const base::Shared<OpenFile> kFresh{new OpenFile(ops, backend, node)};
    if (!kFresh.holds()) {
        return base::KernelError::kOutOfMemory;
    }
    if (ops != nullptr) {
        ops->retain(backend, node);
    }
    kFresh->status_flags = options.status;
    const TableGuard    kGuard(*this);
    const unsigned long kFd = slots_.insert(kFresh, 0);
    if (kFd == kFileTableMax) {
        return base::KernelError::kOutOfMemory;
    }
    if (options.cloexec) {
        cloexec_map_.set(kFd);
    } else {
        cloexec_map_.clear(kFd);
    }
    return kFd;
}

base::Result<unsigned long> FileTable::adopt(const base::Shared<OpenFile>& file) {
    return adopt_from(file, 0);
}

base::Result<unsigned long> FileTable::adopt_at(const base::Shared<OpenFile>& file,
                                                unsigned long                 descriptor) {
    const TableGuard kGuard(*this);
    if (!file.holds() || descriptor >= kFileTableMax) {
        return base::KernelError::kInvalidArgument;
    }
    if (!slots_.place(descriptor, file)) {
        return base::KernelError::kAlreadyExists;
    }
    cloexec_map_.clear(descriptor);
    return descriptor;
}

base::Result<unsigned long> FileTable::adopt_from(const base::Shared<OpenFile>& file,
                                                  unsigned long floor, bool cloexec) {
    const TableGuard kGuard(*this);
    if (!file.holds()) {
        return base::KernelError::kInvalidArgument;
    }
    const unsigned long kFd = slots_.insert(file, floor);
    if (kFd == kFileTableMax) {
        return base::KernelError::kOutOfMemory;
    }
    if (cloexec) {
        cloexec_map_.set(kFd);
    } else {
        cloexec_map_.clear(kFd);
    }
    return kFd;
}

base::Result<base::Shared<OpenFile>> FileTable::shared_locked(unsigned long descriptor) {
    if (descriptor >= kFileTableMax) {
        return base::KernelError::kInvalidArgument;
    }
    const auto* const kSlot = slots_.get(descriptor);
    if (kSlot == nullptr) {
        return base::KernelError::kNotFound;
    }
    return *kSlot;
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
    if (auto* slot = slots_.get(descriptor); slot != nullptr) {
        held = std::move(*slot);
        base::safety::Check(slots_.erase(descriptor), "live file descriptor lost its slot");
    }
    cloexec_map_.clear(descriptor);
    return held;
}

base::Result<void> FileTable::free(unsigned long descriptor) {
    base::Shared<OpenFile> held;
    {
        const TableGuard kGuard(*this);
        if (descriptor >= kFileTableMax) {
            return base::KernelError::kInvalidArgument;
        }
        if (slots_.get(descriptor) == nullptr) {
            return base::KernelError::kNotFound;
        }
        held = detach_locked(descriptor);
    }
    return {};
}

void FileTable::close_all() {
    base::Shared<OpenFile> held[kFileTableMax];
    {
        const TableGuard kGuard(*this);
        for (unsigned long descriptor = 0; descriptor < kFileTableMax; ++descriptor) {
            held[descriptor] = detach_locked(descriptor);
        }
    }
}

base::Result<void> FileTable::set_cloexec(unsigned long descriptor, bool armed) {
    const TableGuard kGuard(*this);
    if (descriptor >= kFileTableMax) {
        return base::KernelError::kInvalidArgument;
    }
    if (armed) {
        cloexec_map_.set(descriptor);
    } else {
        cloexec_map_.clear(descriptor);
    }
    return {};
}

bool FileTable::cloexec(unsigned long descriptor) const {
    const TableGuard kGuard(*this);
    return descriptor < kFileTableMax && cloexec_map_.test(descriptor);
}

void FileTable::close_on_exec() {
    base::Shared<OpenFile> held[kFileTableMax];
    {
        const TableGuard kGuard(*this);
        for (unsigned long descriptor = 0; descriptor < kFileTableMax; ++descriptor) {
            if (cloexec_map_.test(descriptor)) {
                held[descriptor] = detach_locked(descriptor);
            }
        }
    }
}

base::Shared<FileTable> FileTable::clone() {
    base::Shared<FileTable> copy{new FileTable()};
    if (!copy.holds()) {
        return {};
    }
    const TableGuard kGuard(*this);
    slots_.for_each([&](unsigned long descriptor, const base::Shared<OpenFile>& file) {
        base::safety::Check(copy->slots_.place(descriptor, file), "fresh file snapshot occupied");
        if (cloexec_map_.test(descriptor)) {
            copy->cloexec_map_.set(descriptor);
        }
    });
    return copy;
}

base::Result<unsigned long> FileTable::duplicate_at(unsigned long source,
                                                    unsigned long destination) {
    base::Shared<OpenFile> replaced;
    {
        const TableGuard kGuard(*this);
        const auto       kSource = shared_locked(source);
        if (!kSource.ok()) {
            return kSource.error();
        }
        if (destination >= kFileTableMax) {
            return base::KernelError::kInvalidArgument;
        }
        if (source == destination) {
            return destination;
        }
        replaced = detach_locked(destination);
        base::safety::Check(slots_.place(destination, kSource.value()),
                            "duplicate destination remained occupied");
    }
    return destination;
}

}
