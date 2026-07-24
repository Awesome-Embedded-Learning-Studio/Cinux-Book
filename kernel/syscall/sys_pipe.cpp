/**
 * @file kernel/syscall/sys_pipe.cpp
 * @brief sys_pipe handler (P0e SMAP-layered)
 *
 * Layered (Linux-aligned):
 *   - do_pipe_kernel: builds the pipe graph, allocates two fds, writes them
 *     into a KERNEL int[2]. Pure kernel-to-kernel; tests call this.
 *   - sys_pipe: the user boundary. do_* fills the kernel int[2], then
 *     copy_to_user stages it out. The old `pipefd[0]=...; pipefd[1]=...` raw
 *     writes into the user array are gone; is_user_addr is replaced by
 *     copy_to_user's access_ok.
 */

#include "kernel/syscall/sys_pipe.hpp"

#include <stdint.h>

#include <memory>

#include "kernel/arch/x86_64/user_access.hpp"  // P0e (SMAP): copy_to_user
#include "kernel/errno.hpp"
#include "kernel/fs/file.hpp"
#include "kernel/fs/vfs_mount.hpp"
#include "kernel/ipc/pipe.hpp"
#include "kernel/ipc/pipe_ops.hpp"

namespace cinux::syscall {

int64_t do_pipe_kernel(cinux::fs::FDTable& tbl, int* pipefd_kernel) {
    // Build the pipe graph. Each allocation is owned by a UniquePtr so any
    // early return auto-frees everything allocated so far. On success release()
    // all five: FDTable's File entries then keep raw references to the inodes
    // for the pipe's lifetime (closing a pipe fd frees the File but not the
    // inode/ops/Pipe -- a known hobby-OS limitation).
    std::unique_ptr<cinux::ipc::Pipe>         pipe(new cinux::ipc::Pipe());
    std::unique_ptr<cinux::ipc::PipeReadOps>  read_ops(new cinux::ipc::PipeReadOps(pipe.get()));
    std::unique_ptr<cinux::ipc::PipeWriteOps> write_ops(new cinux::ipc::PipeWriteOps(pipe.get()));

    std::unique_ptr<cinux::fs::Inode> read_inode(new cinux::fs::Inode());
    read_inode->ops  = read_ops.get();
    read_inode->type = cinux::fs::InodeType::Regular;

    std::unique_ptr<cinux::fs::Inode> write_inode(new cinux::fs::Inode());
    write_inode->ops  = write_ops.get();
    write_inode->type = cinux::fs::InodeType::Regular;

    int read_fd = tbl.alloc(read_inode.get(), cinux::fs::OpenFlags::RDONLY);
    if (read_fd < 0) {
        return -kEmfile;  // UniquePtrs free pipe/ops/both inodes
    }
    int write_fd = tbl.alloc(write_inode.get(), cinux::fs::OpenFlags::WRONLY);
    if (write_fd < 0) {
        tbl.close(read_fd);  // undo the read-fd allocation (frees its File)
        return -kEmfile;     // UniquePtrs free pipe/ops/both inodes
    }

    // Success -- hand off ownership to FDTable's File entries.
    pipe.release();
    read_ops.release();
    write_ops.release();
    read_inode.release();
    write_inode.release();

    pipefd_kernel[0] = read_fd;
    pipefd_kernel[1] = write_fd;
    return 0;
}

int64_t sys_pipe(uint64_t pipefd_virt, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    cinux::fs::FDTable& tbl = cinux::fs::current_fd_table();
    int                 pipefd[2];
    int64_t             rc = do_pipe_kernel(tbl, pipefd);
    if (rc < 0) {
        return rc;
    }
    if (!cinux::user::copy_to_user(reinterpret_cast<void*>(pipefd_virt), pipefd, sizeof(pipefd))) {
        // Roll back the two fds do_pipe_kernel just installed: a failed copy
        // must not leak them. On the ring-0 test harness's shared global fd
        // table a leaked fd lands on slot 0/1/2 (no stdin/stdout/stderr
        // reservation) and turns busybox echo's `fflush(stdout)==0?0:1` into
        // exit(1) in the ring-3 smoke. Production also benefits (no fd leak
        // on a failed pipe()).
        tbl.close(pipefd[0]);
        tbl.close(pipefd[1]);
        return -kEfault;
    }
    return 0;
}

}  // namespace cinux::syscall
