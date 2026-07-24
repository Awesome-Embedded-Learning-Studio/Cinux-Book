/**
 * @file kernel/mm/stats_kthread_stub.cpp
 * @brief Empty start_stats_thread() when CINUX_STATS_KTHREAD=OFF (§14 file gate)
 *
 * Source has no #ifdef; CMake links this TU instead of stats_kthread.cpp when
 * the option is off, so init.cpp can call start_stats_thread() unconditionally.
 *
 * Namespace: cinux::mm
 */

#include "kernel/mm/diagnostics.hpp"

namespace cinux::mm {

void start_stats_thread() {}

}  // namespace cinux::mm
