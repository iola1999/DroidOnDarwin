/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <poll.h>
#include <signal.h>
#include <time.h>

struct ComponentResult {
    int exec_error, exit_code, signal, usage, timed_out;
};

static uint64_t component_seconds(void)
{
    struct timespec now;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return now.tv_sec;
}

/* Both fixtures create threads but no child processes. The deadline kills and
 * reaps this direct child; pipe EOF and process exit are tracked separately. */
static struct ComponentResult component_run(char *const arguments[], char *const environment[], unsigned seconds)
{
    int error_pipe[2], output_pipe[2];
    CHECK(!pipe2(error_pipe, O_CLOEXEC) && !pipe2(output_pipe, O_CLOEXEC));
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (!pid) {
        close(error_pipe[0]);
        close(output_pipe[0]);
        CHECK(dup2(output_pipe[1], 1) == 1);
        close(output_pipe[1]);
        execve(arguments[0], arguments, environment);
        int error = errno;
        CHECK(write(error_pipe[1], &error, sizeof(error)) == sizeof(error));
        _exit(127);
    }
    CHECK(!close(error_pipe[1]) && !close(output_pipe[1]));
    struct ComponentResult result = {.exit_code = -1};
    uint64_t deadline = component_seconds() + seconds;
    int status = 0, finished = 0, eof = 0;
    char tail[6] = {0};
    size_t tail_size = 0;
    while (!finished || !eof) {
        if (!finished) {
            pid_t waited = waitpid(pid, &status, WNOHANG);
            CHECK(waited == 0 || waited == pid || (waited == -1 && errno == EINTR));
            finished = waited == pid;
            if (!finished && !result.timed_out && component_seconds() >= deadline) {
                dprintf(2, "component timeout: pid=%ld seconds=%u\n", (long)pid, seconds);
                CHECK(kill(pid, SIGKILL) == 0 || errno == ESRCH);
                result.timed_out = 1;
            }
        }
        struct pollfd descriptor = {.fd = eof ? -1 : output_pipe[0], .events = POLLIN};
        int ready = poll(&descriptor, 1, 100);
        CHECK(ready >= 0 || errno == EINTR);
        if (ready <= 0 || eof)
            continue;
        CHECK(!(descriptor.revents & (POLLERR | POLLNVAL)));
        char buffer[4103];
        memcpy(buffer, tail, tail_size);
        ssize_t count = read(output_pipe[0], buffer + tail_size, 4096);
        if (count < 0 && errno == EINTR)
            continue;
        CHECK(count >= 0);
        eof = count == 0;
        size_t sent = 0;
        while (sent < (size_t)count) {
            ssize_t written = write(1, buffer + tail_size + sent, count - sent);
            if (written < 0 && errno == EINTR)
                continue;
            CHECK(written > 0);
            sent += written;
        }
        size_t total = tail_size + count;
        buffer[total] = 0;
        result.usage |= strstr(buffer, "Usage:") != NULL;
        tail_size = total < sizeof(tail) ? total : sizeof(tail);
        memcpy(tail, buffer + total - tail_size, tail_size);
    }
    CHECK(close(output_pipe[0]) == 0);
    ssize_t bytes = read(error_pipe[0], &result.exec_error, sizeof(result.exec_error));
    CHECK(bytes == 0 || bytes == sizeof(result.exec_error));
    CHECK(close(error_pipe[0]) == 0);
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    result.signal = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
    return result;
}
