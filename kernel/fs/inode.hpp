/**
 * @file kernel/fs/inode.hpp
 * @brief VFS inode definitions -- the core abstraction for filesystem objects
 *
 * Defines InodeType (regular file, directory, etc.), the InodeOps virtual
 * table for per-inode operations (read, write, readdir), and the Inode
 * struct itself which ties everything together.
 *
 * Each concrete filesystem (ramdisk, ext2, ...) produces Inode instances
 * whose ops pointers point at filesystem-specific implementations.
 *
 * Namespace: cinux::fs
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <cinux/expected.hpp>

#include "fs/stat.hpp"

namespace cinux::proc {
struct Task;  // forward -- poll_events() parks a poller on a fd's wait queue
}

namespace cinux::drivers {
class IBlockDevice;  // forward -- block_device() exposes a backing block device
}

namespace cinux::fs {

// ============================================================
// poll(2) / select(2) event bits (Linux UAPI values)
// ============================================================
// Returned by InodeOps::poll_events() and masked against each pollfd's
// requested @c events to form @c revents.  POLLERR / POLLHUP / POLLNVAL are
// always reported in revents regardless of what the caller requested.
constexpr uint16_t kPollIn     = 0x0001;  ///< POLLIN  (readable: data available)
constexpr uint16_t kPollPri    = 0x0002;  ///< POLLPRI (priority / out-of-band data)
constexpr uint16_t kPollOut    = 0x0004;  ///< POLLOUT (writable: space available)
constexpr uint16_t kPollErr    = 0x0008;  ///< POLLERR (error condition; always reported)
constexpr uint16_t kPollHup    = 0x0010;  ///< POLLHUP (peer hung up; always reported)
constexpr uint16_t kPollNval   = 0x0020;  ///< POLLNVAL (invalid fd; always reported)
constexpr uint16_t kPollRdNorm = 0x0040;  ///< POLLRDNORM (normal data readable == POLLIN)
constexpr uint16_t kPollWrNorm = 0x0100;  ///< POLLWRNORM (normal data writable == POLLOUT)

// ============================================================
// Inode Type Enumeration
// ============================================================

/// Type of filesystem object represented by an inode
enum class InodeType : uint8_t {
    Unknown   = 0,
    Regular   = 1,
    Directory = 2,
    /// F-USABILITY batch 1a: symbolic link. readlink() yields the target;
    /// vfs_lookup() follows it (kernel/fs/vfs_lookup.{hpp,cpp}, batch 1b).
    Symlink   = 3,
};

// ============================================================
// Inode Operations (virtual function table)
// ============================================================

struct Inode;

/**
 * @brief Abstract base class for inode-level operations
 *
 * Each concrete filesystem provides InodeOps subclasses that implement
 * read, write, readdir, create, mkdir, and unlink.  Unsupported
 * operations fall back to the default implementations (returning -1
 * or nullptr).
 */
class InodeOps {
public:
    virtual ~InodeOps() = default;

    virtual cinux::lib::ErrorOr<int64_t> read(const Inode* inode, uint64_t offset, void* buf,
                                              uint64_t count);
    virtual cinux::lib::ErrorOr<int64_t> write(Inode* inode, uint64_t offset, const void* buf,
                                               uint64_t count);
    virtual cinux::lib::ErrorOr<int64_t> readdir(const Inode* inode, uint64_t index, char* name,
                                                 uint64_t name_max);
    virtual cinux::lib::ErrorOr<Inode*>  create(Inode* dir, const char* name, uint32_t namelen);
    virtual cinux::lib::ErrorOr<Inode*>  mkdir(Inode* dir, const char* name, uint32_t namelen);
    virtual cinux::lib::ErrorOr<void>    unlink(Inode* dir, const char* name, uint32_t namelen);
    virtual cinux::lib::ErrorOr<void>    stat(const Inode* inode, struct stat* st);

    /// Set the file length to @p new_size (sys_open O_TRUNC / ftruncate).
    /// Shrink-only for the O_TRUNC case (new_size 0): the backend updates the
    /// on-disk + VFS size; freeing the now-orphaned data blocks is a follow-up
    /// (a hobby-os leak, not a correctness issue -- reads stop at i_size).  The
    /// default returns NotImplemented; only ext2 overrides for now.
    virtual cinux::lib::ErrorOr<void> truncate(Inode* inode, uint64_t new_size);

    /// Device-specific ioctl (terminal ioctls on a PTY/console inode, ...).
    /// @p request is the Linux ioctl request word (TCGETS, TIOCSCTTY, ...);
    /// @p arg the opaque user payload.  The default returns NotImplemented;
    /// sys_ioctl maps that to -ENOTTY, so an inode type that does not override
    /// this simply answers "not a tty ioctl".  Overrides cross the user/kernel
    /// boundary themselves (copy_to/from_user).
    virtual cinux::lib::ErrorOr<int64_t> ioctl(const Inode* inode, uint32_t request, uint64_t arg);

    /// F-GUI-USERSPACE batch 1: device mmap.  sys_mmap consults this hook when a
    /// non-anonymous mapping targets a character device; an override returns the
    /// physical base the VMA must bind to (e.g. the framebuffer's VBE phys
    /// address).  The default returns NotImplemented, so regular files and
    /// non-mmap-able devices fall through to the normal file-backed path or
    /// reject the mmap.  @p offset is the mmap offset (already validated by the
    /// caller); @p length the requested mapping size, which the override may
    /// bound (e.g. clamp to the screen size).
    virtual cinux::lib::ErrorOr<uint64_t> mmap(const Inode* inode, uint64_t offset,
                                               uint64_t length);

    /// Called by sys_open after lookup resolves this inode.  The default returns
    /// the same inode (bind the fd to what lookup found).  A cloning device --
    /// Linux /dev/ptmx is the classic case -- overrides this to allocate a fresh
    /// per-open resource (a PTY pair) and return a distinct inode (the master
    /// end) for the new fd.  @p flags is the raw Linux open() flag word so a
    /// cloning device can honour direction / O_NONBLOCK (a FIFO's open() uses it
    /// to pick the read or write end of the underlying pipe).  Returning an
    /// error fails the open.
    virtual cinux::lib::ErrorOr<Inode*> open(Inode* inode, uint64_t flags);

    // ============================================================
    // F-ECO batch 2: attribute + dirent operations (default-backed).
    //
    // Each of these has a default that returns NotImplemented, exactly like
    // ioctl()/open() above, so the ~20 existing InodeOps subclasses
    // (pipe/pty/devfs/procfs/ramdisk/socket/...) need no change; ext2 is the
    // only backend that overrides them for now. They are added in one shot so
    // the parallel implementation of the syscalls never edits this shared
    // header again.
    // ============================================================

    /// Change the permission bits of an inode (sys_chmod). Only the low 12
    /// permission bits of @p mode take effect; the file-type bits are kept.
    virtual cinux::lib::ErrorOr<void> chmod(Inode* inode, uint32_t mode);

    /// Change the owner of an inode (sys_chown). A @p uid or @p gid of
    /// 0xFFFFFFFF ((uint32_t)-1) leaves that field untouched, per Linux chown(2).
    virtual cinux::lib::ErrorOr<void> chown(Inode* inode, uint32_t uid, uint32_t gid);

    /// Set access / modification times (sys_utimensat). ext2 revision-0 inodes
    /// store whole seconds; the nsec fields are accepted but truncated.
    virtual cinux::lib::ErrorOr<void> utimensat(Inode* inode, uint64_t atime_sec,
                                                uint32_t atime_nsec, uint64_t mtime_sec,
                                                uint32_t mtime_nsec);

    /// Read a symbolic link's target into @p buf (sys_readlink). Returns the
    /// number of bytes written to @p buf (NOT counting a trailing NUL).
    virtual cinux::lib::ErrorOr<int64_t> readlink(const Inode* inode, char* buf, uint64_t buf_size);

    /// Create a symbolic link named @p name in directory @p dir pointing at the
    /// NUL-terminated @p target string (sys_symlink).
    virtual cinux::lib::ErrorOr<void> symlink(Inode* dir, const char* name, uint32_t namelen,
                                              const char* target);

    /// Create a hard link named @p name in directory @p dir referring to @p
    /// target (sys_link): adds a directory entry and bumps the target's nlink.
    virtual cinux::lib::ErrorOr<void> link(Inode* dir, const char* name, uint32_t namelen,
                                           const Inode* target);

    /// Rename entry @p src_name in @p src_dir to @p dst_name in @p dst_dir
    /// (sys_rename). @p dst_dir may equal @p src_dir. Hobby-OS move: removes the
    /// old directory entry and adds a fresh one; no atomic cross-directory swap.
    virtual cinux::lib::ErrorOr<void> rename(Inode* src_dir, const char* src_name,
                                             uint32_t src_namelen, Inode* dst_dir,
                                             const char* dst_name, uint32_t dst_namelen);

    /// Whether reads against this inode should be served through the file-backed
    /// PageCache.  Disk-backed filesystems (ext2) override to true so that
    /// sys_read and demand paging share one cache; transient inode-ops shims
    /// such as pipes inherit the default false (their content is not on disk and
    /// must never be cached).  Default false keeps every legacy/mock backend
    /// unchanged.
    virtual bool is_page_cacheable() const;

    // ============================================================
    // F8-M5: poll(2) / select(2) readiness.  Added in one shot (with a safe
    // default) so the ~20 existing InodeOps subclasses need no change: only the
    // blocking fd types (pipe/FIFO via PipeReadOps/PipeWriteOps, sockets via
    // SocketOps) override it.  Mirrors Linux file_operations->poll as the
    // uniform readiness + wait-registration seam consumed by sys_poll/select.
    // ============================================================

    /// poll/select readiness for this open file.
    ///
    /// Returns the ready event mask (a subset of the @c kPoll* bits above).
    /// sys_poll masks it against each pollfd's requested @c events to form
    /// @c revents (POLLERR/POLLHUP/POLLNVAL are always passed through).
    ///
    /// Wait registration: if @p waiter is non-null, ALSO enqueue it on this fd's
    /// internal wait queue -- atomically with the readiness check, under this
    /// fd's own lock (the prepare_to_wait contract).  A later state change
    /// (bytes arrive / peer closes) then wakes it via Scheduler::unblock.  The
    /// caller follows with poll_detach_waiter() once it no longer waits.
    ///
    /// @param inode     The open file's inode.
    /// @param waiter    The polling task to park (nullptr = readiness check only).
    /// @param registered Out: set true iff @p waiter was actually queued (i.e.
    ///                   this fd is a blocking type that can later wake the
    ///                   poller).  Regular files never register.
    /// @return The ready event mask.
    ///
    /// Default: a regular file is always ready (kPollIn|kPollOut) and never
    /// registers a waiter -- it never blocks, so poll returns immediately.
    virtual uint32_t poll_events(const Inode* inode, cinux::proc::Task* waiter, bool* registered);

    /// Remove a previously-registered @p waiter from this fd's wait queue.
    /// poll calls this for every fd after it wakes (event or timeout) so the
    /// waiter is not left linked in a queue it no longer waits on (which would
    /// spuriously wake a later, unrelated block, or dangle after the task dies).
    /// Default: no-op (regular files never register).
    virtual void poll_detach_waiter(const Inode* inode, cinux::proc::Task* waiter);

    /// Called by FDTable::close when a File bound to this inode is closed -- the
    /// "release" / last-close hook (Linux file_operations->release).  Used to
    /// free per-open protocol resources: a socket unbinds / sends FIN, a pipe
    /// end signals EOF to its peer.  Default: no-op, so regular files, pipes and
    /// FIFOs (whose close-propagation needs end-refcounting -- a separate DEBT)
    /// are unchanged; only fd types that need it override this.  @p inode is
    /// non-null; the inode itself is owned by its filesystem and is NOT freed
    /// here (release only signals "an open description went away").
    virtual void release(Inode* inode);

    /// F6-M1 B1b: backing block device for a block-device inode (e.g. /dev/sda
    /// in DevFs).  sys_mount resolves its source path to an Inode, then calls
    /// this to obtain the IBlockDevice to mount.  Default nullptr -- regular
    /// files, char devices and pseudo-fs nodes have no block-device backing.
    virtual cinux::drivers::IBlockDevice* block_device(const Inode* /*inode*/) {
        return nullptr;
    }
};

// ============================================================
// Inode Structure
// ============================================================

/**
 * @brief Represents a single filesystem object (file, directory, etc.)
 *
 * Inodes are produced by concrete FileSystem backends during lookup().
 * They are owned by the producing filesystem and must not be freed by
 * the caller -- the filesystem manages their lifetime.
 */
struct Inode {
    uint64_t  ino{0};                    ///< Inode number (filesystem-specific)
    uint64_t  size{0};                   ///< File size in bytes
    InodeType type{InodeType::Regular};  ///< Type of this inode
    InodeOps* ops{nullptr};              ///< Operation function table (may be nullptr)
    void*     fs_private{nullptr};       ///< Opaque pointer for filesystem-specific data

    uint32_t mode{0};   ///< File mode (type + permissions)
    uint32_t uid{0};    ///< Owner user ID
    uint32_t gid{0};    ///< Owner group ID
    uint32_t nlink{1};  ///< Hard link count
    /// Open-description reference count (DEBT-023). Bumped once per File that
    /// points at this inode (alloc / dup / dup2 / fork-copy), dropped when such
    /// a File is destroyed. Reaching 0 = last close -> InodeOps::release fires.
    /// Accessed via __atomic_* because File objects in different FDTables (after
    /// fork) on different CPUs share one inode. This is what lets a pipe/FIFO
    /// end emit POLLHUP/EOF only when the LAST referring fd closes -- a dup no
    /// longer trips an early EOF (the bug that broke `ls|grep` pipelines).
    uint32_t refcount{0};
    uint64_t atime{0};   ///< Time of last access
    uint64_t ctime{0};   ///< Time of last status change
    uint64_t mtime{0};   ///< Time of last modification
    uint64_t blocks{0};  ///< Number of 512-byte blocks allocated
};

}  // namespace cinux::fs
