/**
 * @file kernel/gui/desktop_launch.cpp
 * @brief GUI userspace launch -- fork+execve the userspace GUI host (F-GUI-USERSPACE b3b)
 *
 * b3b replaces the in-kernel gui_worker thread with a USERSPACE GUI process.
 * launch_userspace() fork+execve's /cinux_gui_host (the static musl ELF from
 * tools/musl/build-cinux-gui-host.sh), which opens /dev/fb0 + /dev/event0,
 * builds the widget tree, and pumps GuiCore entirely in user mode. gui_start()
 * still runs first to arm the PS/2 mouse + keyboard listener (dual-write into
 * /dev/event0, batch 2) so the host's poll_event drains pointer + keyboard.
 *
 * handoff_framebuffer_to_gui no longer builds any kernel widget tree (the
 * userspace host owns it); it only detaches the text console so routine logs
 * stop overlaying the framebuffer. The old kernel host_cinux.cpp was deleted in
 * chore/post-gui-cleanup -- the Host ABI table is filled in userspace now.
 *
 * CODING-TASTE §14: this is the GUI-side launch_userspace() impl; the non-GUI
 * counterpart (execve /sbin/init) is kernel/proc/shell_launch.cpp. CMake links
 * exactly one (kernel/gui/ is added only under if(CINUX_GUI)).
 */

#include <stdint.h>

#include "kernel/drivers/video/console.hpp"  // Console::console_sink_adapter
#include "kernel/gui/gui_init.hpp"           // gui_start (mouse + kbd listener)
#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/address_space.hpp"  // AddressSpace (child user AS)
#include "kernel/proc/pid.hpp"          // g_pid_alloc
#include "kernel/proc/process.hpp"      // fork
#include "kernel/proc/scheduler.hpp"    // current / exit_current
#include "kernel/proc/user_launch.hpp"  // launch_user_program
#include "kernel/proc/userspace.hpp"    // launch_userspace / handoff_framebuffer_to_gui

namespace cinux::proc {

void launch_userspace() {
    cinux::lib::kprintf("[INIT] ===== Milestone 035: GUI Desktop (b3b userspace) =====\n");

    // Arm PS/2 mouse + keyboard listener. The listener mirrors each KeyEvent
    // into /dev/event0 (batch 2 dual-write), so the userspace host's
    // poll_event drains pointer + keyboard through the one device.
    cinux::gui::gui_start();

    // Fork+execve the userspace GUI host. It opens /dev/fb0 + /dev/event0,
    // builds the widget tree, and pumps GuiCore forever (argv "0"). Replaces
    // the old in-kernel gui_worker thread. Mirrors the test-kernel smoke's
    // fork+execve+AddressSpace shape (kernel/test/main_test.cpp).
    int child_pid = fork(g_pid_alloc);
    if (child_pid == 0) {
        auto* child        = Scheduler::current();
        child->addr_space  = new cinux::mm::AddressSpace();
        const char* argv[] = {"/cinux_gui_host", "0", nullptr};
        const char* envp[] = {nullptr};
        launch_user_program("/cinux_gui_host", argv, envp);
        Scheduler::exit_current();  // unreachable
    }
    cinux::lib::kprintf("[INIT] userspace GUI host launched (pid=%d)\n", child_pid);
}

void handoff_framebuffer_to_gui(cinux::drivers::Framebuffer& fb, cinux::drivers::PSFFont& font,
                                cinux::drivers::Console& console) {
    // b3b: the userspace host owns the widget tree + GuiCore now (it builds
    // them in main() over /dev/fb0 + mmap). The kernel no longer constructs
    // host_cinux -- only detach the text console so routine logs stop
    // overlaying the framebuffer (kpanic re-enables it). fb init +
    // set_system_framebuffer already ran in kernel_main, so /dev/fb0 mmap
    // resolves the VBE framebuffer phys; fb/font are kept on the §14 interface.
    (void)fb;
    (void)font;
    cinux::lib::kprintf_set_sink_enabled(cinux::drivers::Console::console_sink_adapter, &console,
                                         false);
}

}  // namespace cinux::proc
