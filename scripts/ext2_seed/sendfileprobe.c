#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <asm-generic/errno-base.h>
#include <sys/sendfile.h>

struct Probe {
    int            input;
    int            output;
    unsigned char* bytes;
    size_t         count;
};

static int write_source(struct Probe* probe) {
    size_t done = 0;
    while (done < probe->count) {
        const ssize_t kPut = write(probe->input, probe->bytes + done, probe->count - done);
        if (kPut <= 0) {
            return 0;
        }
        done += (size_t)kPut;
    }
    return lseek(probe->input, 13, SEEK_SET) == 13;
}

static int check_bytes(int descriptor, const unsigned char* bytes, size_t count) {
    unsigned char buffer[1024];
    size_t        done = 0;
    while (done < count) {
        const size_t  kWant = count - done < sizeof(buffer) ? count - done : sizeof(buffer);
        const ssize_t kRead = pread(descriptor, buffer, kWant, (off_t)done);
        if (kRead != (ssize_t)kWant || memcmp(buffer, bytes + done, kWant) != 0) {
            return 0;
        }
        done += kWant;
    }
    return 1;
}

static int check_explicit(struct Probe* probe) {
    off_t offset = 5;
    if (sendfile(probe->output, probe->input, &offset, 137) != 137 || offset != 142 ||
        lseek(probe->input, 0, SEEK_CUR) != 13 || lseek(probe->output, 0, SEEK_CUR) != 137 ||
        !check_bytes(probe->output, probe->bytes + 5, 137)) {
        return 0;
    }
    offset = -1;
    errno  = 0;
    if (sendfile(probe->output, probe->input, &offset, 1) != -1 || errno != EINVAL) {
        return 0;
    }
    errno = 0;
    return sendfile(probe->output, probe->input, (off_t*)1, 1) == -1 && errno == EFAULT;
}

static int check_shared(struct Probe* probe) {
    if (lseek(probe->output, 0, SEEK_SET) != 0) {
        return 0;
    }
    const int kAlias = dup(probe->input);
    if (kAlias < 0) {
        return 0;
    }
    const ssize_t kSent = sendfile(probe->output, kAlias, NULL, probe->count);
    close(kAlias);
    if (kSent != (ssize_t)(probe->count - 13) ||
        lseek(probe->input, 0, SEEK_CUR) != (off_t)probe->count ||
        !check_bytes(probe->output, probe->bytes + 13, probe->count - 13) ||
        sendfile(probe->output, probe->input, NULL, 1) != 0) {
        return 0;
    }
    if (fcntl(probe->output, F_SETFL, O_APPEND) < 0) {
        return 0;
    }
    errno               = 0;
    const int kRejected = sendfile(probe->output, probe->input, NULL, 1) == -1 && errno == EINVAL;
    fcntl(probe->output, F_SETFL, 0);
    return kRejected;
}

static int check_pipe(struct Probe* probe, size_t page) {
    int fds[2] = {-1, -1};
    int result = 0;
    if (pipe(fds) < 0 || fcntl(fds[1], F_SETFL, O_NONBLOCK) < 0 ||
        lseek(probe->input, 0, SEEK_SET) != 0) {
        goto cleanup;
    }
    off_t position = 0;
    if (sendfile(probe->output, fds[0], &position, 1) != -1 || errno != ESPIPE) {
        goto cleanup;
    }
    const ssize_t kFirst = sendfile(fds[1], probe->input, NULL, page + 1);
    if (kFirst <= 0) {
        goto cleanup;
    }
    errno = 0;
    if (sendfile(fds[1], probe->input, NULL, probe->count) != -1 || errno != EAGAIN ||
        lseek(probe->input, 0, SEEK_CUR) != kFirst) {
        goto cleanup;
    }
    unsigned char buffer[1024];
    ssize_t       drained = 0;
    while (drained < kFirst) {
        const ssize_t kRead = read(fds[0], buffer, sizeof(buffer));
        if (kRead <= 0 || memcmp(buffer, probe->bytes + (size_t)drained, (size_t)kRead) != 0) {
            goto cleanup;
        }
        drained += kRead;
    }
    if (sendfile(fds[1], probe->input, NULL, 7) != 7) {
        goto cleanup;
    }
    close(fds[0]);
    fds[0] = -1;
    signal(SIGPIPE, SIG_IGN);
    errno  = 0;
    result = sendfile(fds[1], probe->input, NULL, 1) == -1 && errno == EPIPE;
cleanup:
    if (fds[0] >= 0) {
        close(fds[0]);
    }
    if (fds[1] >= 0) {
        close(fds[1]);
    }
    return result;
}

int main(void) {
    const long kPage = sysconf(_SC_PAGESIZE);
    if (kPage <= 0) {
        return 1;
    }
    struct Probe probe = {.input = -1, .output = -1, .count = ((size_t)kPage * 3) + 37};
    probe.bytes        = malloc(probe.count);
    int result         = 1;
    if (probe.bytes == NULL) {
        return 1;
    }
    for (size_t index = 0; index < probe.count; ++index) {
        probe.bytes[index] = (unsigned char)(index % 251);
    }
    probe.input  = open("/tmp/send.in", O_RDWR | O_CREAT | O_TRUNC, 0600);
    probe.output = open("/tmp/send.out", O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (probe.input >= 0 && probe.output >= 0 && write_source(&probe) && check_explicit(&probe) &&
        check_shared(&probe) && check_pipe(&probe, (size_t)kPage)) {
        puts("SENDFILE_PROBE_PASS offsets shared eof nonblock pipe errors");
        result = 0;
    } else {
        puts("SENDFILE_PROBE_FAIL");
    }
    if (probe.input >= 0) {
        close(probe.input);
    }
    if (probe.output >= 0) {
        close(probe.output);
    }
    free(probe.bytes);
    return result;
}
