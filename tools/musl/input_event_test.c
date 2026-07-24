/*
 * input_event_test.c — ring-3 smoke for /dev/event0 (F-GUI-USERSPACE batch 2).
 *
 * Opens /dev/event0, reads two input events (one mouse, one key), and verifies
 * their type tags + payloads.  This is the end-to-end exercise of the batch-2
 * input path: ISR push_event -> InputEventDevice ring -> sys_read ->
 * copy_to_user of one Event.  The test kernel's smoke_entry pushes a known
 * MouseMove then KeyDown before fork+execve'ing this program (the test kernel
 * has no real mouse/keyboard in QEMU automation), so the reads return at once.
 *
 * The struct layouts mirror kernel/gui/event.hpp (cinux::gui::Event).  Cinux
 * and this program are both built with gcc under the SysV x86_64 ABI, so the C
 * mirror shares the kernel C++ POD layout exactly.
 *
 * Built static against the Cinux musl sysroot (see build-input-event-test.sh).
 */
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

/* Mirror of kernel/gui/event.hpp EventType discriminant. */
enum input_event_type {
    EV_MOUSE_MOVE = 0,
    EV_MOUSE_DOWN = 1,
    EV_MOUSE_UP   = 2,
    EV_KEY_DOWN   = 3,
    EV_KEY_UP     = 4,
};

/* Mirror of cinux::gui::MouseEvent / KeyEvent.  Field order + types match the
 * kernel structs so the union layout is identical. */
struct mouse_event {
    int32_t x;
    int32_t y;
    int32_t dx;
    int32_t dy;
    uint8_t buttons;
    bool    left;
    bool    right;
    bool    middle;
};
struct key_event {
    char    ascii;
    uint8_t scancode;
    bool    pressed;
    bool    shift;
    bool    ctrl;
    bool    alt;
};
struct input_event {
    uint8_t type_;
    union {
        struct mouse_event mouse;
        struct key_event   key;
    };
};

/* Smoke contract: the test harness pushes a MouseMove to (123, 45) and a
 * KeyDown of 'A' before exec'ing us.  Any mismatch is a distinct exit code so
 * the kernel smoke log pinpoints which leg broke. */
#define EXPECTED_MOUSE_X 123
#define EXPECTED_MOUSE_Y 45
#define EXPECTED_KEY_CH  'A'

int main(void) {
    int fd = open("/dev/event0", O_RDONLY);
    if (fd < 0) {
        return 1; /* open failed -- /dev/event0 not registered or devfs off */
    }

    struct input_event ev;
    ssize_t            n;

    /* 1) MouseMove at (123, 45). */
    n = read(fd, &ev, sizeof(ev));
    if (n != (ssize_t)sizeof(ev)) {
        return 2; /* short read or error -- queue empty / copy_to_user broke */
    }
    if (ev.type_ != EV_MOUSE_MOVE) {
        return 3; /* wrong type -- event ordering or Event layout mismatch */
    }
    if (ev.mouse.x != EXPECTED_MOUSE_X || ev.mouse.y != EXPECTED_MOUSE_Y) {
        return 4; /* payload corruption -- copy_to_user wrote wrong bytes */
    }

    /* 2) KeyDown of 'A'.  Also proves read() drains in FIFO order. */
    n = read(fd, &ev, sizeof(ev));
    if (n != (ssize_t)sizeof(ev)) {
        return 5;
    }
    if (ev.type_ != EV_KEY_DOWN) {
        return 6;
    }
    if (ev.key.ascii != EXPECTED_KEY_CH) {
        return 7;
    }

    return 0;
}
