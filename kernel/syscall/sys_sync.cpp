#include "kernel/syscall/sys_sync.hpp"

#include "kernel/fs/file_world.hpp"
#include "kernel/syscall/syscall.hpp"

namespace cinux::syscall {

long long HandleSync(const SyscallFrame& /*frame*/) {
    return fs::FileWorld::self().sync() ? 0 : -kEio;
}

}
