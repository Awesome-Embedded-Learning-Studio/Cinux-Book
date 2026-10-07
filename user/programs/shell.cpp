#include "api/syscall.hpp"
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
    return count;
}

void make_path(char* out, const char* arg) {
    unsigned long i = 0;
    if (arg[0] != '/') {
        out[0] = '/';
        i      = 1;
    }
    for (unsigned long j = 0; arg[j] != '\0' && i < 95; ++j) {
        out[i] = arg[j];
        ++i;
    }
    out[i] = '\0';
}

void report_errno(const char* what, long long code) {
    put_text(what);
    put_text(" failed: errno ");
    char          digits[8];
    unsigned long pos       = 0;
    unsigned long magnitude = code < 0 ? -static_cast<unsigned long>(code) : 0;
    if (magnitude == 0) {
        digits[0] = '0';
        pos       = 1;
    }
    while (magnitude > 0) {
        digits[pos] = static_cast<char>('0' + (magnitude % 10));
        ++pos;
        magnitude /= 10;
    }
    char flipped[8];
    for (unsigned long j = 0; j < pos; ++j) {
        flipped[j] = digits[pos - 1 - j];
    }
    user::Write(1, flipped, pos);
    put_text("\n");
}

void run_echo(unsigned long words, char** argv) {
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

    char path[96];
    make_path(path, argv[end + 1]);
    const long long kFile = user::Open(path, cinux::syscall::kOpenCreat);
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
    char path[96];
    make_path(path, arg);
    const long long kDir = user::Open(path, 0);
    if (kDir < 0) {
        report_errno("open", kDir);
        return;
    }
    cinux::syscall::SyscallDirent entry{};
    for (;;) {
        const long long kOne = user::Getdents(kDir, &entry);
        if (kOne <= 0) {
            break;
        }
        put_text(entry.name);
        put_text(entry.type == 1 ? "/\n" : "\n");
    }
    user::Close(kDir);
}

void run_cat(const char* arg) {
    char path[96];
    make_path(path, arg);
    const long long kFile = user::Open(path, 0);
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
    char path[96];
    make_path(path, arg);
    const long long kFile = user::Open(path, cinux::syscall::kOpenCreat);
    if (kFile < 0) {
        report_errno("open", kFile);
        return;
    }
    user::Close(kFile);
}

void run_mkdir(const char* arg) {
    char path[96];
    make_path(path, arg);
    const long long kMade = user::Mkdir(path);
    if (kMade < 0) {
        report_errno("mkdir", kMade);
    }
}

void run_rm(const char* arg) {
    char path[96];
    make_path(path, arg);
    const long long kGone = user::Unlink(path);
    if (kGone < 0) {
        report_errno("rm", kGone);
    }
}

void run_help() {
    put_text(
        "commands: echo [text...] [> file], ls [dir], cat <file>, touch <file>, "
        "mkdir <dir>, rm <path>, help, yield, exit\n");
}

void run_yield() {
    user::Yield();
    put_text("yielded and back\n");
}

void run_exit() {
    user::Exit(0);
}

void run_unknown(const char* word) {
    put_text("unknown command: ");
    put_text(word);
    put_text("\n");
}

void dispatch(unsigned long words, char** argv) {
    if (str_eq(argv[0], "echo")) {
        run_echo(words, argv);
        return;
    }
    if (words < 2) {
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
    } else if (str_eq(argv[0], "help")) {
        run_help();
    } else if (str_eq(argv[0], "yield")) {
        run_yield();
    } else if (str_eq(argv[0], "exit")) {
        run_exit();
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
