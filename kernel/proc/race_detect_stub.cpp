/**
 * @file kernel/proc/race_detect_stub.cpp
 * @brief Empty stub for CINUX_RACE_DETECT=OFF (§14 file gate)
 *
 * RACE_TOUCH and lockdep_assert_held compile to no-ops via the macros in
 * race_detect.hpp when CINUX_RACE_DETECT / CINUX_LOCKDEP are unset, so nothing
 * references these symbols in an OFF build.  They exist only to satisfy the
 * linker if a translation unit calls race_check_access[_probe] directly (it
 * should not -- tests gate on CINUX_RACE_DETECT too).
 */

#include "kernel/proc/race_detect.hpp"

namespace cinux::proc {

bool race_check_access_probe(RaceWatchpoint& /*w*/) {
    return false;
}

void race_check_access(RaceWatchpoint& /*w*/) {}

}  // namespace cinux::proc
