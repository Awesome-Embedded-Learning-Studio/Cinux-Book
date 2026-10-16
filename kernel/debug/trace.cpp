#include "kernel/debug/trace.hpp"

#include "cinux/ptr.hpp"
#include "kernel/boot/print.hpp"
#include "kernel/debug/kallsyms.hpp"
#include "kernel/debug/trace_config.hpp"
#include "kernel/mm/layout.hpp"

namespace cinux::debug {

namespace {

bool link_plausible(unsigned long long frame_base) {
    return frame_base != 0 && (frame_base & 0xFULL) == 0 &&
           cinux::mm::IsKernelHalf(static_cast<unsigned long>(frame_base));
}

}  // namespace

void TraceFrom(unsigned long long frame_base) {
    cinux::print::Println("[kern]  trace:");
    unsigned long long frame = frame_base;
    for (unsigned int depth = 0; depth < kMaxTraceFrames; ++depth) {
        if (!link_plausible(frame)) {
            break;
        }
        const auto*              slot   = cinux::base::PtrAt<const unsigned long long>(frame);
        const unsigned long long kRet   = slot[1];
        unsigned long long       offset = 0;
        const char* const        kName  = KallsymsLookup(kRet, offset);
        if (kName != nullptr) {
            cinux::print::Println("[kern]   #%u %s+0x%X (at 0x%X)", depth, kName, offset, kRet);
        } else {
            cinux::print::Println("[kern]   #%u 0x%X", depth, kRet);
        }
        frame = slot[0];
    }
}

}  // namespace cinux::debug
