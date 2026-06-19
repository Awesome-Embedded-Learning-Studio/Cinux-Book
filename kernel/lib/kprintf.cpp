/**
 * @file kernel/lib/kprintf.cpp
 * @brief Kernel formatted print implementation (multi-backend sink)
 *
 * Internal structure:
 *   1. The hardware-independent formatting engine lives in
 *      kernel/lib/private/vkprintf_impl.hpp (shared with host tests).
 *   2. A static sink table holds up to KPRINTF_MAX_SINKS entries.
 *   3. Public kprintf / kvprintf / kpanic iterate all enabled sinks
 *      and invoke each with every formatted character.
 */

#include "kernel/lib/kprintf.hpp"

#include <stdarg.h>
#include <stdint.h>

#include "kernel/arch/x86_64/backtrace.hpp"
#include "kernel/drivers/serial/serial.hpp"
#include "kernel/lib/private/vkprintf_impl.hpp"

namespace {

using cinux::drivers::Serial;
using cinux::drivers::SERIAL_COM1;
using cinux::lib::detail::vkprintf_impl;

// ============================================================
// Sink table
// ============================================================

struct Sink {
    cinux::lib::OutputSink fn;
    void*                  ctx;
    bool                   enabled;
};

static Sink     g_sinks[cinux::lib::KPRINTF_MAX_SINKS] = {};
static uint32_t g_sink_count                           = 0;

// ============================================================
// Serial sink adapter
// ============================================================

static Serial g_serial(SERIAL_COM1);

void serial_sink_adapter(char c, void* /*ctx*/) {
    g_serial.putc(c);
}

}  // anonymous namespace

namespace cinux::lib {

// ============================================================
// Sink registration
// ============================================================

void kprintf_register_sink(OutputSink fn, void* ctx) {
    if (fn == nullptr)
        return;
    for (uint32_t i = 0; i < g_sink_count; i++) {
        if (!g_sinks[i].enabled) {
            g_sinks[i] = {fn, ctx, true};
            return;
        }
    }
    if (g_sink_count < KPRINTF_MAX_SINKS) {
        g_sinks[g_sink_count++] = {fn, ctx, true};
    }
}

void kprintf_set_sink_enabled(OutputSink fn, void* ctx, bool enabled) {
    for (uint32_t i = 0; i < g_sink_count; i++) {
        if (g_sinks[i].fn == fn && g_sinks[i].ctx == ctx) {
            g_sinks[i].enabled = enabled;
            return;
        }
    }
}

void kprintf_enable_all_sinks() {
    for (uint32_t i = 0; i < g_sink_count; i++) {
        g_sinks[i].enabled = true;
    }
}

// ============================================================
// kprintf_init -- one-time serial port setup for kprintf
// ============================================================

void kprintf_init() {
    g_serial.init();
    kprintf_register_sink(serial_sink_adapter, nullptr);
}

// ============================================================
// kprintf -- variadic print to all sinks
// ============================================================

void kprintf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vkprintf_impl(
        [&](char c) {
            for (uint32_t i = 0; i < g_sink_count; i++) {
                if (g_sinks[i].enabled) {
                    g_sinks[i].fn(c, g_sinks[i].ctx);
                }
            }
        },
        fmt, args);
    va_end(args);
}

// ============================================================
// kvprintf -- va_list print to all sinks
// ============================================================

void kvprintf(const char* fmt, va_list args) {
    vkprintf_impl(
        [&](char c) {
            for (uint32_t i = 0; i < g_sink_count; i++) {
                if (g_sinks[i].enabled) {
                    g_sinks[i].fn(c, g_sinks[i].ctx);
                }
            }
        },
        fmt, args);
}

// ============================================================
// kpanic -- print + halt
// ============================================================

void kpanic(const char* fmt, ...) {
    // A crash must reach every possible output.  The GUI may have detached the
    // framebuffer console sink to keep logs off the desktop; force every sink
    // back on so the panic (and backtrace below) is visible on screen too.
    kprintf_enable_all_sinks();

    va_list args;
    va_start(args, fmt);
    vkprintf_impl(
        [&](char c) {
            for (uint32_t i = 0; i < g_sink_count; i++) {
                if (g_sinks[i].enabled) {
                    g_sinks[i].fn(c, g_sinks[i].ctx);
                }
            }
        },
        fmt, args);
    va_end(args);

    // FO batch 3: dump the call stack so the kpanic() caller is identifiable
    // even without an exception frame.  Safe at any init stage -- falls back
    // to the boot-stack range and shows raw addresses until KALLSYMS injection.
    cinux::arch::backtrace();

    while (1) {
        __asm__ volatile("cli; hlt");
    }
}

}  // namespace cinux::lib
