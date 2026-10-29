/* Wake QEMU's main loop after libslirp may have created a host socket.
 * Its previous poll set cannot observe the new fd until it is rebuilt.
 * Keep this on the host side: the guest's receive pump is already running.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Slirp Slirp;
typedef void (*SlirpInput)(Slirp *, const uint8_t *, int);
typedef void (*QemuNotify)(void);
static SlirpInput original_input;
static QemuNotify notify_main_loop;
static pthread_once_t resolve_once = PTHREAD_ONCE_INIT;

static void resolve_symbols(void) {
    void *input = dlsym(RTLD_NEXT, "slirp_input");
    void *notify = dlsym(RTLD_DEFAULT, "qemu_notify_event");
    if (input == NULL || notify == NULL) {
        fputs("Cinux QEMU wakeup: required QEMU/libslirp symbols are unavailable; "
              "configure with -DCINUX_QEMU_SLIRP_WAKEUP=OFF for this QEMU build.\n", stderr);
        abort();
    }
    _Static_assert(sizeof(original_input) == sizeof(input), "ELF function pointer ABI");
    _Static_assert(sizeof(notify_main_loop) == sizeof(notify), "ELF function pointer ABI");
    memcpy(&original_input, &input, sizeof(input));
    memcpy(&notify_main_loop, &notify, sizeof(notify));
}

void slirp_input(Slirp *slirp, const uint8_t *packet, int length) {
    pthread_once(&resolve_once, resolve_symbols);
    original_input(slirp, packet, length);
    notify_main_loop();
}
