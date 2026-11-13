/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <limits.h>

struct StockInitPeerStop {
    long pid, tid, result;
};

static void *stock_init_peer_stop(void *opaque)
{
    struct StockInitPeerStop *request = opaque;
    long args[6] = {0};
    request->tid = call(178, args);
    REQUIRE(request->tid > 0);
    args[0] = request->pid;
    request->result = call(247, args);
    return NULL;
}

static void stock_init_owned_ordinary(void)
{
    put_file("/probe-stop-ordinary", "guest.elf");
    long launch[6] = {(long)translation_linux_process("/probe-stop-ordinary"), 0};
    long pid = call(246, launch);
    REQUIRE(pid > 1);
    double deadline = stock_init_now() + 2;
    long ready = 0;
    while (!ready) {
        long args[6] = {-100, (long)"/stock-init-ordinary-ready", 0};
        long fd = call(56, args);
        if (fd >= 0) {
            char text[32] = {0}, extra;
            args[0] = fd;
            args[1] = (long)text;
            args[2] = sizeof(text) - 1;
            long bytes = call(63, args);
            REQUIRE(bytes >= 0 && bytes < (long)sizeof(text) && call(57, args) == 0);
            if (bytes)
                REQUIRE(sscanf(text, "%ld %c", &ready, &extra) == 1 && ready == pid);
        } else
            REQUIRE(fd == -2);
        REQUIRE(stock_init_now() < deadline);
        if (!ready) {
            struct timespec delay = {0, 10000000};
            REQUIRE(nanosleep(&delay, NULL) == 0);
        }
    }
    long args[6] = {pid};
    long denied = call(247, args);
    long alive = call(129, args);
    args[1] = 9;
    long killed = call(129, args);
    int status = -1;
    long wait[6] = {pid, (long)&status, 0};
    long waited = call(260, wait);
    printf("{\"stock_init_owned_ordinary\":true,\"pid\":%ld,\"ready_pid\":%ld,\"host_stop_result\":%ld,\"alive_result\":%ld,\"kill_result\":%ld,\"waited\":%ld,\"status\":%d}\n", pid, ready, denied, alive, killed, waited, status);
    REQUIRE(denied == -1 && alive == 0 && killed == 0 && waited == pid && status == 9);
    const char *files[] = {"/stock-init-ordinary-ready", "/probe-stop-ordinary"};
    for (unsigned i = 0; i < sizeof(files) / sizeof(*files); ++i) {
        long unlink[6] = {-100, (long)files[i], 0};
        REQUIRE(call(35, unlink) == 0);
    }
}

static void stock_init_stop_boundaries(long pid)
{
    const long invalid[] = {0, -1, (long)INT_MAX + 1};
    long invalid_results[3];
    long args[6] = {0};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        args[0] = invalid[i];
        invalid_results[i] = call(247, args);
    }
    args[0] = pid;
    args[1] = 1;
    long extra_result = call(247, args);
    args[0] = INT_MAX;
    args[1] = 0;
    long missing = call(247, args);
    long owner = call(178, args);
    REQUIRE(owner > 0);
    args[0] = owner;
    long host = call(247, args);
    struct StockInitPeerStop peer = {.pid = pid};
    pthread_t thread;
    REQUIRE(pthread_create(&thread, NULL, stock_init_peer_stop, &peer) == 0 && pthread_join(thread, NULL) == 0);
    printf("{\"stock_init_host_stop_boundaries\":true,\"ancestor_pid\":%ld,\"invalid_pid_results\":[%ld,%ld,%ld],\"extra_argument_result\":%ld,\"missing_pid_result\":%ld,\"owner_tid\":%ld,\"host_task_result\":%ld,\"peer_tid\":%ld,\"peer_result\":%ld}\n", pid, invalid_results[0], invalid_results[1], invalid_results[2], extra_result, missing, owner, host, peer.tid, peer.result);
    REQUIRE(invalid_results[0] == -22 && invalid_results[1] == -22 && invalid_results[2] == -22 && extra_result == -22 && missing == -3);
    REQUIRE(host == -1 && peer.result == -1 && peer.tid > 0 && peer.tid != owner);
    stock_init_owned_ordinary();
}

static long stock_init_stop(long pid, int *status, long *killed, int *stopped)
{
    long args[6] = {pid, 9};
    *killed = call(129, args);
    printf("{\"stock_init_stop_request\":true,\"ancestor_pid\":%ld,\"signal\":9,\"result\":%ld}\n", pid, *killed);
    long effective = *killed;
    if (*killed == -13 || *killed == -1) {
        args[1] = 0;
        effective = call(247, args);
        printf("{\"stock_init_host_stop\":true,\"ancestor_pid\":%ld,\"signal\":9,\"result\":%ld}\n", pid, effective);
    }
    if (effective && effective != -3) {
        stock_init_dump_output(1, stdout);
        stock_init_dump_output(2, stderr);
        stock_init_dump_kernel_log();
    }
    REQUIRE(effective == 0 || effective == -3);
    *stopped = effective == 0;
    long wait[6] = {pid, (long)status, effective == -3 ? 1 : 0};
    long waited = call(260, wait);
    if (waited == 0) {
        wait[2] = 0;
        waited = call(260, wait);
    }
    REQUIRE(waited == pid);
    return waited;
}
