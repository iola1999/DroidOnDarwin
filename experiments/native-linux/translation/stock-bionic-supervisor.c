/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
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

#include "component-supervisor.h"

static int stage(const char *name, const char *program, int help)
{
    printf("{\"component_start\":\"%s\"}\n", name);
    char *arguments[] = {(char *)program, NULL};
    char *environment[] = {"LD_LIBRARY_PATH=/apex/com.android.runtime/lib64/bionic", NULL};
    struct ComponentResult result = component_run(arguments, environment, 90);
    int success = !result.exec_error && result.exit_code == 0 && !result.signal && !result.timed_out && (!help || result.usage);
    printf("{\"component_stage\":\"%s\",\"started\":true,\"success\":%s,\"exec_errno\":%d,\"exit_code\":%d,\"signal\":%d,\"usage\":%s}\n",
           name, success ? "true" : "false", result.exec_error, result.exit_code, result.signal, result.usage ? "true" : "false");
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
