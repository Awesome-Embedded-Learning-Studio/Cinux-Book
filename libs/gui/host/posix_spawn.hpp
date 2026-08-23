/**
 * @file host/posix_spawn.hpp
 * @brief Linux host: spawn a child under a PTY (P4-d)
 *
 * forkpty + execvp; the parent gets the PTY master fd, which is bidirectional
 * (write = child stdin, read = child stdout). The signature mirrors
 * core/host.hpp's HostDesktop::spawn so a host can fill that ABI slot directly,
 * but standalone harnesses (terminal-host) also call it themselves.
 *
 * HOST layer (uses <pty.h> / <unistd.h>); NOT part of the host-neutral core.
 */
#pragma once

#include <stddef.h>

namespace cinux::gui {

/** Spawn @p path under a PTY. Returns child pid (>0) on success, -1 on failure.
 * *stdin_fd and *stdout_fd both receive the PTY master (bidirectional). */
int linux_spawn(void* ctx, const char* path, char* const argv[], int* stdin_fd, int* stdout_fd);

}  // namespace cinux::gui
