#include "cinux/memory.hpp"
#include "cinux/ptr.hpp"
#include "kernel/arch/x86_64/page.hpp"
#include "kernel/arch/x86_64/page_entry.hpp"
#include "kernel/mm/address_space.hpp"
#include "kernel/mm/layout.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vma.hpp"
#include "kernel/proc/signals.hpp"

namespace cinux::proc {

bool MapSignalTrampoline(const mm::AddressSpace& space, mm::VmaStore& vmas) {
    mm::VmaRegion region{};
    region.start      = SignalTrampolineAddress();
    region.end        = SignalTrampolineAddress() + cinux::arch::page::kSize;
    region.kind       = mm::VmaKind::kAnonymous;
    region.readable   = true;
    region.executable = true;
    if (!vmas.register_range(region).ok()) {
        return false;
    }
    const mm::Pmm::PhysAddr kFrame = mm::Pmm::self().allocate_page();
    if (kFrame == mm::Pmm::PhysAddr{}) {
        return false;
    }
    auto* const kPage = cinux::base::PtrAt<void>(mm::DirectMapVirt(kFrame.raw));
    cinux::base::SetBytes(kPage, 0, cinux::arch::page::kSize);
    constexpr unsigned char kStub[] = {0xB8, 0x0F, 0x00, 0x00, 0x00, 0x0F, 0x05};
    cinux::base::CopyBytes(kPage, kStub, sizeof(kStub));
    if (!space.map_user(SignalTrampolineAddress(), kFrame.raw, cinux::arch::page::kUser)) {
        mm::Pmm::self().free_page(kFrame);
        return false;
    }
    return true;
}

}  // namespace cinux::proc
