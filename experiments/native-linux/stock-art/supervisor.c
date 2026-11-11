/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(c) \
    do { \
        if (!(c)) { \
            dprintf(2, "stock ART supervisor:%d errno=%d\n", __LINE__, errno); \
            _exit(90); \
        } \
    } while (0)
#include "../translation/component-supervisor.h"

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    char classpath[4096];
    FILE *input = fopen("/data/stock-art/boot-classpath", "r");
    CHECK(input && fgets(classpath, sizeof(classpath), input));
    CHECK(strchr(classpath, '\n') && fgetc(input) == EOF && !ferror(input) && fclose(input) == 0);
    *strchr(classpath, '\n') = 0;
    char *arguments[] = {"/system/bin/stock-art-probe", classpath, NULL};
    char *environment[] = {"LD_LIBRARY_PATH=/apex/com.android.art/lib64:/apex/com.android.runtime/lib64/bionic:/apex/com.android.i18n/lib64:/apex/com.android.os.statsd/lib64:/system/lib64", NULL};
    printf("{\"stock_art_supervisor\":true,\"page_size\":%ld,\"timeout_seconds\":90}\n", sysconf(_SC_PAGESIZE));
    struct ComponentResult result = component_run(arguments, environment, 90);
    int success = !result.exec_error && result.exit_code == 0 && !result.signal && !result.timed_out;
    printf("{\"stock_art_exit\":true,\"success\":%s,\"exec_errno\":%d,\"exit_code\":%d,\"signal\":%d,\"timed_out\":%s}\n",
           success ? "true" : "false", result.exec_error, result.exit_code, result.signal, result.timed_out ? "true" : "false");
    return 0;
}
