#include "kernel/syscall/sys_getppid.hpp"

#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

long long HandleGetppid(Word /*first*/, Word /*second*/, Word /*third*/) {
    return static_cast<long long>(CurrentTask()->ppid());
}

}  // namespace cinux::syscall
