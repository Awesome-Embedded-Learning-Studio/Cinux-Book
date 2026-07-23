target_sources(big_kernel_common PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_read.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_write.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_open.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_close.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_exit.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_yield.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_getdents.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_getdents64.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_creat.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_mkdir.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_mknod.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_unlink.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_rmdir.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/path_util.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_chdir.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_getcwd.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_stat.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_pipe.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_getpid.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_getppid.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_fork.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_clone.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_execve.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_waitpid.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_dmesg.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_mmap.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_brk.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_signal.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_futex.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_pgrp.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_creds.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_lseek.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_ioctl.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_iov.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_arch_prctl.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_set_tid_address.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_clock_gettime.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_ping.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_socket.cpp
    # F-ECO batch 2: VFS metadata + dirent syscalls.
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_chmod.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_chown.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_link.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_readlink.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_rename.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_symlink.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_umask.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_utimensat.cpp
    # F-ECO batch 3: nanosleep (sleep).
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_nanosleep.cpp
    # F-ECO batch 4: dup / dup2 / fcntl (sh + pipe + redirect 质变点).
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_dup.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_fcntl.cpp
    # F-ECO batch 5: sysinfo + getrusage (ps/free/uptime/top).
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_sysinfo.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_getrusage.cpp
    # F-ECO busybox sh smoke: poll stub + uname.
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_poll.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_uname.cpp
    # F-ECO batch 7a: socket syscall alignment (accept4 + setsockopt/getsockopt).
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_accept4.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_setsockopt.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_getsockopt.cpp
    # F-ECO batch 7b: getsockname / getpeername / shutdown / socketpair.
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_getsockname.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_getpeername.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_shutdown.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sys_socketpair.cpp
)
