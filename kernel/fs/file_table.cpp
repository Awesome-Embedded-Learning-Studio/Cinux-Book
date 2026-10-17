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

#include "cinux/assert.hpp"
#include "cinux/result.hpp"
#include "cinux/shared.hpp"
#include "kernel/arch/x86_64/irq_guard.hpp"
#include "kernel/fs/open_file.hpp"
#include "kernel/fs/vfs.hpp"

namespace cinux::fs {

FileTable::~FileTable() {
    close_all();
}

base::Result<unsigned long> FileTable::alloc(const FsOps* ops, void* backend, FsNode node) {
    const arch::IrqGuard         kGuard;
    const base::Shared<OpenFile> kFresh{new OpenFile(ops, backend, node)};
    if (!kFresh.holds()) {
        return base::KernelError::kOutOfMemory;
    }
    const unsigned long kFd = slots_.insert(kFresh, kFirstDescriptor);
    if (kFd == kFileTableMax) {
        return base::KernelError::kOutOfMemory;
    }
    if (ops != nullptr) {
        ops->retain(backend, node);
    }
    return kFd;
}

base::Result<unsigned long> FileTable::adopt(const base::Shared<OpenFile>& file) {
    const arch::IrqGuard kGuard;
    if (!file.holds()) {
        return base::KernelError::kInvalidArgument;
    }
    const unsigned long kFd = slots_.insert(file, kFirstDescriptor);
    if (kFd == kFileTableMax) {
        return base::KernelError::kOutOfMemory;
    }
    return kFd;
}

base::Result<OpenFile*> FileTable::get(unsigned long descriptor) {
    if (descriptor >= kFileTableMax) {
        return base::KernelError::kInvalidArgument;
    }
    const auto* const kSlot = slots_.get(descriptor);
    if (kSlot == nullptr) {
        return base::KernelError::kNotFound;
    }
    return kSlot->get();
}

base::Result<base::Shared<OpenFile>> FileTable::shared(unsigned long descriptor) {
    if (descriptor >= kFileTableMax) {
        return base::KernelError::kInvalidArgument;
    }
    const auto* const kSlot = slots_.get(descriptor);
    if (kSlot == nullptr) {
        return base::KernelError::kNotFound;
    }
    return *kSlot;
}

base::Result<void> FileTable::free(unsigned long descriptor) {
    const arch::IrqGuard                       kGuard;
    const base::Result<base::Shared<OpenFile>> kHeld = shared(descriptor);
    if (!kHeld.ok()) {
        return kHeld.error();
    }
    base::safety::Check(slots_.erase(descriptor), "live file descriptor lost its slot");
    return {};
}

void FileTable::close_all() {
    const arch::IrqGuard kGuard;
    slots_.for_each([this](unsigned long descriptor, [[maybe_unused]] base::Shared<OpenFile>&) {
        base::safety::Check(free(descriptor).ok(), "live file descriptor refused close");
    });
}

}  // namespace cinux::fs
