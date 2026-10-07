#include "api/syscall.hpp"

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

void run_echo(unsigned long words, char** argv) {
    for (unsigned long i = 1; i < words; ++i) {
        if (i > 1) {
            put_text(" ");
        }
        put_text(argv[i]);
    }
    put_text("\n");
}

void run_help() {
    put_text("commands: echo <text...>, help, yield, exit\n");
}

void run_yield() {
    user::Yield();
    put_text("yielded and back\n");
}

}  // namespace

// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" void _start() {
    put_text("cinux shell - type 'help'\n");
    char  line[kMaxLine];
    char* words[kMaxWords];
    for (;;) {
        put_text("cinux> ");
        if (read_line(line) == 0) {
            continue;
        }
        unsigned long const kCount = tokenize(line, words);
        if (kCount == 0) {
            continue;
        }
        if (str_eq(words[0], "echo")) {
            run_echo(kCount, words);
        } else if (str_eq(words[0], "help")) {
            run_help();
        } else if (str_eq(words[0], "yield")) {
            run_yield();
        } else if (str_eq(words[0], "exit")) {
            user::Exit(0);
        } else {
            put_text("unknown command: ");
            put_text(words[0]);
            put_text("\n");
        }
    }
}
