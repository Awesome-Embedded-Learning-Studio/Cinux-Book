/**
 * @file kernel/syscall/sys_pipe.cpp
 * @brief sys_pipe handler implementation
 *
 * Creates a Pipe on the kernel heap, wraps each end in an Inode with
 * the appropriate PipeReadOps / PipeWriteOps, allocates two fd slots
 * in the global FDTable, and writes the two descriptor numbers into
 * a user-space int[2] array.
 */

#include "kernel/syscall/sys_pipe.hpp"

#include <stdint.h>

#include "kernel/errno.hpp"
#include "kernel/fs/file.hpp"
#include "kernel/fs/vfs_mount.hpp"
#include "kernel/ipc/pipe.hpp"
#include "kernel/ipc/pipe_ops.hpp"
#include <memory>

namespace cinux::syscall {

namespace {

/**
 * @brief Validate a user-space virtual address
 *
 * Checks the canonical-address rules for x86_64 user space:
 *   - bit 47 must equal bits 48..63
 *   - bit 47 == 0 and upper == 0  =>  user-space low half
 *   - bit 47 == 1 and upper == 0xFFFF  =>  kernel-space (rejected)
 *
 * @param addr  The virtual address to validate
 * @return true if the address looks like a valid user-space pointer
 */
bool is_user_addr(uint64_t addr) {
    if (addr == 0) {
        return false;
    }
    uint64_t bit47 = (addr >> 47) & 1;
    uint64_t upper = addr >> 48;
    if (bit47 == 0 && upper != 0) {
        return false;
    }
    // Reject kernel-space addresses (bit 47 set)
    if (bit47 == 1) {
        return false;
    }
    return true;
}

}  // anonymous namespace

int64_t sys_pipe(uint64_t pipefd_virt, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    // Step 1: Validate the user-space pointer to the int[2] array
    if (!is_user_addr(pipefd_virt)) {
        return -kEfault;
    }

    // Steps 2-4: Build the pipe graph. Each allocation is owned by a UniquePtr
    // so any early return below auto-frees everything allocated so far. On the
    // success path we release() all five: FDTable's File entries then keep raw
    // references to the inodes for the pipe's lifetime (closing a pipe fd frees
    // the File but not the inode/ops/Pipe -- a known hobby-OS limitation).
    std::unique_ptr<cinux::ipc::Pipe>        pipe(new cinux::ipc::Pipe());
    std::unique_ptr<cinux::ipc::PipeReadOps> read_ops(
        new cinux::ipc::PipeReadOps(pipe.get()));
    std::unique_ptr<cinux::ipc::PipeWriteOps> write_ops(
        new cinux::ipc::PipeWriteOps(pipe.get()));

    std::unique_ptr<cinux::fs::Inode> read_inode(new cinux::fs::Inode());
    read_inode->ops  = read_ops.get();
    read_inode->type = cinux::fs::InodeType::Regular;

    std::unique_ptr<cinux::fs::Inode> write_inode(new cinux::fs::Inode());
    write_inode->ops  = write_ops.get();
    write_inode->type = cinux::fs::InodeType::Regular;

    // Step 5: Allocate two fd slots. read_inode stays owned until BOTH fds
    // succeed, so a write-fd failure can still free it after undoing the
    // read-fd allocation (close() frees the File but not the inode).
    auto& table = cinux::fs::current_fd_table();

    int read_fd = table.alloc(read_inode.get(), cinux::fs::OpenFlags::RDONLY);
    if (read_fd < 0) {
        return -kEmfile;  // UniquePtrs free pipe/ops/both inodes
    }

    int write_fd = table.alloc(write_inode.get(), cinux::fs::OpenFlags::WRONLY);
    if (write_fd < 0) {
        table.close(read_fd);  // undo the read-fd allocation (frees its File)
        return -kEmfile;       // UniquePtrs free pipe/ops/both inodes
    }

    // Step 6: Success -- hand off ownership to FDTable's File entries.
    pipe.release();
    read_ops.release();
    write_ops.release();
    read_inode.release();
    write_inode.release();

    auto* pipefd = reinterpret_cast<int32_t*>(pipefd_virt);
    pipefd[0]    = read_fd;
    pipefd[1]    = write_fd;

    return 0;
}

}  // namespace cinux::syscall
