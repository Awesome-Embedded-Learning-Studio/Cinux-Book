#include "kernel/syscall/sys_getpid.hpp"

#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

long long HandleGetpid(Word /*first*/, Word /*second*/, Word /*third*/) {
    return static_cast<long long>(CurrentTask()->pid);
}

}  // namespace cinux::syscall
