/**
 * @file kernel/drivers/serial/serial.cpp
 * @brief Serial port (UART 16550) driver implementation
 */

#include "serial.hpp"

#include <stdint.h>

// Pull in the I/O primitives (io_inb / io_outb)
#include "kernel/arch/x86_64/io.hpp"

using cinux::io::io_inb;
using cinux::io::io_outb;

namespace cinux::drivers {

// ============================================================
// Constructor
// ============================================================

Serial::Serial(uint16_t port) : base_port_(port) {
    // Caller calls init() explicitly after construction.
}

// ============================================================
// init() -- configure the UART
// ============================================================

void Serial::init() {
    io_outb(base_port_ + SerialReg::IER, SerialCfg::IER_DISABLE);
    io_outb(base_port_ + SerialReg::LCR, SerialCfg::LCR_8N1);
    io_outb(base_port_ + SerialReg::FCR, SerialCfg::FCR_ON);
    io_outb(base_port_ + SerialReg::MCR, SerialCfg::MCR_DTR_RTS);
    io_inb(base_port_ + SerialReg::LSR);  // read LSR to clear any pending line status
}

// ============================================================
// is_tx_ready() -- check THR empty
// ============================================================

bool Serial::is_tx_ready() const {
    return (io_inb(base_port_ + SerialReg::LSR) & SerialLSR::TX_READY) != 0;
}

// ============================================================
// is_ready() -- public check for TX readiness
// ============================================================

bool Serial::is_ready() const {
    return is_tx_ready();
}

// ============================================================
// putc() -- write one character (blocking)
// ============================================================

void Serial::putc(char c) {
    while (!is_tx_ready()) {
        __asm__ volatile("pause");
    }

    io_outb(base_port_ + SerialReg::THR, static_cast<uint8_t>(c));
}

// ============================================================
// puts() -- write a null-terminated string
// ============================================================

void Serial::puts(const char* s) {
    if (s == nullptr) {
        return;
    }

    while (*s != '\0') {
        if (*s == '\n') {
            putc('\r');
        }
        putc(*s);
        s++;
    }
}

}  // namespace cinux::drivers
