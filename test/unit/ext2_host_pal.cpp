/**
 * @file test/unit/ext2_host_pal.cpp
 * @brief Host-side PAL stubs for the ext2 library (libs/ext2/)
 *
 * The ext2 driver is built twice: once for the kernel (big_kernel_common,
 * inheriting -mcmodel=kernel codegen) and once for host integration tests
 * (compiled directly, host codegen -- the net_tcp pattern). The host build
 * cannot link the kernel's own PAL implementations:
 *   - kernel/lib/kprintf.cpp pulls serial.hpp + x86 inline asm (outl/cli/hlt)
 *   - kernel/mm/slab.cpp routes through PMM/buddy (kernel-only)
 *
 * So this file provides the symbols ext2 consumes, backed by libc:
 *   cinux::lib::kprintf / kvprintf / kpanic + sink-registration no-ops
 *   cinux::mm::kmalloc / kfree  (aligned + zeroed, matching the slab contract)
 *
 * Spinlock comes from host_spinlock.cpp; inode_ref / inode_unref from
 * kernel/fs/file.cpp (host-safe, the devfs test precedent). Both are linked
 * into the same integration test target.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/slab.hpp"

namespace cinux::lib {

void kprintf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);  // host: route to stdout
    va_end(args);
}

void kvprintf(const char* fmt, va_list args) {
    vprintf(fmt, args);
}

[[noreturn]] void kpanic(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    abort();
}

// Sink registration is a kernel multi-backend concept (serial + framebuffer +
// QEMU debugconsole). The host PAL has a single implicit stdout sink (the
// vprintf above), so these are no-ops.
void kprintf_register_sink(OutputSink /*fn*/, void* /*ctx*/) {}
void kprintf_set_sink_enabled(OutputSink /*fn*/, void* /*ctx*/, bool /*enabled*/) {}
void kprintf_enable_all_sinks() {}
void kprintf_init() {}

}  // namespace cinux::lib

namespace cinux::mm {

void* kmalloc(size_t size, size_t align) {
    if (size == 0) return nullptr;
    if (align < sizeof(void*)) align = sizeof(void*);
    // aligned_alloc requires size to be a multiple of alignment.
    size_t rounded = (size + align - 1) & ~(align - 1);
    void* p = aligned_alloc(align, rounded);
    // Match the kernel slab contract: returned memory is zeroed (no stale-data
    // leak), which ext2 scratch buffers (KmBuf) and the allocator paths assume.
    if (p != nullptr) {
        memset(p, 0, rounded);
    }
    return p;
}

void kfree(void* ptr) {
    free(ptr);
}

}  // namespace cinux::mm
