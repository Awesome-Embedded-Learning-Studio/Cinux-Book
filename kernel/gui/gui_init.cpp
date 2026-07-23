/**
 * @file kernel/gui/gui_init.cpp
 * @brief GUI subsystem initialisation implementation
 *
 * Encapsulates all GUI setup (mouse driver, window manager, PIT
 * tick callback) behind the gui_init() / gui_start() interface so
 * that kernel_main and kernel_init_thread remain GUI-agnostic.
 */

#include "gui_init.hpp"

#include "kernel/drivers/canvas.hpp"
#include "kernel/drivers/keyboard/keyboard.hpp"
#include "kernel/drivers/mouse/mouse.hpp"
#include "kernel/drivers/tty/pty_device.hpp"  // pty_alloc/master/slave (F-ECO busybox PTY)
#include "kernel/drivers/video/font.hpp"
#include "kernel/fs/file.hpp"
#include "kernel/fs/inode.hpp"
#include "kernel/fs/vfs_mount.hpp"
#include "kernel/gui/desktop_icon.hpp"
#include "kernel/gui/host_cinux.hpp"
#include "kernel/gui/icon.hpp"
#include "kernel/gui/terminal.hpp"
#include "kernel/gui/window_manager.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/lib/string.hpp"
#include "kernel/mm/address_space.hpp"
#include "kernel/proc/percpu.hpp"
#include "kernel/proc/pid.hpp"
#include "kernel/proc/process.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/user_launch.hpp"

namespace cinux::gui {

// ============================================================
// Module-internal state
// ============================================================

namespace {
cinux::drivers::Canvas*  g_screen = nullptr;
cinux::drivers::PSFFont* g_font   = nullptr;

/// Counter for generating unique terminal titles
uint32_t g_terminal_counter = 0;

}  // anonymous namespace

// ============================================================
// gui_init() -- one-time GUI setup from kernel_main
// ============================================================

void gui_init(cinux::drivers::Canvas& screen, cinux::drivers::PSFFont& font) {
    cinux::lib::kprintf("[GUI] Initialising GUI subsystem...\n");

    // Store pointers for later use in gui_start()
    g_screen = &screen;
    g_font   = &font;

    // Initialise the window manager. The desktop itself is NOT drawn here --
    // gui_start() composites it once after icons are registered, and ongoing
    // refresh is driven by the gui_worker thread calling pump() (see
    // init.cpp), not by a PIT IRQ callback.
    WindowManager::instance().init(&screen, &font);

    cinux::lib::kprintf("[GUI] GUI subsystem initialised (refresh via gui_worker pump).\n");
}

// ============================================================
// Internal helper: create a shell terminal window
// ============================================================

namespace {

/// Launch info passed from the parent (gui_worker) to the shell child task.
struct ShellLaunchInfo {
    cinux::fs::Inode* slave_inode;  ///< PTY slave: the shell's controlling tty
    const char*       path;
};

/// Entry function for shell child tasks.  Runs on a clean kernel stack
/// allocated by TaskBuilder (no inherited parent frames), so the full
/// 16 KB stack is available from the start.
static void shell_child_entry() {
    auto* task = cinux::proc::Scheduler::current();
    auto* info = static_cast<ShellLaunchInfo*>(task->private_data);

    __asm__ volatile("cli");

    task->addr_space = new cinux::mm::AddressSpace();

    // The shell's stdin/stdout/stderr are all the PTY slave -- a real tty, so
    // isatty(0)==true: busybox ash enters interactive mode (prompt + line edit)
    // and musl line-buffers stdout (command output flushes per line, not stuck
    // in a 4 KB stdio buffer as it was on a pipe).  No -i flag needed.
    task->fd_table = new cinux::fs::FDTable();
    task->fd_table->set(0, new cinux::fs::File(info->slave_inode, 0, cinux::fs::OpenFlags::RDONLY));
    task->fd_table->set(1, new cinux::fs::File(info->slave_inode, 0, cinux::fs::OpenFlags::WRONLY));
    task->fd_table->set(2, new cinux::fs::File(info->slave_inode, 0, cinux::fs::OpenFlags::WRONLY));

    // No -i: the PTY slave is a real tty, so isatty(0)==true and busybox ash
    // enters interactive mode on its own (prompt + echo via the TTY discipline).
    const char* argv[] = {info->path, nullptr};
    const char* envp[] = {"PATH=/bin", nullptr};
    // Load the program, set up the user stack, and jump to user mode.
    // Consolidated with the non-GUI shell launch in init.cpp into
    // launch_user_program(); never returns (jumps to user mode or exits).
    cinux::proc::launch_user_program(info->path, argv, envp);
}

}  // anonymous namespace

void create_shell_terminal() {
    auto& wm = WindowManager::instance();

    // Generate a unique title for this terminal instance
    g_terminal_counter++;
    char title_buf[64];
    strcpy(title_buf, "Shell #");
    utoa(title_buf + strlen(title_buf), g_terminal_counter);

    // Calculate terminal dimensions
    uint32_t term_w = Terminal::COLS * 8;   // 80 * 8 = 640
    uint32_t term_h = Terminal::ROWS * 16;  // 25 * 16 = 400

    // Centre the terminal on screen if possible
    uint32_t term_x = 80;
    uint32_t term_y = 60;

    if (g_screen != nullptr) {
        uint32_t sw = g_screen->width();
        uint32_t sh = g_screen->height();
        if (term_w + 80 < sw) {
            term_x = (sw - term_w) / 2;
        }
        if (term_h + 60 < sh) {
            term_y = (sh - term_h) / 2;
        }
    }

    auto* term = new Terminal(term_x, term_y, title_buf);
    term->set_font(g_font);

    // --- Allocate a PTY pair for this terminal (F-ECO busybox) ---
    // The shell gets the slave (a real tty: isatty true → ash interactive +
    // musl line-buffers stdout).  The terminal holds the master (keyboard in,
    // shell output + TTY echo out).  Replaces the old stdin/stdout pipe pair
    // which gave the shell a non-tty → no echo + stdio full-buffer (output stuck).
    auto pty_idx = cinux::drivers::pty_alloc();
    if (!pty_idx.ok()) {
        cinux::lib::kprintf("[GUI] pty_alloc failed for terminal '%s'\n", title_buf);
        delete term;
        return;
    }
    cinux::fs::Inode* master_inode = cinux::drivers::pty_master_inode(*pty_idx);
    auto              slave_r      = cinux::drivers::pty_slave_inode(*pty_idx);
    if (master_inode == nullptr || !slave_r.ok()) {
        cinux::lib::kprintf("[GUI] PTY master/slave lookup failed for terminal '%s'\n", title_buf);
        cinux::drivers::pty_release(*pty_idx);
        delete term;
        return;
    }
    cinux::fs::Inode* slave_inode = *slave_r;

    term->set_master(master_inode, *pty_idx);

    // --- Spawn shell via TaskBuilder (clean stack, no fork) ---
    auto* info = new ShellLaunchInfo{slave_inode, "/bin/sh"};

    cinux::proc::TaskBuilder builder;
    builder.set_entry(shell_child_entry).set_name("shell");
    auto* child = builder.build();
    if (child == nullptr) {
        cinux::lib::kprintf("[GUI] TaskBuilder::build failed for shell\n");
        delete info;
        // term's destructor will pty_release via set_master's index.
        delete term;
        return;
    }

    // PID + parent/child linkage (TaskBuilder handles TCB + stack only)
    child->pid          = cinux::proc::g_pid_alloc.alloc();
    child->private_data = info;
    auto* parent        = cinux::proc::Scheduler::current();
    child->ppid         = parent->pid;
    child->parent       = parent;
    child->wait_next    = parent->children;
    parent->children    = child;

    cinux::proc::Scheduler::add_task(child);

    term->set_shell_pid(child->pid);
    cinux::lib::kprintf("[GUI] Terminal '%s': shell spawned pid=%d (pty=%d)\n", title_buf,
                        child->pid, *pty_idx);

    wm.add_window(term);
    cinux::lib::kprintf("[GUI] Terminal '%s' created with PTY master=%p\n", title_buf,
                        reinterpret_cast<void*>(master_inode));
}

// ============================================================
// gui_start() -- activate the WM (refresh driven by gui_worker pump)
// ============================================================

namespace {
// Key listener registered with the keyboard driver: mirror each decoded
// KeyEvent into the GUI EventQueue so the window manager sees keyboard input.
// Lives here (not in keyboard.cpp) so the keyboard driver has no GUI/EventQueue
// dependency -- it just calls a registered listener (CODING-TASTE §14).
void on_key_event(const cinux::drivers::KeyEvent& ev) {
    cinux::gui::Event gui_ev{};
    gui_ev.type_     = ev.pressed ? cinux::gui::EventType::KeyDown : cinux::gui::EventType::KeyUp;
    gui_ev.key.ascii = ev.ascii;
    gui_ev.key.scancode = ev.scancode;
    gui_ev.key.pressed  = ev.pressed;
    gui_ev.key.shift    = ev.shift;
    gui_ev.key.ctrl     = ev.ctrl;
    gui_ev.key.alt      = ev.alt;
    cinux::drivers::Mouse::event_queue().enqueue(gui_ev);
}
}  // namespace

void gui_start() {
    cinux::lib::kprintf("[GUI] ===== Milestone 033: GUI Desktop =====\n");

    // Initialise PS/2 mouse driver
    cinux::drivers::Mouse::init();
    // Dual-dispatch keys into the GUI queue via a listener (the keyboard has no
    // #ifdef CINUX_GUI; it just calls whoever registered -- §14).
    cinux::drivers::Keyboard::register_key_listener(on_key_event);

    // Configure mouse screen bounds to match the canvas
    if (g_screen != nullptr) {
        cinux::drivers::Mouse::set_screen_bounds(g_screen->width(), g_screen->height());
    }

    // Register desktop icons on the desktop
    auto& wm = WindowManager::instance();

    DesktopIcon shell_icon{
        .x      = 40,
        .y      = 40,
        .bitmap = icons::data::k_shell_icon.data(),
        .mask   = icons::data::k_shell_mask.data(),
        .label  = "Shell",
        .width  = icons::ICON_SIZE,
        .height = icons::ICON_SIZE,
        .action = IconAction::OpenShell,
    };
    wm.add_desktop_icon(shell_icon);

    DesktopIcon calc_icon{
        .x      = 40,
        .y      = 120,
        .bitmap = icons::data::k_calc_icon.data(),
        .mask   = icons::data::k_calc_mask.data(),
        .label  = "Calculator",
        .width  = icons::ICON_SIZE,
        .height = icons::ICON_SIZE,
        .action = IconAction::OpenCalculator,
    };
    wm.add_desktop_icon(calc_icon);

    cinux::lib::kprintf("[GUI] Desktop icons registered: Shell, Calculator.\n");

    // Composite the desktop once now (icons registered) so the staging back
    // buffer is populated. Ongoing refresh is driven by the gui_worker thread
    // calling pump() in a loop (see init.cpp), NOT by a PIT IRQ callback.
    // This removes the GUI's dependency on PIT tick delivery, which only fires
    // once under APIC routing on the production path (pre-existing F4 issue) --
    // the worker pump keeps the screen live regardless of whether PIT ticks
    // arrive. F13 §4c: composite() renders the back buffer only; the pump
    // flushes the dirty region to the host. Mark the whole screen dirty so the
    // first pump iteration pushes the initial desktop.
    wm.composite();
    wm.invalidate_all();
    cinux::lib::kprintf("[GUI] desktop composited; refresh driven by gui_worker pump loop.\n");

    // Initialise the cinux::gui Host ABI adapter (F13 §3b/§4c): fills the host table
    // that the gui_worker's pump() drives. The callbacks forward to the
    // facilities wired above; flush forwards dirty rects to the framebuffer.
    cinux_host_init(g_screen != nullptr ? g_screen->framebuffer() : nullptr);
}

}  // namespace cinux::gui
