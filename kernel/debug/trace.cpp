#include "kernel/debug/trace.hpp"

#include "cinux/ptr.hpp"
#include "kernel/boot/print.hpp"
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
        const auto* slot = cinux::base::PtrAt<const unsigned long long>(frame);
        cinux::print::Println("[kern]   #%u 0x%X", depth, slot[1]);
        frame = slot[0];
    }
}

}  // namespace cinux::debug
