#include <api/syscall.hpp>

namespace {

/// The two threads run on private stacks of one page... four, to be dull.
constexpr unsigned long long kThreadStackBytes = 16ULL * 1024ULL;

void put(const char* text) {
    unsigned long long length = 0;
    while (text[length] != '\0') {
        length++;
    }
    user::Write(1, text, length);
}

void put_signed(long long value) {
    char               digits[24] = {};
    unsigned long long count      = 0;
    auto               raw        = static_cast<unsigned long long>(value < 0 ? -value : value);
    if (raw == 0) {
        put("0");
        return;
    }
    while (raw != 0) {
        digits[count] = static_cast<char>('0' + (raw % 10ULL));
        count++;
        raw /= 10ULL;
    }
    if (value < 0) {
        put("-");
    }
    char out[24] = {};
    for (unsigned long long index = 0; index < count; ++index) {
        out[index] = digits[count - 1 - index];
    }
    put(out);
}

/// Shared world: both threads and the maker see every word here.
unsigned long long g_counter       = 0;
char               g_letterbox[32] = {};
unsigned long long g_tls_a         = 0xA11CEULL;
unsigned long long g_tls_b         = 0xB0B05ULL;

/// The join words: clone's CHILD_SETTID writes each thread's tid
/// here, its death zeroes the word and wakes the parked joiner.
unsigned int g_tid_word_a = 0;
unsigned int g_tid_word_b = 0;

/// What each thread hands back, parked here before it exits —
/// pthread_join's return value travels the same way.
long long g_result_a = 0;
long long g_result_b = 0;

alignas(16) unsigned char g_stack_a[kThreadStackBytes];
alignas(16) unsigned char g_stack_b[kThreadStackBytes];

/// Reads the quadword at fs:0 — the slot the thread's own base names.
unsigned long long read_fs_slot() {
    unsigned long long value = 0;  // NOLINT(misc-const-correctness) written by the asm output
    __asm__ volatile("movq %%fs:0, %0" : "=r"(value));
    return value;
}

void worker_a(void* /*arg*/) {
    const auto kTid = static_cast<unsigned long long>(user::Gettid());
    put("[thr a] tid=");
    put_signed(static_cast<long long>(kTid));
    put(" getpid=");
    put_signed(user::Getpid());
    put(" fs:0=");
    put_signed(static_cast<long long>(read_fs_slot()));
    put("\n");
    char note[32] = {};
    for (unsigned long index = 0; index < 31; ++index) {
        note[index] = static_cast<char>('a' + static_cast<char>(index % 26));
    }
    for (unsigned long index = 0; index < 32; ++index) {
        g_letterbox[index] = note[index];
    }
    g_counter += 100;
    char borrowed[8] = {};
    static_cast<void>(user::Read(3, borrowed, sizeof(borrowed)));
    put("[thr a] borrowed 8 bytes through the shared descriptor\n");
    g_result_a = 65;
    user::Exit(65);
}

void worker_b(void* /*arg*/) {
    const auto kTid = static_cast<unsigned long long>(user::Gettid());
    put("[thr b] tid=");
    put_signed(static_cast<long long>(kTid));
    put(" getpid=");
    put_signed(user::Getpid());
    put(" fs:0=");
    put_signed(static_cast<long long>(read_fs_slot()));
    put("\n");
    g_counter += 155;
    g_result_b = 34;
    user::Exit(34);
}

/// Parks on one thread's join word until its death wakes this side.
/// A thread that left before the park answers -11 (the word moved on);
/// that is a join too, and reading the zero confirms it.
long long join_by_tid(unsigned long long word_address, unsigned int tid) {
    const long long kAnswer = user::FutexWait(word_address, tid);
    if (kAnswer == 0 || kAnswer == -11) {
        return 0;
    }
    return kAnswer;
}

}  // namespace

// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" void _start() {
    put("[thr] main pid=");
    put_signed(user::Getpid());
    put(" tid=");
    put_signed(user::Gettid());
    put("\n");

    const long long kFd = user::Open("/etc/motd", 0);
    put("[thr] motd descriptor=");
    put_signed(kFd);
    put("\n");

    const unsigned long long kShareFlags =
        0x100ULL | 0x400ULL | 0x10000ULL | 0x80000ULL | 0x200000ULL | 0x1000000ULL;
    const long long kTidA = user::Clone(
        kShareFlags, g_stack_a + kThreadStackBytes, reinterpret_cast<unsigned long long>(&g_tls_a),
        &worker_a, nullptr, 0, reinterpret_cast<unsigned long long>(&g_tid_word_a));
    const long long kTidB = user::Clone(
        kShareFlags, g_stack_b + kThreadStackBytes, reinterpret_cast<unsigned long long>(&g_tls_b),
        &worker_b, nullptr, 0, reinterpret_cast<unsigned long long>(&g_tid_word_b));
    put("[thr] clone answered a=");
    put_signed(kTidA);
    put(" b=");
    put_signed(kTidB);
    put("\n");
    put("[thr] tid words after clone: a=");
    put_signed(g_tid_word_a);
    put(" b=");
    put_signed(g_tid_word_b);
    put("\n");

    const long long kJoinA = join_by_tid(reinterpret_cast<unsigned long long>(&g_tid_word_a),
                                         static_cast<unsigned int>(kTidA));
    const long long kJoinB = join_by_tid(reinterpret_cast<unsigned long long>(&g_tid_word_b),
                                         static_cast<unsigned int>(kTidB));
    put("[thr] joined a: wait=");
    put_signed(kJoinA);
    put(" word=");
    put_signed(g_tid_word_a);
    put(" code=");
    put_signed(g_result_a);
    put("\n");
    put("[thr] joined b: wait=");
    put_signed(kJoinB);
    put(" word=");
    put_signed(g_tid_word_b);
    put(" code=");
    put_signed(g_result_b);
    put("\n");

    int             code_word    = 0;
    const long long kNotWaitable = user::Wait4(kTidA, &code_word);
    put("[thr] wait4 on a thread answers ");
    put_signed(kNotWaitable);
    put(" (a thread is not a waitable child)\n");

    put("[thr] counter=");
    put_signed(static_cast<long long>(g_counter));
    put(" letterbox[0]=");
    put_signed(static_cast<long long>(g_letterbox[0]));
    put("\n");

    char            after[8] = {};
    const long long kGot = user::Read(static_cast<unsigned long long>(kFd), after, sizeof(after));
    put("[thr] main read ");
    put_signed(kGot);
    put(" bytes from the shared descriptor after the join: ");
    user::Write(1, after, sizeof(after));
    put("\n");

    user::Exit(0);
}
