#include "kernel/mm/heap_runtime.hpp"

#include "cinux/addr.hpp"
#include "cinux/assert.hpp"
#include "cinux/math.hpp"
#include "kernel/arch/x86_64/irq_guard.hpp"  // NOLINT(misc-include-cleaner) IrqGuard is used below
#include "kernel/arch/x86_64/page.hpp"
#include "kernel/arch/x86_64/registers.hpp"
#include "kernel/mm/heap.hpp"
#include "kernel/mm/heap_config.hpp"
#include "kernel/mm/layout.hpp"
#include "kernel/mm/page_walk.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vmm.hpp"

namespace cinux::mm {

namespace {

using cinux::arch::page::kSize;
using walk::MapPage;

bool map_heap_pages(unsigned long spot, unsigned long bytes) {
    KernelTables        tables;
    unsigned long const kRoot  = cinux::arch::ReadCr3();
    unsigned long const kPages = cinux::base::math::Ceil(bytes, kSize);
    for (unsigned long page = 0; page < kPages; ++page) {
        base::PhysAddr const kFrame = Pmm::self().allocate_page();
        if (kFrame == base::PhysAddr{}) {
            return false;
        }
        if (!MapPage(tables, kRoot, spot + (page * kSize), kFrame.raw)) {
            return false;
        }
    }
    return true;
}

}  // namespace

bool BringUpHeap() {
    unsigned long const kBase = kHeapBase + kHeapLowerGuardBytes;
    if (!map_heap_pages(kBase, kHeapInitialBytes)) {
        return false;
    }
    return Heap::self().init(kBase, kHeapInitialBytes);
}

void* HeapAllocate(unsigned long bytes) {
    const cinux::arch::IrqGuard kGuard;
    if (void* const kBlock = Heap::self().allocate(bytes)) {
        return kBlock;
    }
    unsigned long const kPages  = cinux::base::math::Ceil(bytes, kSize) + 1;
    unsigned long const kGrow   = kPages * kSize;
    unsigned long const kOldTop = Heap::self().top();
    if (kOldTop == 0 || !map_heap_pages(kOldTop, kGrow) || !Heap::self().grow(kOldTop + kGrow)) {
        return nullptr;
    }
    return Heap::self().allocate(bytes);
}

bool HeapFree(void* block) {
    const cinux::arch::IrqGuard kGuard;
    return Heap::self().free(block);
}

}  // namespace cinux::mm

void* operator new(unsigned long bytes) {
    void* const kBlock = cinux::mm::HeapAllocate(bytes);
    cinux::base::safety::Check(kBlock != nullptr, "kernel heap exhausted");
    return kBlock;
}

void* operator new[](unsigned long bytes) {
    void* const kBlock = cinux::mm::HeapAllocate(bytes);
    cinux::base::safety::Check(kBlock != nullptr, "kernel heap exhausted");
    return kBlock;
}

void operator delete(void* block) noexcept {
    if (block == nullptr) {
        return;
    }
    cinux::base::safety::Check(cinux::mm::HeapFree(block), "invalid heap free");
}

void operator delete(void* block, unsigned long size [[maybe_unused]]) noexcept {
    if (block == nullptr) {
        return;
    }
    cinux::base::safety::Check(cinux::mm::HeapFree(block), "invalid heap free");
}

void operator delete[](void* block) noexcept {
    if (block == nullptr) {
        return;
    }
    cinux::base::safety::Check(cinux::mm::HeapFree(block), "invalid heap free");
}

void operator delete[](void* block, unsigned long size [[maybe_unused]]) noexcept {
    if (block == nullptr) {
        return;
    }
    cinux::base::safety::Check(cinux::mm::HeapFree(block), "invalid heap free");
}
