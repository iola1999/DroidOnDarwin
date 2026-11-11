/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(c) \
    do { \
        if (!(c)) { \
            dprintf(2, "stock supervisor:%d errno=%d\n", __LINE__, errno); \
            _exit(90); \
        } \
    } while (0)

static int stage(const char *name, const char *program, int help)
{
    int error_pipe[2];
    CHECK(!pipe2(error_pipe, O_CLOEXEC));
    printf("{\"component_start\":\"%s\"}\n", name);
    int output = open("/stock-output", O_CREAT | O_TRUNC | O_RDWR, 0600);
    CHECK(output >= 0);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (!pid) {
        close(error_pipe[0]);
        CHECK(dup2(output, 1) == 1);
        close(output);
        char *arguments[] = {(char *)program, NULL};
        char *environment[] = {"LD_LIBRARY_PATH=/apex/com.android.runtime/lib64/bionic", NULL};
        execve(program, arguments, environment);
        int error = errno;
        CHECK(write(error_pipe[1], &error, sizeof(error)) == sizeof(error));
        _exit(127);
    }
    CHECK(!close(error_pipe[1]));
    int status;
    CHECK(waitpid(pid, &status, 0) == pid);
    int exec_error = 0;
    ssize_t bytes = read(error_pipe[0], &exec_error, sizeof(exec_error));
    CHECK(bytes == 0 || bytes == sizeof(exec_error));
    CHECK(!close(error_pipe[0]) && lseek(output, 0, SEEK_SET) == 0);
    char text[4097];
    int usage = 0;
    while ((bytes = read(output, text, sizeof(text) - 1)) > 0) {
        text[bytes] = 0;
        usage |= strstr(text, "Usage:") != NULL;
        CHECK(write(1, text, bytes) == bytes);
    }
    CHECK(!bytes && !close(output));
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    int signal = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
    int success = !exec_error && exit_code == 0 && !signal && (!help || usage);
    printf("{\"component_stage\":\"%s\",\"started\":true,\"success\":%s,\"exec_errno\":%d,\"exit_code\":%d,\"signal\":%d,\"usage\":%s}\n",
           name, success ? "true" : "false", exec_error, exit_code, signal, usage ? "true" : "false");
    return success;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("{\"component_supervisor\":true,\"page_size\":%ld}\n", sysconf(_SC_PAGESIZE));
    int help = stage("linker_help", "/apex/com.android.runtime/bin/linker64", 1);
    if (help)
        stage("dynamic", "/system/bin/stock-bionic", 0);
    else
        puts("{\"component_stage\":\"dynamic\",\"started\":false,\"reason\":\"linker_failed\"}");
    puts("{\"test\":\"stock_bionic_observation_complete\",\"passed\":true}");
    return 0;
}
