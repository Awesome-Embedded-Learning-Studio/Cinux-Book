/**
 * @file user/cinux_gui_host/crt_stub.cpp
 * @brief C++ runtime stubs for the userspace GUI host (F-GUI-USERSPACE b3a)
 *
 * Mirrors kernel/arch/x86_64/crt_stub.cpp operator new/delete, but redirects
 * to musl malloc/free (the kernel redirects to kmalloc). Cinux-GUI core is
 * freestanding C++ (-fno-rtti -fno-exceptions); it uses new/delete only for
 * the staging buffer + Region rects (gui_core.cpp / region.cpp), no STL
 * containers, no exceptions. So these stubs + musl libc.a are sufficient --
 * no libstdc++ linkage.
 *
 * malloc/free/abort are declared extern "C" directly (not via <cstdlib>) so
 * -ffreestanding cannot drop them from the global namespace.
 *
 * __cxa_pure_virtual: Widget has abstract methods; the compiler emits a
 * reference for the vtable's pure-virtual slot. Abort if ever dispatched.
 */

#include <stddef.h>  // size_t

#include <new>  // std::align_val_t (freestanding header; no libstdc++ dep)

extern "C" void* malloc(size_t);
extern "C" void  free(void*);
extern "C" void  abort(void);

extern "C" void __cxa_pure_virtual() {
    abort();
}

// ============================================================
// Operator new / delete -> malloc / free (musl libc.a)
// ============================================================
// Must NOT be inside extern "C" -- they need C++ mangling. Full overload set
// (scalar/array x plain/aligned/sized) so no new/delete variant is left
// undefined at link time.

void* operator new(unsigned long size) {
    return malloc(static_cast<size_t>(size));
}
void* operator new[](unsigned long size) {
    return malloc(static_cast<size_t>(size));
}
void* operator new(unsigned long size, std::align_val_t align) {
    (void)align;  // malloc returns 16-byte-aligned on x86_64 -- sufficient for core
    return malloc(static_cast<size_t>(size));
}
void* operator new[](unsigned long size, std::align_val_t align) {
    (void)align;
    return malloc(static_cast<size_t>(size));
}

void operator delete(void* p) noexcept {
    free(p);
}
void operator delete(void* p, unsigned long) noexcept {
    free(p);
}
void operator delete[](void* p) noexcept {
    free(p);
}
void operator delete[](void* p, unsigned long) noexcept {
    free(p);
}
void operator delete(void* p, std::align_val_t) noexcept {
    free(p);
}
void operator delete(void* p, unsigned long, std::align_val_t) noexcept {
    free(p);
}
