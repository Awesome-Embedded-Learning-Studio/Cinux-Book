#include "kernel/mm/hardware_frames.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <span>

#include "kernel/arch/x86_64/page.hpp"

namespace cinux::mm {
namespace {
struct Span {
    unsigned long base = 0;
    unsigned long past = 0;
};
class HardwareFrames {
public:
    static HardwareFrames& self() {
        static HardwareFrames frames;
        return frames;
    }
    bool reserve(unsigned long first, unsigned long pages) {
        constexpr auto kPage = arch::page::kSize;
        if (pages == 0 || first % kPage != 0 ||
            pages > (std::numeric_limits<unsigned long>::max() - first) / kPage ||
            seated_ == spans_.size()) {
            return false;
        }
        const auto kPast = first + (pages * kPage);
        if (std::ranges::any_of(occupied(), [first, kPast](const Span& span) {
                return first < span.past && kPast > span.base;
            })) {
            return false;
        }
        spans_[seated_++] = {.base = first, .past = kPast};
        return true;
    }
    [[nodiscard]] bool contains(unsigned long physical) const {
        return std::ranges::any_of(occupied(), [physical](const Span& span) {
            return physical >= span.base && physical < span.past;
        });
    }

private:
    [[nodiscard]] std::span<const Span> occupied() const {
        return std::span{spans_}.first(seated_);
    }
    std::array<Span, 4> spans_{};
    unsigned long       seated_ = 0;
};
}
bool ReserveHardwarePages(unsigned long physical_base, unsigned long page_count) {
    return HardwareFrames::self().reserve(physical_base, page_count);
}
bool IsHardwarePage(unsigned long physical) {
    return HardwareFrames::self().contains(physical);
}
}
