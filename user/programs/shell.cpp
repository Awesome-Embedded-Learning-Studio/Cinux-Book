#include "api/syscall.hpp"
#include "kernel/fs/fs_config.hpp"
#include "kernel/syscall/syscall.hpp"

namespace {

constexpr unsigned long kMaxLine  = 128;
constexpr unsigned long kMaxWords = 16;

unsigned long str_len(const char* text) {
    unsigned long length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

bool str_eq(const char* left, const char* right) {
    unsigned long i = 0;
    for (; left[i] != '\0' && right[i] != '\0'; ++i) {
        if (left[i] != right[i]) {
            return false;
        }
    }
    return left[i] == right[i];
}

void put_text(const char* text) {
    user::Write(1, text, str_len(text));
}

unsigned long read_line(char* line) {
    unsigned long pos = 0;
    while (pos + 1 < kMaxLine) {
        char glyph = 0;
        if (user::Read(0, &glyph, 1) != 1) {
            continue;
        }
        if (glyph == '\n') {
            put_text("\n");
            break;
        }
        if (glyph == '\b' || glyph == 0x7F) {
            if (pos > 0) {
                --pos;
                put_text("\b \b");
            }
            continue;
        }
        user::Write(1, &glyph, 1);
        line[pos] = glyph;
        ++pos;
    }
    line[pos] = '\0';
    return pos;
}

unsigned long tokenize(char* line, char** words) {
    unsigned long count = 0;
    unsigned long i     = 0;
    while (line[i] != '\0' && count < kMaxWords) {
        while (line[i] == ' ' || line[i] == '\t') {
            ++i;
        }
        if (line[i] == '\0') {
            break;
        }
        words[count] = &line[i];
        ++count;
        while (line[i] != '\0' && line[i] != ' ' && line[i] != '\t') {
            ++i;
        }
        if (line[i] != '\0') {
            line[i] = '\0';
            ++i;
        }
    }
    words[count] = nullptr;
    return count;
}

void put_decimal(unsigned long long value) {
    char          digits[24];
    unsigned long pos = 0;
    if (value == 0) {
        digits[0] = '0';
        pos       = 1;
    }
    while (value > 0) {
        digits[pos] = static_cast<char>('0' + (value % 10));
        ++pos;
        value /= 10;
    }
    char flipped[24];
    for (unsigned long j = 0; j < pos; ++j) {
        flipped[j] = digits[pos - 1 - j];
    }
    user::Write(1, flipped, pos);
}

void report_errno(const char* what, long long code) {
    put_text(what);
    put_text(" failed: errno ");
    const unsigned long kMagnitude = code < 0 ? -static_cast<unsigned long>(code) : 0;
    put_decimal(kMagnitude);
    put_text("\n");
}

const char* or_root(const char* arg) {
    return arg == nullptr ? "/" : arg;
}

void run_echo(unsigned long words, const char* const* argv) {
    unsigned long end = words;
    for (unsigned long i = 1; i < words; ++i) {
        if (str_eq(argv[i], ">") && i + 1 < words) {
            end = i;
            break;
        }
    }
    if (end == words) {
        for (unsigned long i = 1; i < words; ++i) {
            if (i > 1) {
                put_text(" ");
            }
            put_text(argv[i]);
        }
        put_text("\n");
        return;
    }

    const long long kFile = user::Open(argv[end + 1], cinux::syscall::kOpenCreat);
    if (kFile < 0) {
        report_errno("open", kFile);
        return;
    }
    for (unsigned long i = 1; i < end; ++i) {
        if (i > 1) {
            user::Write(kFile, " ", 1);
        }
        user::Write(kFile, argv[i], str_len(argv[i]));
    }
    user::Write(kFile, "\n", 1);
    user::Close(kFile);
}

void run_ls(const char* arg) {
    const long long kDir = user::Open(or_root(arg), 0);
    if (kDir < 0) {
        report_errno("open", kDir);
        return;
    }
    cinux::syscall::SyscallDirent entry{};
    for (;;) {
        const long long kOne = user::Getdents(kDir, &entry);
        if (kOne == 0) {
            break;
        }
        if (kOne < 0) {
            report_errno("getdents", kOne);
            break;
        }
        put_text(entry.name);
        put_text(entry.type == cinux::syscall::kDirentDirectory ? "/\n" : "\n");
    }
    user::Close(kDir);
}

void run_cat(const char* arg) {
    const long long kFile = user::Open(or_root(arg), 0);
    if (kFile < 0) {
        report_errno("open", kFile);
        return;
    }
    char chunk[128];
    for (;;) {
        const long long kGot = user::Read(kFile, chunk, sizeof(chunk));
        if (kGot <= 0) {
            break;
        }
        user::Write(1, chunk, static_cast<unsigned long long>(kGot));
    }
    user::Close(kFile);
}

void run_touch(const char* arg) {
    const long long kFile = user::Open(or_root(arg), cinux::syscall::kOpenCreat);
    if (kFile < 0) {
        report_errno("open", kFile);
        return;
    }
    user::Close(kFile);
}

void run_mkdir(const char* arg) {
    const long long kMade = user::Mkdir(or_root(arg));
    if (kMade < 0) {
        report_errno("mkdir", kMade);
    }
}

void run_rm(const char* arg) {
    const long long kGone = user::Unlink(or_root(arg));
    if (kGone < 0) {
        report_errno("rm", kGone);
    }
}

void run_cd(const char* arg) {
    const long long kMoved = user::Chdir(or_root(arg));
    if (kMoved < 0) {
        report_errno("cd", kMoved);
    }
}

void run_pwd() {
    char            path[cinux::fs::kFsPathMax];
    const long long kLen = user::Getcwd(path, sizeof(path));
    if (kLen < 0) {
        report_errno("getcwd", kLen);
        return;
    }
    put_text(path);
    put_text("\n");
}

void run_stat(const char* arg) {
    // NOLINTNEXTLINE(misc-const-correctness) the kernel fills it via the syscall
    cinux::syscall::SyscallStat probed{};
    const long long             kAsked = user::Stat(or_root(arg), &probed);
    if (kAsked < 0) {
        report_errno("stat", kAsked);
        return;
    }
    put_text("  inode: ");
    put_decimal(probed.ino);
    put_text("\n   mode: ");
    put_decimal(probed.mode);
    put_text("\n  links: ");
    put_decimal(probed.links);
    put_text("\n   size: ");
    put_decimal(probed.size);
    put_text("\n  mtime: ");
    put_decimal(probed.mtime);
    put_text("\n");
}

void run_help() {
    put_text(
        "commands: echo [text...] [> file], ls [dir], cat <file>, stat <path>, "
        "touch <file>, mkdir <dir>, rm <path>, cd [dir], pwd, run <prog>, "
        "help, yield, exit, poweroff\n");
}

void report_child(long long pid, int code) {
    const auto kStatus = static_cast<unsigned long long>(code);
    put_text("[");
    put_decimal(static_cast<unsigned long long>(pid));
    if ((kStatus & 0xFFULL) != 0ULL) {
        put_text("] killed by signal ");
        put_decimal(kStatus & 0xFFULL);
    } else {
        put_text("] exited with ");
        put_decimal((kStatus >> 8ULL) & 0xFFULL);
    }
    put_text("\n");
}

void put_signed(long long value) {
    if (value < 0) {
        put_text("-");
        value = -value;
    }
    put_decimal(static_cast<unsigned long long>(value));
}

void run_program(const char* path) {
    if (path == nullptr) {
        put_text("run: need a program path\n");
        return;
    }
    const long long kPid = user::Fork();
    if (kPid < 0) {
        put_text("run: fork refused ");
        put_signed(kPid);
        put_text("\n");
        return;
    }
    if (kPid == 0) {
        user::Execve(path);
        put_text("run: exec refused\n");
        user::Exit(1);
    }
    int             code    = 0;
    const long long kReaped = user::Wait4(-1, &code);
    if (kReaped < 0) {
        put_text("run: wait refused ");
        put_signed(kReaped);
        put_text("\n");
        return;
    }
    report_child(kReaped, code);
}

void run_yield() {
    user::Yield();
    put_text("yielded and back\n");
}

void run_exit() {
    user::Exit(0);
}

void run_poweroff() {
    put_text("cutting power through acpi\n");
    const long long kOff = user::Reboot();
    if (kOff < 0) {
        report_errno("poweroff", kOff);
    }
    put_text("machine still standing; firmware ignored the sleep word\n");
}

void run_unknown(const char* word) {
    put_text("unknown command: ");
    put_text(word);
    put_text("\n");
}

void dispatch(unsigned long      words,
              const char* const* argv) {  // NOLINT(misc-const-correctness) the parser hands a
                                          // mutable buffer's view
    if (str_eq(argv[0], "echo")) {
        run_echo(words, argv);
        return;
    }
    if (str_eq(argv[0], "ls")) {
        run_ls(argv[1]);
    } else if (str_eq(argv[0], "cat")) {
        run_cat(argv[1]);
    } else if (str_eq(argv[0], "touch")) {
        run_touch(argv[1]);
    } else if (str_eq(argv[0], "mkdir")) {
        run_mkdir(argv[1]);
    } else if (str_eq(argv[0], "rm")) {
        run_rm(argv[1]);
    } else if (str_eq(argv[0], "cd")) {
        run_cd(argv[1]);
    } else if (str_eq(argv[0], "pwd")) {
        run_pwd();
    } else if (str_eq(argv[0], "stat")) {
        run_stat(argv[1]);
    } else if (str_eq(argv[0], "run")) {
        run_program(argv[1]);
    } else if (str_eq(argv[0], "help")) {
        run_help();
    } else if (str_eq(argv[0], "yield")) {
        run_yield();
    } else if (str_eq(argv[0], "exit")) {
        run_exit();
    } else if (str_eq(argv[0], "poweroff")) {
        run_poweroff();
    } else {
        run_unknown(argv[0]);
    }
}

}  // namespace

// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" void _start() __attribute__((section(".text.start")));
extern "C" void _start() {
    put_text("cinux shell - type 'help'\n");
    char  line[kMaxLine];
    char* words[kMaxWords];
    for (;;) {
        put_text("cinux> ");
        if (read_line(line) == 0) {
            continue;
        }
        const unsigned long kCount = tokenize(line, words);
        if (kCount > 0) {
            dispatch(kCount, words);
        }
    }
}
