/*
 * calleetest.c -- verify the kernel preserves user callee-saved registers
 * (RBX, RBP, R12-R15) across syscalls (SysV AMD64 ABI requirement).
 *
 * syscall_entry (syscall.S) saves ONLY RBX (frame+80) and RBP (frame+88) in the
 * pt_regs frame; it does NOT save R12-R15, relying on syscall_dispatch (a C
 * function) to preserve them per the ABI.  If any kernel build setting or inline
 * asm breaks that assumption, user callee-saved regs come back garbage after a
 * syscall -- corrupting any user state the compiler keeps in those regs across a
 * call (e.g. a `path` pointer in RBX/R12).  This program sets all five
 * callee-saved regs to magic values, issues a syscall, and checks them.
 *
 * Output: "CALLREETEST ok=N bad=M\n" + first failing reg/value.  Load as /hello
 * so the ring-3 smoke runs it under -smp 2.
 */
#include <stdint.h>
#include <unistd.h>

static long sys_write(int fd, const void *buf, unsigned long n) {
    long ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(1L), "D"((long)fd), "S"(buf), "d"(n)
                     : "rcx", "r11", "memory");
    return ret;
}

/* Set callee-saved regs to magics, issue SYS_yield, read them back -- one asm so
 * the compiler cannot spill them around the call. Returns # of regs corrupted.
 * (RBP can't be pinned as a register-asm var -- gcc reserves it for the frame
 * pointer -- and syscall_entry already restores RBP explicitly; so we probe
 * RBX + R12-R15, the regs syscall_entry does NOT save and relies on the C ABI to
 * preserve.) */
static int probe(uint64_t out[5]) {
    register uint64_t rbx asm("rbx");
    register uint64_t r12 asm("r12");
    register uint64_t r13 asm("r13");
    register uint64_t r14 asm("r14");
    register uint64_t r15 asm("r15");
    __asm__ volatile(
        "movq $0xE1E1E1E1E1E1E1E1, %%rbx\n\t"
        "movq $0xA3A3A3A3A3A3A3A3, %%r12\n\t"
        "movq $0xB4B4B4B4B4B4B4B4, %%r13\n\t"
        "movq $0xC5C5C5C5C5C5C5C5, %%r14\n\t"
        "movq $0xD6D6D6D6D6D6D6D6, %%r15\n\t"
        "movq $24, %%rax\n\t" /* SYS_yield */
        "xorq %%rdi, %%rdi\n\t"
        "syscall\n\t"
        : "=r"(rbx), "=r"(r12), "=r"(r13), "=r"(r14), "=r"(r15)
        :
        : "rax", "rcx", "rdx", "rsi", "r8", "r9", "r10", "r11", "memory");
    out[0] = rbx;
    out[1] = r12;
    out[2] = r13;
    out[3] = r14;
    out[4] = r15;
    static const uint64_t want[5] = {0xE1E1E1E1E1E1E1E1UL, 0xA3A3A3A3A3A3A3A3UL,
                                     0xB4B4B4B4B4B4B4B4UL, 0xC5C5C5C5C5C5C5C5UL,
                                     0xD6D6D6D6D6D6D6D6UL};
    int bad = 0;
    for (int i = 0; i < 5; i++)
        if (out[i] != want[i])
            bad++;
    return bad;
}

static void put_str(char *b, int *o, const char *s) {
    while (*s)
        b[(*o)++] = *s++;
}
static void put_dec(char *b, int *o, long v) {
    char t[24];
    int n = 0;
    if (v == 0)
        t[n++] = '0';
    for (long x = v; x > 0; x /= 10)
        t[n++] = (char)('0' + x % 10);
    while (n > 0)
        b[(*o)++] = t[--n];
}
static void put_hex(char *b, int *o, uint64_t v) {
    char t[24];
    int n = 0;
    for (int i = 0; i < 16; i++)
        t[n++] = (char)((v >> (4 * (15 - i))) & 0xf);
    /* trim leading zeros */
    int s = 0;
    while (s < 15 && t[s] == '0')
        s++;
    for (int i = s; i < 16; i++)
        b[(*o)++] = t[i] < 10 ? (char)('0' + t[i]) : (char)('a' + t[i] - 10);
}

int main(void) {
    long ok = 0, bad = 0;
    uint64_t first_out[5] = {0};
    static const char *names[5] = {"rbx", "r12", "r13", "r14", "r15"};

    for (long i = 0; i < 2000; i++) {
        uint64_t out[5];
        int b = probe(out);
        if (b == 0) {
            ok++;
        } else {
            bad++;
            if (bad == 1)
                for (int k = 0; k < 5; k++)
                    first_out[k] = out[k];
        }
    }

    char buf[160];
    int off = 0;
    put_str(buf, &off, "CALLREETEST ok=");
    put_dec(buf, &off, ok);
    put_str(buf, &off, " bad=");
    put_dec(buf, &off, bad);
    buf[off++] = '\n';
    if (bad > 0) {
        static const uint64_t want[5] = {0xE1E1E1E1E1E1E1E1UL, 0xA3A3A3A3A3A3A3A3UL,
                                         0xB4B4B4B4B4B4B4B4UL, 0xC5C5C5C5C5C5C5C5UL,
                                         0xD6D6D6D6D6D6D6D6UL};
        put_str(buf, &off, "first bad regs:");
        for (int k = 0; k < 5; k++) {
            if (first_out[k] != want[k]) {
                buf[off++] = ' ';
                put_str(buf, &off, names[k]);
                buf[off++] = '=';
                put_hex(buf, &off, first_out[k]);
            }
        }
        buf[off++] = '\n';
    }
    sys_write(1, buf, off);
    return bad > 0 ? 1 : 0;
}
