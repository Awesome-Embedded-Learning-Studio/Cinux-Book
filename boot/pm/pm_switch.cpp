extern "C" [[noreturn]] void PmEntry();

extern "C" [[noreturn]] void EnterProtectedMode() {
    asm volatile(
        "cli\n"
        "lgdt kGdtr\n"
        "movl %%cr0, %%eax\n"
        "orb $1, %%al\n"
        "movl %%eax, %%cr0\n"
        "ljmp $0x08, $PmEntry\n"
        :
        :
        : "ax", "memory");
    __builtin_unreachable();
}
