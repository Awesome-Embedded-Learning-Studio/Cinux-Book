/**
 * @file user/programs/shell/main.cpp
 * @brief Shell REPL main loop, line reader, tokenizer, and dispatch
 *
 * Entry point for the Cinux user-space shell.  Runs the main loop:
 *   print_prompt -> read_line -> tokenize -> dispatch -> repeat
 *
 * The builtin command table is defined here; each command handler lives
 * in its own .cpp file (cmd_echo.cpp, cmd_help.cpp, cmd_clear.cpp).
 */

#include "libc/string.hpp"
#include "libc/syscall.h"
#include "shell.hpp"

using cinux::user::strcmp;
using cinux::user::strlen;

// ============================================================
// Constants
// ============================================================

/// Maximum input line length (including NUL terminator)
constexpr size_t MAX_LINE = 256;

/// Maximum number of tokens after splitting
constexpr size_t MAX_TOKENS = 16;

/// Shell prompt string
constexpr const char PROMPT[] = "cinux> ";

// ============================================================
// I/O helpers
// ============================================================

namespace {

void write_str(const char* s) {
    sys_write(1, s, strlen(s));
}

void write_buf(const char* buf, size_t len) {
    sys_write(1, buf, len);
}

/// Launch an external program by path (the standard shell fallback when a token
/// matches no builtin): fork, the child execve()s @p path, the parent
/// waitpid()s.  Lets the old user/libc shell start real musl binaries (e.g.
/// typing /hello runs the musl static hello).  Returns the child exit status,
/// or a negative errno if fork/exec failed.
int64_t launch_program(const char* path, char** argv) {
    int64_t pid = sys_fork();
    if (pid < 0) {
        return pid;  // fork error
    }
    if (pid == 0) {
        char* child_envp[1] = {nullptr};
        sys_execve(path, argv, child_envp);  // returns only on failure
        sys_exit(127);
    }
    int     status = 0;
    int64_t reaped = sys_waitpid(static_cast<int>(pid), &status, 0);
    return reaped > 0 ? static_cast<int64_t>(status) : reaped;
}

}  // anonymous namespace

// ============================================================
// Line reading
// ============================================================

namespace {

/**
 * @brief Read one line from stdin into @p buf
 *
 * Reads characters one at a time via sys_read(0, ...).
 * Backspace (0x7F) / '\\b' erases the previous character.
 * Enter ('\\n') terminates the line.  The buffer is NUL-terminated;
 * the trailing newline is NOT stored.
 *
 * @param buf Destination buffer
 * @param cap Buffer capacity (including NUL)
 * @return Number of characters stored (excluding NUL)
 */
size_t read_line(char* buf, size_t cap) {
    size_t pos = 0;

    while (pos < cap - 1) {
        char    c = 0;
        int64_t n = sys_read(0, &c, 1);
        if (n <= 0) {
            continue;
        }

        if (c == '\n') {
            write_buf("\n", 1);
            break;
        }

        if (c == 0x7F || c == '\b') {
            if (pos > 0) {
                --pos;
                write_buf("\b \b", 3);
            }
            continue;
        }

        // Echo back and store
        write_buf(&c, 1);
        buf[pos++] = c;
    }

    buf[pos] = '\0';
    return pos;
}

}  // anonymous namespace

// ============================================================
// Tokenizer
// ============================================================

namespace {

/**
 * @brief Tokenize a line by whitespace, in-place
 *
 * Splits @p line on space (' ') and tab ('\\t').
 * Token pointers are stored in @p argv.  The string @p line
 * is modified (whitespace replaced with NUL).
 *
 * @param line        Mutable input line
 * @param argv        Output array of token pointers
 * @param max_tokens  Maximum number of tokens to store
 * @return Number of tokens produced (argc)
 */
size_t tokenize(char* line, char** argv, size_t max_tokens) {
    size_t argc = 0;

    while (*line != '\0' && argc < max_tokens) {
        // Skip leading whitespace
        while (*line == ' ' || *line == '\t') {
            ++line;
        }
        if (*line == '\0') {
            break;
        }

        argv[argc++] = line;

        // Advance past non-whitespace
        while (*line != '\0' && *line != ' ' && *line != '\t') {
            ++line;
        }

        // NUL-terminate the token
        if (*line != '\0') {
            *line++ = '\0';
        }
    }

    argv[argc] = nullptr;  // terminate so consumers (launch_program/execve) stop correctly
    return argc;
}

}  // anonymous namespace

// ============================================================
// Command dispatch table (data-driven)
// ============================================================

namespace {

/// Built-in command dispatch table.
/// To add a new command: implement cmd_xxx in cmd_xxx.cpp, declare
/// it in shell.hpp, and add an entry here.
constexpr CmdEntry builtin_cmds[] = {
    {"echo", cmd_echo},   {"help", cmd_help},   {"clear", cmd_clear}, {"cat", cmd_cat},
    {"ls", cmd_ls},       {"touch", cmd_touch}, {"mkdir", cmd_mkdir}, {"rm", cmd_rm},
    {"rmdir", cmd_rmdir}, {"cd", cmd_cd},       {"pwd", cmd_pwd},     {"stat", cmd_stat},
    {"ping", cmd_ping},   {nullptr, nullptr},
};

}  // anonymous namespace

// ============================================================
// Shell main loop
// ============================================================

static void shell_main() {
    char  line[MAX_LINE];
    // MAX_TOKENS + 1 slots: tokenize() fills up to MAX_TOKENS pointers
    // (argv[0..MAX_TOKENS-1]) then writes a nullptr terminator at argv[argc] --
    // which lands at argv[MAX_TOKENS] when the line has exactly MAX_TOKENS
    // tokens. A [MAX_TOKENS] array made that terminator write out of bounds
    // (-Warray-bounds; a real stack smash under -O2).
    char* argv[MAX_TOKENS + 1];

    write_str("Cinux shell - type 'help' for commands\n");

    while (true) {
        // Print prompt
        write_str(PROMPT);

        // Read input line
        size_t len = read_line(line, MAX_LINE);
        if (len == 0) {
            continue;
        }

        // Tokenize
        size_t argc = tokenize(line, argv, MAX_TOKENS);
        if (argc == 0) {
            continue;
        }

        // Dispatch through builtin command table
        bool found = false;
        for (size_t i = 0; builtin_cmds[i].name != nullptr; ++i) {
            if (strcmp(argv[0], builtin_cmds[i].name) == 0) {
                builtin_cmds[i].handler(static_cast<int>(argc), argv);
                found = true;
                break;
            }
        }

        if (!found) {
            // No builtin matched -- treat the token as a program path and try
            // to fork+execve it (the standard shell fallback, like typing
            // /bin/ls in bash).  A negative result means fork/exec failed; the
            // launched program's own output (if it ran) already went to stdout.
            int64_t r = launch_program(argv[0], argv);
            // launch_program's child does sys_exit(127) when execve fails (the
            // shell convention for "command not found"); fork/exec errors come
            // back negative. Both -> print the not-found message.
            if (r < 0 || r == 127) {
                write_str(argv[0]);
                write_str(": command not found\n");
            }
        }
    }
}

// ============================================================
// ELF entry point
// ============================================================

extern "C" {

void _start() {
    shell_main();
    sys_exit(0);
}

}  // extern "C"
