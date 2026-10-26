#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <asm-generic/errno-base.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>

static void fill_text(char* text, unsigned long count) {
    for (unsigned long index = 0; index < count; ++index) {
        text[index] = 'x';
    }
}

static int run_child(char* const* arguments, char* const* environment) {
    const pid_t kPid = fork();
    if (kPid < 0) {
        return 0;
    }
    if (kPid == 0) {
        syscall(SYS_execve, "/bin/execprobe", arguments, environment);
        _exit(100 + errno);
    }
    int status = 0;
    return waitpid(kPid, &status, 0) == kPid && WIFEXITED(status) && WEXITSTATUS(status) == 42;
}

static int expect_error(char* const* arguments, int expected) {
    const pid_t kBefore = getpid();
    const int   kFd     = open("/hello.c", O_RDONLY | O_CLOEXEC);
    if (kFd < 0) {
        return 0;
    }
    errno              = 0;
    const long kResult = syscall(SYS_execve, "/bin/execprobe", arguments, NULL);
    const int  kError  = errno;
    const int  kAlive  = getpid() == kBefore && fcntl(kFd, F_GETFD) == FD_CLOEXEC;
    close(kFd);
    return kResult == -1 && kError == expected && kAlive;
}

static int check_payload(int argc, char** argv, char** environment) {
    for (int index = 2; index < argc; ++index) {
        for (const char* scan = argv[index]; *scan != '\0'; ++scan) {
            if (*scan != 'x') {
                return 1;
            }
        }
    }
    for (unsigned long index = 0; environment[index] != NULL; ++index) {
        if (strcmp(environment[index], "EXEC_PROBE=yes") != 0) {
            return 1;
        }
    }
    return 42;
}

static int child_mode(int argc, char** argv, char** environment) {
    if (argc == 1 && argv[0][0] == '\0' && environment[0] == NULL) {
        return 42;
    }
    if (argc > 1 && strcmp(argv[1], "verify") == 0) {
        return check_payload(argc, argv, environment);
    }
    return -1;
}

static unsigned long page_size(void) {
    const long kPageSize = sysconf(_SC_PAGESIZE);
    if (kPageSize <= 0 || kPageSize > (LONG_MAX / 32) - 1) {
        return 0;
    }
    return (unsigned long)kPageSize;
}

static int check_brk(unsigned long page) {
    const unsigned long kOriginal = (unsigned long)syscall(SYS_brk, 0);
    const unsigned long kPageBase = ((kOriginal + page - 1) / page) * page;
    const unsigned long kTarget   = kPageBase + (2 * page) + 17;
    int                 result    = 0;
    if ((unsigned long)syscall(SYS_brk, kTarget) != kTarget) {
        goto cleanup;
    }
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    unsigned char* const kBytes = (unsigned char*)kPageBase;
    kBytes[0]                   = 0x5a;
    kBytes[page]                = 0xa5;
    if ((unsigned long)syscall(SYS_brk, kPageBase + 31) != kPageBase + 31 ||
        (unsigned long)syscall(SYS_brk, kTarget) != kTarget || kBytes[0] != 0x5a) {
        goto cleanup;
    }
    for (unsigned long index = page; index < 2 * page; ++index) {
        if (kBytes[index] != 0) {
            goto cleanup;
        }
    }
    result = (unsigned long)syscall(SYS_brk, ULONG_MAX) == kTarget;
cleanup:
    return (unsigned long)syscall(SYS_brk, kOriginal) == kOriginal && result;
}

static int fill_fd_limit(int* descriptors, unsigned long count) {
    for (unsigned long index = 0; index < count; ++index) {
        descriptors[index] = open("/hello.c", O_RDONLY | O_CLOEXEC);
        if (descriptors[index] < 0) {
            return 0;
        }
    }
    errno            = 0;
    const int kExtra = open("/hello.c", O_RDONLY);
    if (kExtra >= 0) {
        close(kExtra);
        return 0;
    }
    return errno == EMFILE;
}

static int check_lowered_fd_limit(int highest, const struct rlimit* original) {
    const struct rlimit kLow = {.rlim_cur = 64, .rlim_max = original->rlim_max};
    if (syscall(SYS_prlimit64, 0, RLIMIT_NOFILE, &kLow, NULL) != 0 ||
        fcntl(highest, F_GETFD) != FD_CLOEXEC) {
        return 0;
    }
    errno          = 0;
    const int kDup = fcntl(highest, F_DUPFD, 64);
    return kDup == -1 && errno == EINVAL;
}

static int check_fd_limit(void) {
    struct rlimit original = {0};
    if (syscall(SYS_prlimit64, 0, RLIMIT_NOFILE, NULL, &original) != 0) {
        return 0;
    }
    const struct rlimit kLimit = {.rlim_cur = 260, .rlim_max = original.rlim_max};
    int                 descriptors[257];
    for (unsigned long index = 0; index < 257; ++index) {
        descriptors[index] = -1;
    }
    int result = 0;
    if (syscall(SYS_prlimit64, 0, RLIMIT_NOFILE, &kLimit, NULL) == 0) {
        result =
            fill_fd_limit(descriptors, 257) && check_lowered_fd_limit(descriptors[256], &original);
    }
    if (syscall(SYS_prlimit64, 0, RLIMIT_NOFILE, &original, NULL) != 0) {
        result = 0;
    }
    for (unsigned long index = 0; index < 257; ++index) {
        if (descriptors[index] >= 0) {
            close(descriptors[index]);
        }
    }
    return result;
}

static int ready_runtime(unsigned long page) {
    if (page != 0 && check_brk(page) && check_fd_limit()) {
        return 1;
    }
    puts("EXEC_PROBE_FAIL brk_or_fd_limit");
    return 0;
}

int main(int argc, char** argv, char** environment) {
    const int kMode = child_mode(argc, argv, environment);
    if (kMode >= 0) {
        return kMode;
    }
    const unsigned long kPage = page_size();
    if (!ready_runtime(kPage)) {
        return 1;
    }
    const unsigned long kMaxString   = kPage * 32;
    char* const         kText        = malloc(kMaxString + 1);
    char** const        kArguments   = (char**)calloc(1025, sizeof(*kArguments));
    char** const        kEnvironment = (char**)calloc(301, sizeof(*kEnvironment));
    int                 result       = 1;
    if (kText == NULL || kArguments == NULL || kEnvironment == NULL) {
        goto cleanup;
    }
    fill_text(kText, kMaxString);
    kText[32]     = '\0';
    kArguments[0] = "/bin/execprobe";
    kArguments[1] = "verify";
    for (unsigned long index = 2; index < 1024; ++index) {
        kArguments[index] = kText;
    }
    for (unsigned long index = 0; index < 300; ++index) {
        kEnvironment[index] = "EXEC_PROBE=yes";
    }
    if (!run_child(kArguments, kEnvironment)) {
        puts("EXEC_PROBE_FAIL multipage");
        goto cleanup;
    }
    kArguments[3] = NULL;
    fill_text(kText, kMaxString);
    kText[kMaxString - 1] = '\0';
    if (!run_child(kArguments, NULL)) {
        puts("EXEC_PROBE_FAIL string_boundary");
        goto cleanup;
    }
    kText[kMaxString - 1] = 'x';
    kText[kMaxString]     = '\0';
    if (!expect_error(kArguments, E2BIG)) {
        puts("EXEC_PROBE_FAIL string_limit");
        goto cleanup;
    }
    kText[(16 * kPage) - 1] = '\0';
    for (unsigned long index = 2; index < 26; ++index) {
        kArguments[index] = kText;
    }
    kArguments[26] = NULL;
    if (!run_child(kArguments, NULL)) {
        puts("EXEC_PROBE_FAIL large_payload");
        goto cleanup;
    }
    struct rlimit old_limit = {0};
    struct rlimit limit     = {.rlim_cur = 4UL * 1024 * 1024, .rlim_max = RLIM_INFINITY};
    if (syscall(SYS_prlimit64, 0, RLIMIT_STACK, &limit, &old_limit) != 0 ||
        !expect_error(kArguments, E2BIG)) {
        puts("EXEC_PROBE_FAIL stack_limit");
        goto cleanup;
    }
    limit.rlim_cur          = 256UL * 1024;
    kArguments[3]           = NULL;
    kText[(25 * kPage) - 1] = '\0';
    fill_text(kText, (25 * kPage) - 1);
    if (syscall(SYS_prlimit64, 0, RLIMIT_STACK, &limit, NULL) != 0 ||
        !run_child(kArguments, NULL) ||
        syscall(SYS_prlimit64, 0, RLIMIT_STACK, &old_limit, NULL) != 0) {
        puts("EXEC_PROBE_FAIL arg_max_floor");
        goto cleanup;
    }
    char* bad_arguments[] = {"/bin/execprobe", (char*)1, NULL};
    if (!expect_error((char* const*)1, EFAULT) || !expect_error(bad_arguments, EFAULT) ||
        !run_child(NULL, NULL)) {
        puts("EXEC_PROBE_FAIL fault_or_null");
        goto cleanup;
    }
    result = 0;
    puts(
        "EXEC_PROBE_PASS fd_growth_rlimit brk_exact_shrink_zero multipage string_limit rlimit "
        "rollback "
        "null_vectors");
cleanup:
    free((void*)kEnvironment);
    free((void*)kArguments);
    free(kText);
    return result;
}
