#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/arch/x86_64/phys_virt.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vmm.hpp"

namespace cinux::test {

class UserPage {
public:
    explicit UserPage(uint64_t virt) : virt_(virt) {
        phys_ = cinux::mm::g_pmm.alloc_page();
        if (phys_ == 0) {
            return;
        }
        ok_ = cinux::mm::g_vmm.map(virt_, phys_, cinux::arch::FLAG_PRESENT |
                                                    cinux::arch::FLAG_WRITABLE |
                                                    cinux::arch::FLAG_USER);
        if (!ok_) {
            cinux::mm::g_pmm.free_page(phys_);
            phys_ = 0;
        }
    }

    ~UserPage() {
        if (ok_) {
            cinux::mm::g_vmm.unmap(virt_);
        }
        if (phys_ != 0) {
            cinux::mm::g_pmm.free_page(phys_);
        }
    }

    UserPage(const UserPage&)            = delete;
    UserPage& operator=(const UserPage&) = delete;

    bool ok() const { return ok_; }

    uint64_t addr(size_t off = 0) const { return virt_ + off; }

    template <typename T>
    bool write(size_t off, const T& value) {
        return write_bytes(off, &value, sizeof(T));
    }

    template <typename T>
    bool read(size_t off, T& value) const {
        return read_bytes(off, &value, sizeof(T));
    }

    bool write_bytes(size_t off, const void* src, size_t n) {
        if (!ok_ || off + n > cinux::arch::PAGE_SIZE) {
            return false;
        }
        auto*       dst = reinterpret_cast<uint8_t*>(phys_ + cinux::arch::DIRECT_MAP_BASE + off);
        const auto* in  = reinterpret_cast<const uint8_t*>(src);
        for (size_t i = 0; i < n; ++i) {
            dst[i] = in[i];
        }
        return true;
    }

    bool read_bytes(size_t off, void* dst, size_t n) const {
        if (!ok_ || off + n > cinux::arch::PAGE_SIZE) {
            return false;
        }
        auto*       out = reinterpret_cast<uint8_t*>(dst);
        const auto* src =
            reinterpret_cast<const uint8_t*>(phys_ + cinux::arch::DIRECT_MAP_BASE + off);
        for (size_t i = 0; i < n; ++i) {
            out[i] = src[i];
        }
        return true;
    }

private:
    uint64_t virt_{};
    uint64_t phys_{};
    bool     ok_{false};
};

}  // namespace cinux::test
