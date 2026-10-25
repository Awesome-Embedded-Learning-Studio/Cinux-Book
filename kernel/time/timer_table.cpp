#include "kernel/time/timer_table.hpp"

#include "cinux/result.hpp"
#include "kernel/time/timer_config.hpp"

namespace cinux::time {

base::Result<TimerTicket, base::KernelError> TimerTable::arm(void* context,
                                                             void (*call)(void* context),
                                                             TimerDeadline deadline_ns) {
    for (unsigned long index = 0; index < kTimerSeats; ++index) {
        Seat& seat = seats_[index];
        if (seat.used) {
            continue;
        }
        const unsigned long kStamp = seat.stamp + 1;
        seat                       = Seat{};
        seat.used                  = true;
        seat.stamp                 = kStamp;
        seat.context               = context;
        seat.call                  = call;
        seat.deadline_ns           = deadline_ns;
        return TimerTicket{.slot = index, .stamp = kStamp};
    }
    return base::KernelError::kOutOfMemory;
}

bool TimerTable::cancel(const TimerTicket& ticket) {
    if (ticket.slot >= kTimerSeats) {
        return false;
    }
    Seat& seat = seats_[ticket.slot];
    if (!seat.used || seat.stamp != ticket.stamp) {
        return false;
    }
    const unsigned long kStamp = seat.stamp + 1;
    seat                       = Seat{};
    seat.stamp                 = kStamp;
    return true;
}

void TimerTable::advance(TimerDeadline now) {
    for (Seat& seat : seats_) {
        if (!seat.used || now < seat.deadline_ns) {
            continue;
        }
        const unsigned long kStamp   = seat.stamp + 1;
        void* const         kContext = seat.context;
        void (*const kCall)(void*)   = seat.call;
        seat                         = Seat{};
        seat.stamp                   = kStamp;
        kCall(kContext);
    }
}

}  // namespace cinux::time
