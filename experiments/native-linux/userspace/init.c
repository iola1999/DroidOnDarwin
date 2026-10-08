/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { dprintf(2, "init:%d errno=%d\n", __LINE__, errno); return 90; } } while (0)

static volatile unsigned initialized = 0x19761234;
static volatile unsigned char bss[65536];
static unsigned transform(unsigned x) { return (x * 17) ^ 0x73524102; }
static unsigned (*volatile indirect)(unsigned) = transform;

int main(int argc, char **argv)
{
    CHECK(argc == 1 && !strcmp(argv[0], "/init-user"));
    CHECK(getpid() == 1);
    char *rejected[] = {"/unregistered-elf", NULL};
    CHECK(execv(rejected[0], rejected) == -1 && errno == ENOEXEC);
    CHECK(initialized == 0x19761234 && bss[65535] == 0);
    for (unsigned i = 0; i < sizeof(bss); i += 16384) {
        CHECK(bss[i] == 0);
        bss[i] = (unsigned char)(i / 16384 + 1);
    }
    CHECK(indirect(7) == transform(7));
    CHECK(sysconf(_SC_PAGESIZE) == 16384);
    struct utsname uts;
    CHECK(uname(&uts) == 0 && !strcmp(uts.sysname, "Linux"));
    CHECK(getenv("NCE_TEST") && !strcmp(getenv("NCE_TEST"), "userspace"));
    FILE *script = fopen("/boot.script", "r");
    CHECK(script);
    char *line = NULL;
    size_t capacity = 0;
    unsigned completed = 0;
    while (getline(&line, &capacity, script) > 0) {
        char *args[8];
        int n = 0;
        for (char *word = strtok(line, " \t\r\n"); word && n < 7; word = strtok(NULL, " \t\r\n")) args[n++] = word;
        args[n] = NULL;
        if (!n) continue;
        int fds[2];
        CHECK(pipe(fds) == 0);
        pid_t child = fork();
        CHECK(child >= 0);
        if (!child) {
            close(fds[0]);
            if (dup2(fds[1], 1) < 0) _exit(91);
            close(fds[1]);
            execv(args[0], args);
            _exit(92);
        }
        close(fds[1]);
        char output[256];
        ssize_t length = read(fds[0], output, sizeof(output) - 1);
        CHECK(length > 0);
        output[length] = 0;
        close(fds[0]);
        int status;
        CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
        CHECK(strstr(output, "worker passed"));
        ++completed;
    }
    free(line);
    CHECK(fclose(script) == 0 && completed == 8);
    FILE *result = fopen("/userspace.result", "w");
    CHECK(result);
    CHECK(fprintf(result, "init pid=%ld commands=%u libc=musl passed\n", (long)getpid(), completed) > 0);
    CHECK(fclose(result) == 0);
    return 0;
}
