/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { STOCK_INIT_FIRST_STAGE_SYSFS_BYTES = 4096 };

struct stock_init_first_stage_sysfs_attempts {
    long *values;
    size_t count, capacity;
};

struct stock_init_first_stage_sysfs_read {
    const char *path;
    int reading;
    char value[STOCK_INIT_FIRST_STAGE_SYSFS_BYTES + 1];
    size_t bytes;
    struct stock_init_first_stage_sysfs_attempts open, read, readlink;
    long fd, close_result, result;
};

static long stock_init_first_stage_sysfs_call(struct stock_init_first_stage_sysfs_attempts *attempts, long number, long args[6])
{
    if (attempts->count == attempts->capacity) {
        if (attempts->capacity > SIZE_MAX / 2 / sizeof(long))
            return -12;
        size_t capacity = attempts->capacity ? attempts->capacity * 2 : 8;
        long *values = realloc(attempts->values, capacity * sizeof(*values));
        if (!values)
            return -12;
        attempts->values = values;
        attempts->capacity = capacity;
    }
    long result = call(number, args);
    attempts->values[attempts->count++] = result;
    return result;
}

static void stock_init_first_stage_sysfs_hex(const void *value, size_t bytes)
{
    const unsigned char *data = value;
    for (size_t i = 0; i < bytes; ++i)
        printf("%02x", data[i]);
}

static void stock_init_first_stage_sysfs_attempts(const struct stock_init_first_stage_sysfs_attempts *attempts)
{
    if (!attempts->count) {
        fputs("null", stdout);
        return;
    }
    putchar('[');
    for (size_t i = 0; i < attempts->count; ++i)
        printf("%s%ld", i ? "," : "", attempts->values[i]);
    putchar(']');
}

static void stock_init_first_stage_sysfs_report(const struct stock_init_first_stage_sysfs_read *record)
{
    flockfile(stdout);
    printf("{\"stock_init_first_stage_sysfs\":true,\"operation\":\"%s\",\"path_hex\":\"", record->reading ? "read" : "readlink");
    stock_init_first_stage_sysfs_hex(record->path, strlen(record->path));
    fputs("\",\"value_hex\":\"", stdout);
    stock_init_first_stage_sysfs_hex(record->value, record->bytes);
    fputs("\",\"open_results\":", stdout);
    stock_init_first_stage_sysfs_attempts(&record->open);
    fputs(",\"read_results\":", stdout);
    stock_init_first_stage_sysfs_attempts(&record->read);
    fputs(",\"close_result\":", stdout);
    if (record->fd >= 0)
        printf("%ld", record->close_result);
    else
        fputs("null", stdout);
    fputs(",\"readlink_results\":", stdout);
    stock_init_first_stage_sysfs_attempts(&record->readlink);
    printf(",\"result\":%ld}\n", record->result);
    funlockfile(stdout);
}

static long stock_init_first_stage_sysfs_query(const char *path, int reading)
{
    struct stock_init_first_stage_sysfs_read record = {.path = path, .reading = reading, .fd = -1};
    long result;
    if (reading) {
        long open[6] = {-100, (long)path, 0x80000}; /* Linux arm64 O_RDONLY | O_CLOEXEC. */
        do {
            result = stock_init_first_stage_sysfs_call(&record.open, 56, open);
        } while (result == -4);
        if (result < 0) {
            record.result = result;
        } else {
            record.fd = result;
            for (;;) {
                size_t requested = record.bytes < STOCK_INIT_FIRST_STAGE_SYSFS_BYTES ? STOCK_INIT_FIRST_STAGE_SYSFS_BYTES - record.bytes : 1;
                long read[6] = {record.fd, (long)(record.value + record.bytes), (long)requested};
                do {
                    result = stock_init_first_stage_sysfs_call(&record.read, 63, read);
                } while (result == -4);
                if (result < 0) {
                    record.result = result;
                    break;
                }
                if ((unsigned long)result > requested) {
                    record.result = -5;
                    break;
                }
                if (!result)
                    break;
                record.bytes += result;
                if (record.bytes > STOCK_INIT_FIRST_STAGE_SYSFS_BYTES) {
                    record.result = -7;
                    break;
                }
            }
            long close[6] = {record.fd};
            /* Linux releases the fd before a close error; retrying can close a reused fd. */
            record.close_result = call(57, close);
            if (!record.result && record.close_result)
                record.result = record.close_result < 0 ? record.close_result : -5;
        }
    } else {
        long readlink[6] = {-100, (long)path, (long)record.value, sizeof(record.value)};
        do {
            result = stock_init_first_stage_sysfs_call(&record.readlink, 78, readlink);
        } while (result == -4);
        if (result < 0) {
            record.result = result;
        } else if ((unsigned long)result > sizeof(record.value)) {
            record.result = -5;
        } else {
            record.bytes = result;
            if (record.bytes > STOCK_INIT_FIRST_STAGE_SYSFS_BYTES)
                record.result = -7;
        }
    }
    stock_init_first_stage_sysfs_report(&record);
    free(record.open.values);
    free(record.read.values);
    free(record.readlink.values);
    return record.result;
}

static void stock_init_first_stage_sysfs_observe(long *first_error, const char *path, int reading, int absent)
{
    long result = stock_init_first_stage_sysfs_query(path, reading);
    if (result && !(absent && result == -2) && !*first_error)
        *first_error = result;
}

/* /sys must be mounted. The caller owns its mount and all disk lifetimes. */
static long stock_init_first_stage_devices(int ranchu, int data, int metadata)
{
    long result = 0;
    if ((ranchu != 0 && ranchu != 1) || (data != 0 && data != 1) || (metadata != 0 && metadata != 1)) {
        result = -22;
    } else {
        stock_init_first_stage_sysfs_observe(&result, "/sys/devices/platform/a003c00.virtio_mmio/subsystem", 0, !ranchu);
        stock_init_first_stage_sysfs_observe(&result, ranchu ? "/sys/class/block/ncedisk0" : "/sys/class/block/vdc", 0, 1);
        const char *disks[] = {ranchu ? "vdc" : "ncedisk0", "ncedisk1", "ncedisk2"};
        const int present[] = {data, 1, metadata};
        const char *files[] = {"dev", "uevent", "ro", "size"};
        char path[160];
        for (unsigned i = 0; i < 3; ++i) {
            snprintf(path, sizeof(path), "/sys/class/block/%s", disks[i]);
            stock_init_first_stage_sysfs_observe(&result, path, 0, !present[i]);
            if (!present[i])
                continue;
            for (unsigned j = 0; j < 4; ++j) {
                snprintf(path, sizeof(path), "/sys/class/block/%s/%s", disks[i], files[j]);
                stock_init_first_stage_sysfs_observe(&result, path, 1, 0);
            }
            snprintf(path, sizeof(path), "/sys/class/block/%s/device", disks[i]);
            stock_init_first_stage_sysfs_observe(&result, path, 0, !ranchu);
        }
        const char *partitions[] = {"ncedisk1p1", "ncedisk1p2", "ncedisk2p1"};
        const char *part_files[] = {"dev", "uevent", "ro", "partition", "start", "size"};
        for (unsigned i = 0; i < 3; ++i) {
            int exists = i < 2 || metadata;
            snprintf(path, sizeof(path), "/sys/class/block/%s", partitions[i]);
            stock_init_first_stage_sysfs_observe(&result, path, 0, !exists);
            if (!exists)
                continue;
            for (unsigned j = 0; j < 6; ++j) {
                snprintf(path, sizeof(path), "/sys/class/block/%s/%s", partitions[i], part_files[j]);
                stock_init_first_stage_sysfs_observe(&result, path, 1, 0);
            }
            snprintf(path, sizeof(path), "/sys/class/block/%s/subsystem", partitions[i]);
            stock_init_first_stage_sysfs_observe(&result, path, 0, 0);
        }
    }
    flockfile(stdout);
    printf("{\"stock_init_first_stage_devices\":true,\"ranchu\":%d,\"data\":%d,\"metadata\":%d,\"result\":%ld}\n", ranchu, data, metadata, result);
    funlockfile(stdout);
    return result;
}
