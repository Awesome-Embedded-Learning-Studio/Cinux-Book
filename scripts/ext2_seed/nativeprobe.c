#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/wait.h>

static void check(int condition, const char* label) {
    if (!condition) {
        perror(label);
        exit(1);
    }
}

static volatile sig_atomic_t g_notified;
static void                  on_child(int number __attribute__((unused))) {
    g_notified = 1;
}

static void check_temporary_signal_mask(void) {
    struct sigaction action = {0};
    action.sa_handler       = on_child;
    sigemptyset(&action.sa_mask);
    check(sigaction(SIGCHLD, &action, NULL) == 0, "install child handler");
    // NOLINTNEXTLINE(misc-include-cleaner) glibc private bits headers provide it
    sigset_t child_mask;
    sigset_t before;
    sigset_t current;
    sigemptyset(&child_mask);
    sigaddset(&child_mask, SIGCHLD);
    check(sigprocmask(SIG_BLOCK, &child_mask, &before) == 0, "block child signal");
    pid_t child = fork();
    check(child >= 0, "fork signal probe");
    if (child == 0) {
        _exit(0);
    }
    struct timespec timeout = {2, 0};
    // NOLINTNEXTLINE(misc-include-cleaner) glibc private bits headers provide it
    check(ppoll(NULL, 0, &timeout, &before) == -1 && errno == EINTR, "masked wait interruption");
    // NOLINTNEXTLINE(misc-include-cleaner) glibc private bits headers provide it
    check(g_notified == 1, "masked wait delivered child handler");
    check(sigprocmask(SIG_SETMASK, NULL, &current) == 0 && sigismember(&current, SIGCHLD) == 1,
          "original mask restored after handler");
    int status = 0;
    check(waitpid(child, &status, 0) == child && WIFEXITED(status), "reap child after masked wait");
    check(sigprocmask(SIG_SETMASK, &before, NULL) == 0, "restore signal probe mask");
}

static void check_directory_relative_paths(void) {
    int directory = open("/tmp", O_RDONLY | O_DIRECTORY);
    check(directory >= 0, "open directory");
    int file = openat(directory, "native-at", O_CREAT | O_RDWR | O_TRUNC, 0640);
    check(file >= 0 && write(file, "x", 1) == 1, "create directory-relative file");
    struct stat facts;
    check(fstatat(directory, "native-at", &facts, 0) == 0 && facts.st_size == 1 &&
              (facts.st_mode & 0777U) == 0640,
          "directory-relative metadata and creation mode");
    // NOLINTNEXTLINE(misc-include-cleaner) glibc private bits headers provide it
    check(openat(file, "child", O_RDONLY) == -1 && errno == ENOTDIR,
          // NOLINTNEXTLINE(misc-include-cleaner) glibc private bits headers provide it
          "relative path rejects nondirectory");
    check(close(file) == 0 && close(directory) == 0, "close directory-relative files");
    check(unlink("/tmp/native-at") == 0, "remove directory-relative file");
    file = creat("/tmp/native-creat", 0600);
    check(file >= 0 && close(file) == 0 && unlink("/tmp/native-creat") == 0, "creat syscall");
}

static void check_cold_mapped_buffer(int loop_fd) {
    int first_counter  = eventfd(0, EFD_CLOEXEC);
    int second_counter = eventfd(0, EFD_CLOEXEC);
    check(first_counter >= 0 && second_counter >= 0, "cold-buffer counters");
    unsigned long long wake = 1;
    check(write(first_counter, &wake, sizeof(wake)) == sizeof(wake), "wake cold first");
    check(write(second_counter, &wake, sizeof(wake)) == sizeof(wake), "wake cold second");
    struct epoll_event left  = {.events = EPOLLIN, .data.u64 = 777};
    struct epoll_event right = {.events = EPOLLIN, .data.u64 = 888};
    check(epoll_ctl(loop_fd, EPOLL_CTL_ADD, first_counter, &left) == 0, "add cold first");
    check(epoll_ctl(loop_fd, EPOLL_CTL_ADD, second_counter, &right) == 0, "add cold second");
    int holder = open("/tmp/epoll-cold", O_CREAT | O_TRUNC | O_RDWR, 0600);
    check(holder >= 0, "open cold buffer file");
    const char kLast = 0;
    check(lseek(holder, 4095, SEEK_SET) == 4095 && write(holder, &kLast, 1) == 1,
          "size cold buffer file");
    void* mapped = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, holder, 0);
    check(mapped != MAP_FAILED, "map cold buffer");
    check(epoll_wait(loop_fd, mapped, 2, 0) == 2, "drain both into a cold mapped buffer");
    check(((struct epoll_event*)mapped)[0].data.u64 != ((struct epoll_event*)mapped)[1].data.u64,
          "both counters answered");
    unsigned long long drained = 0;
    check(read(first_counter, &drained, sizeof(drained)) == sizeof(drained), "drain cold first");
    check(read(second_counter, &drained, sizeof(drained)) == sizeof(drained), "drain cold second");
    check(epoll_wait(loop_fd, mapped, 1, 0) == 0, "cold buffer drained clean");
    check(munmap(mapped, 4096) == 0 && unlink("/tmp/epoll-cold") == 0 && close(holder) == 0,
          "release cold buffer");
    check(close(first_counter) == 0 && close(second_counter) == 0, "close cold counters");
}

static void check_epoll_rotation(int loop_fd) {
    int first_counter  = eventfd(0, EFD_NONBLOCK);
    int second_counter = eventfd(0, EFD_NONBLOCK);
    check(first_counter >= 0 && second_counter >= 0, "rotation counters");
    unsigned long long wake = 1;
    check(write(first_counter, &wake, sizeof(wake)) == sizeof(wake), "wake rotation first");
    check(write(second_counter, &wake, sizeof(wake)) == sizeof(wake), "wake rotation second");
    struct epoll_event left  = {.events = EPOLLIN, .data.u64 = 1};
    struct epoll_event right = {.events = EPOLLIN, .data.u64 = 2};
    check(epoll_ctl(loop_fd, EPOLL_CTL_ADD, first_counter, &left) == 0, "add rotation first");
    check(epoll_ctl(loop_fd, EPOLL_CTL_ADD, second_counter, &right) == 0, "add rotation second");
    struct epoll_event seen;
    check(epoll_wait(loop_fd, &seen, 1, 0) == 1, "rotation turn one");
    const unsigned long long kTurnOne = seen.data.u64;
    check(epoll_wait(loop_fd, &seen, 1, 0) == 1 && seen.data.u64 != kTurnOne, "rotation turn two");
    check(epoll_wait(loop_fd, &seen, 1, 0) == 1 && seen.data.u64 == kTurnOne,
          "rotation turn three");
    check(close(first_counter) == 0 && close(second_counter) == 0, "close rotation counters");
}

static void check_eventfd_edges(int loop_fd) {
    int counter = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    check(counter >= 0, "edge counter");
    struct epoll_event interest = {.events = EPOLLIN | EPOLLET, .data.u64 = 919};
    struct epoll_event seen = {0};
    check(epoll_ctl(loop_fd, EPOLL_CTL_ADD, counter, &interest) == 0, "add edge counter");
    check(epoll_wait(loop_fd, &seen, 1, 0) == 0, "empty edge counter");
    uint64_t one = 1;
    for (int turn = 0; turn < 3; ++turn) {
        check(write(counter, &one, sizeof(one)) == sizeof(one), "write edge counter");
        check(epoll_wait(loop_fd, &seen, 1, 0) == 1 && seen.data.u64 == 919 &&
                  seen.events == EPOLLIN, "each write notifies without a read");
        check(epoll_wait(loop_fd, &seen, 1, 0) == 0, "edge does not repeat while readable");
    }
    check(epoll_ctl(loop_fd, EPOLL_CTL_MOD, counter, &interest) == 0, "rearm readable edge");
    check(epoll_wait(loop_fd, &seen, 1, 0) == 1, "modify reports current readiness");
    check(epoll_wait(loop_fd, &seen, 1, 0) == 0, "modified edge consumed once");
    uint64_t taken = 0;
    check(read(counter, &taken, sizeof(taken)) == sizeof(taken) && taken == 3,
          "drain edge counter");
    check(write(counter, &one, sizeof(one)) == sizeof(one), "wake drained edge counter");
    check(epoll_wait(loop_fd, &seen, 1, 0) == 1, "edge after drain");
    check(close(counter) == 0, "close edge counter");
    check(epoll_wait(loop_fd, &seen, 1, 0) == 0, "closed edge removed");
}

static void check_blocking_counter_write(void) {
    int counter = eventfd(0, 0);
    check(counter >= 0, "blocking counter");
    unsigned long long fill = ~0ULL - 1;
    check(write(counter, &fill, sizeof(fill)) == sizeof(fill), "fill blocking counter");
    const pid_t kReader = fork();
    check(kReader >= 0, "fork counter reader");
    if (kReader == 0) {
        unsigned long long taken = 0;
        _exit(read(counter, &taken, sizeof(taken)) == sizeof(taken) && taken == fill ? 0 : 2);
    }
    unsigned long long one = 1;
    check(write(counter, &one, sizeof(one)) == sizeof(one),
          "full blocking write waits for the reader");
    int status = 0;
    check(waitpid(kReader, &status, 0) == kReader && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "reap counter reader");
    check(close(counter) == 0, "close blocking counter");
}

static void check_masked_epoll_wait(int loop_fd) {
    sigset_t everything;
    sigfillset(&everything);
    struct epoll_event seen;
    check(epoll_pwait(loop_fd, &seen, 1, 0, &everything) == 0,
          "kill and stop stripped from the wait mask");
}

int main(void) {
    puts("nativeprobe: signal mask");
    check_temporary_signal_mask();
    puts("nativeprobe: directory identity");
    check_directory_relative_paths();
    int loop = epoll_create1(EPOLL_CLOEXEC);
    int wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    check(loop >= 0 && wake >= 0, "create loop/counter");
    // NOLINTNEXTLINE(misc-include-cleaner) glibc private bits headers provide it
    check(fsync(wake) == -1 && errno == EINVAL, "anonymous descriptor fsync");
    // NOLINTNEXTLINE(misc-include-cleaner) glibc private bits headers provide it
    struct epoll_event interest = {.events = EPOLLIN, .data.u64 = 12345};
    struct epoll_event ready    = {0};
    check(epoll_ctl(loop, EPOLL_CTL_ADD, wake, &interest) == 0, "add counter");
    check(epoll_wait(loop, &ready, 1, 0) == 0, "empty counter readiness");
    uint64_t value = 7;
    check(write(wake, &value, sizeof(value)) == sizeof(value), "wake counter");
    // NOLINTNEXTLINE(misc-include-cleaner) glibc private bits headers provide it
    check(epoll_wait(loop, &ready, 1, 0) == 1 && ready.data.u64 == 12345, "counter readiness/data");
    check(epoll_wait(loop, &ready, 1, 0) == 1, "level readiness");
    check(read(wake, &value, sizeof(value)) == sizeof(value) && value == 7, "drain counter");
    // NOLINTNEXTLINE(misc-include-cleaner) glibc private bits headers provide it
    check(read(wake, &value, sizeof(value)) == -1 && errno == EAGAIN, "nonblocking counter");
    int alias = dup(wake);
    check(alias >= 0 && close(wake) == 0, "duplicate counter lifetime");
    value = 1;
    check(write(alias, &value, sizeof(value)) == sizeof(value), "wake through duplicate");
    check(epoll_wait(loop, &ready, 1, 0) == 1, "watch survives duplicate");
    check(close(alias) == 0, "last counter close");
    wake = eventfd(1, EFD_NONBLOCK);
    check(wake >= 0, "reuse descriptor");
    check(epoll_wait(loop, &ready, 1, 0) == 0, "last close removes watch");
    check(close(wake) == 0, "close replacement");

    puts("nativeprobe: cold file-backed buffer");
    check_cold_mapped_buffer(loop);

    puts("nativeprobe: epoll rotation");
    check_epoll_rotation(loop);

    puts("nativeprobe: eventfd edges");
    check_eventfd_edges(loop);

    puts("nativeprobe: blocking counter");
    check_blocking_counter_write();
    check_masked_epoll_wait(loop);

    int pipefd[2];
    check(pipe2(pipefd, O_CLOEXEC | O_NONBLOCK) == 0, "nonblocking pipe2");
    check(((unsigned int)fcntl(pipefd[0], F_GETFL) & (unsigned int)O_NONBLOCK) != 0,
          "pipe read flags");
    check(((unsigned int)fcntl(pipefd[1], F_GETFL) & (unsigned int)O_NONBLOCK) != 0,
          "pipe write flags");
    // NOLINTNEXTLINE(misc-include-cleaner) glibc private bits headers provide it
    int enabled = 0;
    // NOLINTNEXTLINE(misc-include-cleaner) glibc private bits headers provide it
    check(ioctl(pipefd[0], FIONBIO, &enabled) == 0, "clear FIONBIO");
    check(((unsigned int)fcntl(pipefd[0], F_GETFL) & (unsigned int)O_NONBLOCK) == 0,
          "FIONBIO flags");
    check(epoll_ctl(loop, EPOLL_CTL_ADD, pipefd[0], &interest) == 0, "add pipe");
    check(write(pipefd[1], "x", 1) == 1, "write pipe");
    check(epoll_wait(loop, &ready, 1, 0) == 1 && (ready.events & EPOLLIN), "pipe readiness");
    char byte;
    check(read(pipefd[0], &byte, 1) == 1 && byte == 'x', "read pipe");
    check(close(pipefd[1]) == 0, "close writer");
    check(epoll_wait(loop, &ready, 1, 0) == 1 && (ready.events & EPOLLHUP), "pipe hangup");
    check(close(pipefd[0]) == 0 && close(loop) == 0, "close loop");
    int output = open("/tmp/native-writev", O_CREAT | O_TRUNC | O_RDWR, 0600);
    check(output >= 0, "open writev file");
    char first[1500];
    char second[300];
    char result[1800];
    for (size_t index = 0; index < sizeof(first); ++index) {
        first[index] = 'A';
    }
    // NOLINTNEXTLINE(misc-include-cleaner) glibc private bits headers provide it
    for (size_t index = 0; index < sizeof(second); ++index) {
        second[index] = 'B';
    }
    // NOLINTNEXTLINE(misc-include-cleaner) glibc private bits headers provide it
    struct iovec runs[2] = {{first, sizeof(first)}, {second, sizeof(second)}};
    int          index   = 0;
    while (index < 2) {
        ssize_t put = writev(output, runs + index, 2 - index);
        check(put > 0, "writev progress");
        while (index < 2 && (size_t)put >= runs[index].iov_len) {
            put -= (ssize_t)runs[index].iov_len;
            ++index;
        }
        if (index < 2) {
            runs[index].iov_base = (char*)runs[index].iov_base + put;
            runs[index].iov_len -= (size_t)put;
        }
    }
    check(lseek(output, 0, SEEK_SET) == 0, "rewind writev file");
    size_t received = 0;
    while (received < sizeof(result)) {
        ssize_t got = read(output, result + received, sizeof(result) - received);
        check(got > 0, "read writev file");
        received += (size_t)got;
    }
    check(memcmp(result, first, sizeof(first)) == 0, "writev first segment preserved");
    check(memcmp(result + sizeof(first), second, sizeof(second)) == 0,
          "writev second segment preserved");
    check(close(output) == 0, "close writev file");
    check(rename("/tmp/native-writev", "/tmp/native-moved") == 0, "rename file");
    check(chmod("/tmp/native-moved", 0640) == 0, "chmod file");
    struct stat facts;
    check(stat("/tmp/native-moved", &facts) == 0 && (facts.st_mode & 0777U) == 0640,
          "persistent permissions");
    check(unlink("/tmp/native-moved") == 0, "unlink probe file");
    puts("nativeprobe: epoll, eventfd, duplicate lifetime, pipe2, FIONBIO passed");
    return 0;
}
