#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <asm-generic/errno-base.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/poll.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>

static void require(int condition, const char* stage) {
    if (condition == 0) {
        const int kError = errno;
        fputs("ECO_FAIL ", stderr);
        fputs(stage, stderr);
        fputs(" errno=", stderr);
        fputs(strerror(kError), stderr);
        fputc('\n', stderr);
        exit(1);
    }
}

static void shared_memory(void) {
    const int kSegment = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0600);
    require(kSegment >= 0, "shmget");
    volatile int* const kPage = shmat(kSegment, NULL, 0);
    require(kPage != MAP_FAILED, "shmat");
    *kPage                = 41;
    struct shmid_ds facts = {0};
    require(
        shmctl(kSegment, IPC_STAT, &facts) == 0 && facts.shm_segsz == 4096 && facts.shm_nattch == 1,
        "shm-stat");
    const pid_t kChild = fork();
    require(kChild >= 0, "shm-fork");
    if (kChild == 0) {
        require(*kPage == 41, "shm-child-read");
        *kPage = 73;
        require(shmdt((const void*)kPage) == 0, "shm-child-detach");
        _exit(0);
    }
    require(shmctl(kSegment, IPC_RMID, NULL) == 0, "shm-rmid-live");
    int status = -1;
    require(waitpid(kChild, &status, 0) == kChild && status == 0 && *kPage == 73,
            "shm-child-writes-shared");
    require(shmdt((const void*)kPage) == 0, "shmdt");
    require(shmat(kSegment, NULL, 0) == MAP_FAILED, "shm-stale-id");
}

static void local_pair(void) {
    int pair[2];
    require(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair) == 0, "unix-pair");
    char payload[8] = {0};
    require(read(pair[1], payload, 1) == -1 && errno == EAGAIN, "unix-empty");
    require(write(pair[0], "hello", 5) == 5, "unix-write");
    struct pollfd record = {.fd = pair[1], .events = POLLIN, .revents = 0};
    require(poll(&record, 1, 0) == 1 && ((unsigned int)record.revents & (unsigned int)POLLIN) != 0,
            "unix-poll");
    require(read(pair[1], payload, 2) == 2 && read(pair[1], payload + 2, 6) == 3 &&
                memcmp(payload, "hello", 5) == 0,
            "unix-stream-short-read");
    require(shutdown(pair[0], SHUT_WR) == 0 && read(pair[1], payload, 1) == 0, "unix-eof");
    require(write(pair[1], "r", 1) == 1 && read(pair[0], payload, 1) == 1, "unix-half-close");
    close(pair[0]);
    close(pair[1]);
}

static void local_names(void) {
    const int          kListener = socket(AF_UNIX, SOCK_STREAM, 0);
    const int          kClient   = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un address   = {0};
    address.sun_family           = AF_UNIX;
    const char kName[]           = "cinux-eco";
    for (size_t index = 0; index < sizeof(kName) - 1; ++index) {
        address.sun_path[index + 1] = kName[index];
    }
    const socklen_t kLength = offsetof(struct sockaddr_un, sun_path) + 10;
    require(kListener >= 0 && kClient >= 0, "unix-named-create");
    require(bind(kListener, (struct sockaddr*)&address, kLength) == 0 && listen(kListener, 2) == 0,
            "unix-listen");
    require(connect(kClient, (struct sockaddr*)&address, kLength) == 0, "unix-connect");
    const int kAccepted = accept(kListener, NULL, NULL);
    require(kAccepted >= 0, "unix-accept");
    require(send(kClient, "n", 1, 0) == 1, "unix-send");
    char payload = 0;
    require(recv(kAccepted, &payload, 1, 0) == 1 && payload == 'n', "unix-recv");
    close(kAccepted);
    close(kClient);
    close(kListener);
    int pair[2];
    require(socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) == 0, "unix-dgram-pair");
    require(send(pair[0], "abc", 3, 0) == 3 && send(pair[0], "z", 1, 0) == 1, "unix-datagrams");
    require(recv(pair[1], &payload, 1, 0) == 1 && payload == 'a' &&
                recv(pair[1], &payload, 1, 0) == 1 && payload == 'z',
            "unix-dgram-boundary");
    close(pair[0]);
    close(pair[1]);
}

static void mount_lifetime(void) {
    const int kFile = open("/tmp/lifetime", O_CREAT | O_RDWR, 0600);
    require(kFile >= 0, "mount-open");
    require(umount("/tmp") == -1 && errno == EBUSY, "mount-busy");
    require(umount2("/tmp", MNT_DETACH) == 0, "mount-detach");
    require(write(kFile, "alive", 5) == 5, "mount-detached-open");
    close(kFile);
    require(mount("none", "/tmp", "tmpfs", 0, NULL) == 0, "mount-reuse-seat");
    require(access("/tmp/lifetime", F_OK) == -1 && errno == ENOENT, "mount-fresh-volume");
    require(umount("/tmp") == 0, "mount-idle-unmount");
    require(mount("none", "/tmp", "tmpfs", 0, NULL) == 0, "mount-remount");
}

static void protection(void) {
    char* const kPages =
        mmap(NULL, 3UL * 4096UL, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    require(kPages != MAP_FAILED, "map-three");
    kPages[4096] = 'r';
    require(mprotect(kPages + 4096, 4096, PROT_READ) == 0, "protect-middle");
    kPages[0]    = 'a';
    kPages[8192] = 'b';
    require(kPages[4096] == 'r', "protect-preserve");
    require(munmap(kPages, 3UL * 4096UL) == 0, "unmap-three");
}

int main(int argc, char** argv) {
    shared_memory();
    local_pair();
    local_names();
    protection();
    if (argc > 1 && strcmp(argv[1], "--guest") == 0) {
        mount_lifetime();
    }
    puts("ECOSYSTEM_PASS shm unix poll relro");
    return 0;
}
