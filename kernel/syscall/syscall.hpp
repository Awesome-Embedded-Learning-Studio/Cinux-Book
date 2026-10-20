/**
 * @file    syscall.hpp
 * @brief   The numbered door ring 3 knocks on.
 *
 * Numbers follow Linux x86_64, registers follow Linux x86_64, errors
 * come back as negative errno — the three conventions a future musl
 * (and through it, any rootfs) assumes without asking. The entry
 * trampoline lives in syscall.S; this file is the table it dispatches
 * into and the frame contract both sides must agree on. Handlers
 * receive raw words and translate for themselves: the kernel does not
 * trust a user pointer until it has been checked against the user
 * half.
 *
 * @author  Charliechen114514
 * @date    2026-10-07
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_syscall
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace cinux::syscall {

/**
 * @brief   Syscall numbers, matching Linux x86_64.
 * @note    Matching the host ABI of busybox-land is the whole point:
 *          a static musl binary issues these numbers verbatim.
 * @since   0.1.0
 * @ingroup kernel_syscall
 */
// NOLINTNEXTLINE(performance-enum-size)
enum class SyscallNr : std::uint16_t {
    kRead          = 0,    ///< read(fd, buf, count)
    kWrite         = 1,    ///< write(fd, buf, count)
    kOpen          = 2,    ///< open(path, flags)
    kClose         = 3,    ///< close(fd)
    kStat          = 4,    ///< stat(path, out): one node's metadata
    kLstat         = 6,    ///< lstat(path, out): the same promise — no links exist to follow
    kMmap          = 9,    ///< mmap(hint, len, prot, flags, fd, offset)
    kMprotect      = 10,   ///< mprotect(addr, len, prot)
    kMunmap        = 11,   ///< munmap(addr, len)
    kBrk           = 12,   ///< brk(target): returns the break, not an errno
    kRtSigaction   = 13,   ///< rt_sigaction(sig, act, oact, size): install a handler
    kRtSigprocmask = 14,   ///< rt_sigprocmask(how, set, oldset, size): move the mask
    kRtSigreturn   = 15,   ///< rt_sigreturn(): the trampoline's way home
    kYield         = 24,   ///< cooperative yield to the scheduler
    kGetpid        = 39,   ///< getpid(): the caller's own identity
    kClone         = 56,   ///< clone(flags, stack, ...): fork with a say in what stays shared
    kFork          = 57,   ///< fork(): one call, two returns
    kVfork         = 58,   ///< vfork(): fork held until exec — served as plain fork
    kExecve        = 59,   ///< execve(path): replace this world with a program
    kExit          = 60,   ///< exit(code), never returns
    kExitGroup     = 231,  ///< exit_group(code): the whole task group leaves at once
    kWait4         = 61,   ///< wait4(pid, status): block for one child
    kKill          = 62,   ///< kill(pid, sig): deliver a signal to one task
    kSetpgid       = 109,  ///< setpgid(pid, pgid): move a task into a group
    kSetsid        = 112,  ///< setsid(): become a session of one's own
    kGetpgid       = 121,  ///< getpgid(pid): read a task's group
    kGetsid        = 124,  ///< getsid(pid): read a task's session
    kGetppid       = 110,  ///< getppid(): who the caller may be waited on by
    kGetcwd        = 79,   ///< getcwd(buf, size): the caller's working directory
    kChdir         = 80,   ///< chdir(path): move the caller's working directory
    kReboot        = 169,  ///< reboot(): power the machine off through ACPI
    kMkdir         = 83,   ///< mkdir(path)
    kUnlink        = 87,   ///< unlink(path)
    kGetdents      = 217,  ///< getdents(fd, entry): one directory entry per call
    kArchPrctl     = 158,  ///< arch_prctl(op, addr): move a thread-pointer base
    kGettid        = 186,  ///< gettid(): the caller's own thread identity
    kFutex         = 202,  ///< futex(uaddr, op, val): park or wake on a user word
    kSetTidAddress = 218,  ///< set_tid_address(addr): name the word to clear at death
    kFstat         = 5,    ///< fstat(fd, out): one open's metadata
    kIoctl         = 16,   ///< ioctl(fd, cmd, arg): device questions
    kWritev        = 20,   ///< writev(fd, iov, count): scatter write in one call
    kAccess        = 21,   ///< access(path, mode): does the path answer
    kPipe          = 22,   ///< pipe(fds): one ring, read end first
    kDup           = 32,   ///< dup(fd): the same open, lowest free number
    kDup2          = 33,   ///< dup2(fd, target): the same open at target
    kPipe2         = 293,  ///< pipe2(fds, flags): the ring, zero flags only
    kUname         = 63,   ///< uname(buf): name the system to the caller
    kFcntl         = 72,   ///< fcntl(fd, cmd, arg): descriptor knobs
    kGettimeofday  = 96,   ///< gettimeofday(tv, tz): wall clock, coarse
    kGetuid        = 102,  ///< getuid(): the caller's user id
    kGetgid        = 104,  ///< getgid(): the caller's group id
    kSetuid        = 105,  ///< setuid(uid): adopt a user id
    kSetgid        = 106,  ///< setgid(gid): adopt a group id
    kGeteuid       = 107,  ///< geteuid(): the effective user id
    kGetegid       = 108,  ///< getegid(): the effective group id
    kPrctl         = 157,  ///< prctl(op, ...): process knobs
    kTime          = 201,  ///< time(tloc): seconds since the epoch
    kClockGettime  = 228,  ///< clock_gettime(clk, tp): seconds and nanoseconds
    kPrlimit64     = 302,  ///< prlimit64(pid, res, new, old): resource ceilings
    kGetrandom     = 318,  ///< getrandom(buf, len, flags): bytes no one can predict
    kOpenat        = 257,  ///< openat(dirfd, path, flags): open, relative to a descriptor
    kNewfstatat    = 262,  ///< newfstatat(dirfd, path, out): stat, relative to a descriptor
    kPpoll         = 271,  ///< ppoll(ufds, nfds, tmo, mask): answered no-events
    kPoll          = 7,    ///< poll(ufds, nfds, timeout_ms): ppoll's older spelling.
    kReadlink      = 89,   ///< readlink(path, buf, size): what the one link stands for.
    kSysinfo       = 99,   ///< sysinfo(info): the machine's signs in one record.
    kNanosleep     = 35,   ///< nanosleep(asked, remain): park until the clock says.
    kTgkill        = 200,  ///< tgkill(tgid, tid, sig): raise's own spelling.
};

/// Linux errno values the handlers can produce.
inline constexpr int kEperm        = 1;   ///< The caller may not do this to the target.
inline constexpr int kEnoent       = 2;   ///< No such file or directory.
inline constexpr int kEsrch        = 3;   ///< No such task.
inline constexpr int kEintr        = 4;   ///< The call was interrupted by a signal.
inline constexpr int kEio          = 5;   ///< The volume read came back short.
inline constexpr int kE2big        = 7;   ///< The argument list does not fit the entry page.
inline constexpr int kEnoexec      = 8;   ///< The image is not one this kernel can run.
inline constexpr int kEbadf        = 9;   ///< Not a descriptor the kernel serves.
inline constexpr int kEchild       = 10;  ///< The wait names no child of the caller.
inline constexpr int kEagain       = 11;  ///< Try again; nothing is ready right now.
inline constexpr int kEnomem       = 12;  ///< The kernel has no room for this.
inline constexpr int kEfault       = 14;  ///< The buffer does not live in the user half.
inline constexpr int kEexist       = 17;  ///< The name already exists.
inline constexpr int kEnotdir      = 20;  ///< A path component is a file.
inline constexpr int kEisdir       = 21;  ///< A file-only operation met a directory.
inline constexpr int kEinval       = 22;  ///< Unusable argument.
inline constexpr int kEmfile       = 24;  ///< The descriptor table has no room left.
inline constexpr int kEnotty       = 25;  ///< Not a terminal.
inline constexpr int kEpipe        = 32;  ///< The pipe's reader is gone.
inline constexpr int kErange       = 34;  ///< The output buffer is too small.
inline constexpr int kEnametoolong = 36;  ///< The folded path overflows its buffer.
inline constexpr int kEnosys       = 38;  ///< No such syscall at this number.
inline constexpr int kEnotempty    = 39;  ///< The directory still holds entries.

/// open() flag the kernel serves: create the file when it is missing.
inline constexpr unsigned long long kOpenCreat = 0x40;

/// open() status bits, the face fcntl's F_GETFL answers and F_SETFL
/// re-arms: the access mode names the open's read/write shape, the
/// rest ride the open for its whole life.
inline constexpr unsigned long long kOpenAccMode   = 0x3;
inline constexpr unsigned long long kOpenReadWrite = 2;
inline constexpr unsigned long long kOpenAppend    = 0x400;
inline constexpr unsigned long long kOpenNonblock  = 0x800;
inline constexpr unsigned long long kOpenDirect    = 0x4000;
inline constexpr unsigned long long kOpenNoatime   = 0x40000;

/// open() flag kept off the status face: it arms the descriptor
/// seat's close-on-exec bit and never answers through F_GETFL.
inline constexpr unsigned long long kOpenCloexec = 0x80000;

/// The whole status face one open carries: the access mode plus every
/// bit F_SETFL may re-arm.
inline constexpr unsigned long long kOpenStatusMask =
    kOpenAccMode | kOpenAppend | kOpenNonblock | kOpenDirect | kOpenNoatime;

/// clone() flags, Linux-shaped. The served four overlay the fork
/// child; the tid-word trio arms the join handshake the death side
/// of futex answers.
inline constexpr unsigned long long kCloneVm     = 0x100;    ///< Share the address space.
inline constexpr unsigned long long kCloneFiles  = 0x400;    ///< Share the descriptor table.
inline constexpr unsigned long long kCloneThread = 0x10000;  ///< Join the maker's thread group.
inline constexpr unsigned long long kCloneSetTls = 0x80000;  ///< Arm the child's thread pointer.
inline constexpr unsigned long long kCloneParentSetTid =
    0x100000;  ///< Write the child tid for the maker.
inline constexpr unsigned long long kCloneChildClearTid =
    0x200000;  ///< Clear a word at child death.
inline constexpr unsigned long long kCloneChildSetTid =
    0x1000000;  ///< Write the child tid for the child.

/// mmap protection bits, Linux-shaped.
inline constexpr unsigned long long kProtRead  = 0x1;  ///< Loads through the range promised.
inline constexpr unsigned long long kProtWrite = 0x2;  ///< Stores through the range promised.
inline constexpr unsigned long long kProtExec  = 0x4;  ///< Running code from the range promised.

/// mmap flags. Private anonymous is the shape the kernel serves; a fixed
/// address is refused rather than half-honored.
inline constexpr unsigned long long kMapFixed     = 0x10;
inline constexpr unsigned long long kMapAnonymous = 0x20;

/// One past the highest served number; the dispatch table spans it.
constexpr unsigned long long kSyscallCount = 319;

/// The lowest address that is none of ring 3's business.
constexpr unsigned long long kUserAddressLimit = 0x0000800000000000ULL;

/**
 * @brief   One packed directory record's byte plan, the hosted shape
 *          getdents fills.
 * @note    Linux dirent64 laid out by offset, not by a C struct: the
 *          natural alignment of a struct would pad the nineteen-byte
 *          head to twenty-four, so the writer places fields by these
 *          offsets instead — inode at zero, the resume cookie at
 *          eight, the record's own length at sixteen, a type word at
 *          eighteen, and the NUL-terminated name from nineteen on.
 *          Records abut inside the caller's buffer, each whole length
 *          eight-aligned; the cookie echoes the ordinal this kernel
 *          reads directories by, one past the entry it names.
 * @since   0.3.0
 * @ingroup kernel_syscall
 */
/// Where the inode number sits in one packed record.
inline constexpr unsigned long kDirent64InoAt  = 0;
/// Where the resume cookie sits in one packed record.
inline constexpr unsigned long kDirent64OffAt  = 8;
/// Where the record's own length sits in one packed record.
inline constexpr unsigned long kDirent64LenAt  = 16;
/// Where the type word sits in one packed record.
inline constexpr unsigned long kDirent64TypeAt = 18;
/// Where the NUL-terminated name starts in one packed record.
inline constexpr unsigned long kDirent64NameAt = 19;

/// getdents' type word for a regular file, the hosted DT_REG.
inline constexpr unsigned char kDirentTypeFile = 8;

/// getdents' type word for a directory, the hosted DT_DIR.
inline constexpr unsigned char kDirentTypeDirectory = 4;

/// getdents' type word for a character device, the hosted DT_CHR.
inline constexpr unsigned char kDirentTypeChar = 2;

/**
 * @brief   One node's metadata, as stat hands it to ring 3.
 * @note    Linux x86_64 stat layout. FsStat is an internal summary;
 *          libc consumes this full record at fixed byte offsets.
 * @since   0.3.0
 * @ingroup kernel_syscall
 */
struct SyscallStat {
    std::uint64_t dev;    ///< Device identity, zero until backends expose it.
    std::uint64_t ino;    ///< Inode number.
    std::uint64_t links;  ///< Directory entries naming this node.
    std::uint32_t mode;   ///< Type and permission bits, as stored.
    std::uint32_t uid;    ///< Owner, zero in the current single-user system.
    std::uint32_t gid;    ///< Group, zero in the current single-user system.
    std::uint32_t padding;
    std::uint64_t rdev;
    std::int64_t  size;     ///< File bytes; directories report their block bytes.
    std::int64_t  blksize;  ///< Preferred I/O size, zero when unspecified.
    std::int64_t  blocks;   ///< Allocated 512-byte units, not yet exposed by FsStat.
    std::uint64_t atime;
    std::uint64_t atime_nsec;
    std::uint64_t mtime;  ///< Modification time, Unix seconds.
    std::uint64_t mtime_nsec;
    std::uint64_t ctime;
    std::uint64_t ctime_nsec;
    std::int64_t  reserved[3];
};

static_assert(sizeof(SyscallStat) == 144, "Linux x86_64 stat is 144 bytes");
static_assert(offsetof(SyscallStat, ino) == 8, "stat inode sits at +8");
static_assert(offsetof(SyscallStat, links) == 16, "stat link count sits at +16");
static_assert(offsetof(SyscallStat, mode) == 24, "stat mode is a 32-bit word at +24");
static_assert(offsetof(SyscallStat, size) == 48, "stat size sits at +48");
static_assert(offsetof(SyscallStat, mtime) == 88, "stat mtime sits at +88");
static_assert(offsetof(SyscallStat, reserved) == 120, "stat reserved words sit at +120");

/**
 * @brief   The frame the entry trampoline builds on the kernel stack.
 * @note    Offset order is the contract with syscall.S; the asserts
 *          pin the layout so C++ and assembly cannot drift. user_rsp
 *          rides at the top so the exit path can rebuild RSP from the
 *          frame itself — gs:8 only stages it on the way in, because
 *          another ring-3 task's entry would overwrite it in between.
 *          Every general register rides the frame since the signal
 *          station: a handler delivery rewrites the whole image and
 *          sends the task back to ring 3 from here, so the frame is
 *          the task's user register file while it sits in the kernel.
 * @since   0.1.0
 * @ingroup kernel_syscall
 */
struct SyscallFrame {
    unsigned long long user_rsp;  ///< +0,   the caller's stack.
    unsigned long long rcx;       ///< +8,   return rip, banked by SYSCALL.
    unsigned long long r11;       ///< +16,  rflags, banked by SYSCALL.
    unsigned long long rax;       ///< +24,  the syscall number.
    unsigned long long rdi;       ///< +32,  first argument.
    unsigned long long rsi;       ///< +40,  second argument.
    unsigned long long rdx;       ///< +48,  third argument.
    unsigned long long r10;       ///< +56,  fourth argument, unserved yet.
    unsigned long long r8;        ///< +64,  fifth argument, unserved yet.
    unsigned long long r9;        ///< +72,  sixth argument, unserved yet.
    unsigned long long rbx;       ///< +80,  callee-saved, restored on exit.
    unsigned long long rbp;       ///< +88,  callee-saved, restored on exit.
    unsigned long long r12;       ///< +96,  callee-saved, restored on exit.
    unsigned long long r13;       ///< +104, callee-saved, restored on exit.
    unsigned long long r14;       ///< +112, callee-saved, restored on exit.
    unsigned long long r15;       ///< +120, callee-saved, restored on exit.
};

static_assert(offsetof(SyscallFrame, rcx) == 8, "entry banks rcx at +8");
static_assert(offsetof(SyscallFrame, rax) == 24, "the number sits at +24");
static_assert(offsetof(SyscallFrame, rbx) == 80, "the rbx lesson stays pinned");
static_assert(offsetof(SyscallFrame, r12) == 96, "r12 closes the sixteen-word frame");
static_assert(sizeof(SyscallFrame) == 128, "sixteen quadwords of frame");

}  // namespace cinux::syscall

extern "C" {

/**
 * @brief         The LSTAR target: ring 3 arrives here on SYSCALL.
 * @since         0.1.0
 * @ingroup       kernel_syscall
 */
void SyscallEntry();

/**
 * @brief         Run one numbered syscall; the C side of the door.
 * @param[in,out] frame   The entry frame: number and every ABI word
 *                        come off it, and its rax is the return slot.
 * @return        The value ring 3 receives in RAX: a count, or a
 *                negative errno on refusal.
 * @note          C linkage for the assembly caller, which hands the
 *                frame over as one pointer. Handlers may switch tasks
 *                inside (yield, exit, blocking read); the frame on
 *                the task's kernel stack is what makes that
 *                survivable.
 * @since         0.1.0
 * @ingroup       kernel_syscall
 */
long long InvokeSyscall(cinux::syscall::SyscallFrame& frame);

}  // extern "C"
