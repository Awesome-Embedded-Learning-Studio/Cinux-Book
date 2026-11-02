#include <stdint.h>

#include "cinux/bit_ops/bitmask.hpp"
#include "cinux/byte_order.hpp"
#include "kernel/driver/usb/usb_tablet.hpp"

namespace cinux::driver {

TabletReport DecodeTablet(const unsigned char* report) {
    return TabletReport{
        .buttons = base::bit::BitMask<uint8_t>{report[0]}.extract({.low = 0, .width = 3}),
        .x       = base::ReadWord16(report, 1),
        .y       = base::ReadWord16(report, 3),
    };
}

}
