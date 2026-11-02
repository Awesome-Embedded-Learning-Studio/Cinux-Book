#include "kernel/fs/devfs/fb_node.hpp"

#include <cstdint>
#include <span>
#include <string_view>

#include "cinux/assert.hpp"
#include "cinux/container/name_table.hpp"
#include "cinux/math.hpp"
#include "cinux/memory.hpp"
#include "cinux/result.hpp"
#include "cinux/text.hpp"
#include "kernel/arch/x86_64/page.hpp"
#include "kernel/boot/boot_info.hpp"
#include "kernel/console/screen.hpp"
#include "kernel/driver/device/device.hpp"
#include "kernel/driver/framebuffer/framebuffer.hpp"
#include "kernel/driver/framebuffer/framebuffer_config.hpp"
#include "kernel/fs/devfs/devfs.hpp"
#include "kernel/mm/hardware_frames.hpp"
#include "kernel/syscall/syscall.hpp"

namespace cinux::fs {

namespace {

using driver::Framebuffer;

/// The fixed record's fb_type: packed pixels.
constexpr uint32_t kFbTypePacked = 0;

/// The fixed record's fb_visual: direct color values, no palette.
constexpr uint32_t kFbVisualTrue = 2;

/// Dress the two fbdev geometry records from one layout; every word the
/// records carry that the layout cannot name lands at its honest zero,
/// and the identity copies at most fifteen bytes.
void fill_geometry(const cinux::boot::FramebufferInfo& from, const char* identity,
                   driver::FbVarScreeninfoWire& var, driver::FbFixScreeninfoWire& fix) {
    var                = driver::FbVarScreeninfoWire{};
    var.xres           = from.width;
    var.yres           = from.height;
    var.xres_virtual   = from.width;
    var.yres_virtual   = from.height;
    var.bits_per_pixel = from.bpp;
    var.red.offset     = from.red_shift;
    var.red.length     = from.red_size;
    var.green.offset   = from.green_shift;
    var.green.length   = from.green_size;
    var.blue.offset    = from.blue_shift;
    var.blue.length    = from.blue_size;

    fix = driver::FbFixScreeninfoWire{};
    const unsigned long kNamed =
        identity == nullptr ? 0UL : base::StrLength(identity, sizeof(fix.id) + 1U);
    base::CopyText(std::span<char>{fix.id, sizeof(fix.id)}, std::string_view{identity, kNamed});
    fix.smem_start  = from.physical;
    fix.smem_len    = from.pitch * from.height;
    fix.type        = kFbTypePacked;
    fix.visual      = kFbVisualTrue;
    fix.line_length = from.pitch;
}

/// Answers the changeable-geometry question: the stage word points at a
/// kernel-side copy of the record.
long long answer_var_screeninfo(unsigned long long argument) {
    if (!Framebuffer::self().alive()) {
        return -syscall::kEnodev;
    }
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto* const                 kRecord = reinterpret_cast<driver::FbVarScreeninfoWire*>(argument);
    driver::FbVarScreeninfoWire var{};
    driver::FbFixScreeninfoWire fix{};
    fill_geometry(Framebuffer::self().layout(), "CinuxFB", var, fix);
    *kRecord = var;
    return 0;
}

/// Answers the fixed-geometry question the same way.
long long answer_fix_screeninfo(unsigned long long argument) {
    if (!Framebuffer::self().alive()) {
        return -syscall::kEnodev;
    }
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto* const                 kRecord = reinterpret_cast<driver::FbFixScreeninfoWire*>(argument);
    driver::FbVarScreeninfoWire var{};
    driver::FbFixScreeninfoWire fix{};
    fill_geometry(Framebuffer::self().layout(), "CinuxFB", var, fix);
    *kRecord = fix;
    return 0;
}

/// One framebuffer question and its answer.
struct FbAnswer {
    unsigned long long word;
    long long (*answer)(unsigned long long argument);
};

constexpr FbAnswer kFbAnswers[] = {
    {.word = driver::kFbGetVarScreeninfo, .answer = answer_var_screeninfo},
    {.word = driver::kFbGetFixScreeninfo, .answer = answer_fix_screeninfo},
};

/// Answers the two geometry questions; everything else is nobody's.
long long fb_ioctl(void* /*context*/, unsigned long long word, unsigned long long argument) {
    const FbAnswer* const kAnswer = cinux::base::container::FindByWord(kFbAnswers, word);
    if (kAnswer == nullptr) {
        return -syscall::kEnotty;
    }
    return kAnswer->answer(argument);
}

/// Bytes out of the frame are the mapping's story, not a stream's: the
/// device answers end-of-file to a plain read.
base::Result<unsigned long> fb_read(void* /*context*/, void* /*sink*/, unsigned long /*count*/) {
    return 0UL;
}

/// A plain write has no home either; the mapping is the only pen.
base::Result<unsigned long> fb_write(void* /*context*/, const void* /*source*/,
                                     unsigned long count) {
    static_cast<void>(count);
    return base::KernelError::kNotSupported;
}

/// Names the hardware frame one mapping offset shows.
base::Result<unsigned long> fb_map_frame(void* /*context*/, unsigned long long offset) {
    if (!Framebuffer::self().alive()) {
        return base::KernelError::kInvalidArgument;
    }
    const unsigned long kExtent = Framebuffer::self().frame_bytes();
    const auto          kWanted = static_cast<unsigned long>(offset);
    if (kWanted >= cinux::base::math::AlignUp(kExtent, cinux::arch::page::kSize)) {
        return base::KernelError::kInvalidArgument;
    }
    const unsigned long kBase = cinux::base::math::AlignDown(
        static_cast<unsigned long>(Framebuffer::self().layout().physical),
        cinux::arch::page::kSize);
    return kBase + kWanted;
}

/// One independent open took the frame: the text screen steps aside.
void fb_retain(void* /*context*/) {
    console::TextConsole::self().yield_frame();
}

/// The last descriptor gave the frame back: the text screen resumes.
void fb_release(void* /*context*/) {
    console::TextConsole::self().reclaim_frame();
}

const DeviceOps kFbOps{
    .read      = fb_read,
    .write     = fb_write,
    .has_input = nullptr,
    .ioctl     = fb_ioctl,
    .map_frame = fb_map_frame,
    .release   = fb_release,
    .context   = nullptr,
    .retain    = fb_retain,
};

}  // namespace

void RegisterFramebufferDevice() {
    base::safety::Check(Framebuffer::self().alive(), "framebuffer node has no live screen");
    const unsigned long kExtent =
        cinux::base::math::AlignUp(Framebuffer::self().frame_bytes(), cinux::arch::page::kSize);
    const bool kReserved = mm::ReserveHardwarePages(
        cinux::base::math::AlignDown(
            static_cast<unsigned long>(Framebuffer::self().layout().physical),
            cinux::arch::page::kSize),
        kExtent / cinux::arch::page::kSize);
    base::safety::Check(kReserved, "hardware frame registry refused the framebuffer span");
    const base::Result<void> kSeated = driver::DeviceModel::self().seat({
        .klass       = driver::DeviceClass::kCharacter,
        .name        = "fb0",
        .number      = MakeDeviceId(29, 0),
        .ops         = &kFbOps,
        .bus         = "virtual",
        .driver_name = "framebuffer",
    });
    base::safety::Check(kSeated.ok(), "device model refused the fb0 seat");
}

}  // namespace cinux::fs
