/**
 * @file    tick.cpp
 * @brief   The one instance of the tick service.
 *
 * Compiled into the no-SSE island: the IRQ entry reaches on_interrupt,
 * and everything an iret returns through must keep its hands off the
 * vector registers.
 *
 * @author  Charliechen114514
 * @date    2026-10-02
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_time
 * @copyright Copyright (c) 2026
 */

#include "kernel/time/tick.hpp"

namespace cinux::time {

Tick& Tick::self() {
    static Tick local_tick;
    return local_tick;
}

}  // namespace cinux::time
