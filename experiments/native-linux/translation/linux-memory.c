/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "linux-memory.h"
#include <errno.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static pthread_t worker;
static unsigned interval;
static bool stopping;
static const char *phase_name;
static uint64_t samples, errors, started, collection_ns, resident_lifetime_peak;
static struct Metric {
    uint64_t sum, peak, latest;
} footprint, resident;

static uint64_t now(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t))
        abort();
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

static void add(struct Metric *metric, uint64_t value)
{
    metric->sum += value;
    metric->latest = value;
    if (value > metric->peak)
        metric->peak = value;
}

static void sample(void)
{
    uint64_t begin = now();
    task_vm_info_data_t info = {0};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) == KERN_SUCCESS) {
        ++samples;
        add(&footprint, info.phys_footprint);
        add(&resident, info.resident_size);
        resident_lifetime_peak = info.resident_size_peak;
        printf("{\"memory_sample\":\"%s\",\"monotonic_ns\":%llu,\"footprint_bytes\":%llu,\"resident_bytes\":%llu,\"resident_lifetime_peak_bytes\":%llu}\n",
               phase_name, now(), info.phys_footprint, info.resident_size, info.resident_size_peak);
    } else
        ++errors;
    collection_ns += now() - begin;
}

static void *monitor(void *unused)
{
    (void)unused;
    pthread_mutex_lock(&lock);
    while (!stopping) {
        struct timespec delay = {.tv_sec = interval / 1000, .tv_nsec = (interval % 1000) * 1000000};
        int error = pthread_cond_timedwait_relative_np(&condition, &lock, &delay);
        if (error && error != ETIMEDOUT)
            abort();
        if (!stopping)
            sample();
    }
    pthread_mutex_unlock(&lock);
    return NULL;
}

void translation_memory_phase(const char *phase)
{
    if (!interval)
        return;
    pthread_mutex_lock(&lock);
    if (phase_name) {
        sample();
        printf("{\"memory_statistics\":\"%s\",\"interval_ms\":%u,\"samples\":%llu,\"errors\":%llu,\"duration_ns\":%llu,\"collection_ns\":%llu,\"footprint_mean_bytes\":%.3f,\"footprint_sample_peak_bytes\":%llu,\"footprint_latest_bytes\":%llu,\"resident_mean_bytes\":%.3f,\"resident_sample_peak_bytes\":%llu,\"resident_latest_bytes\":%llu,\"resident_lifetime_peak_bytes\":%llu}\n",
               phase_name, interval, samples, errors, now() - started, collection_ns,
               samples ? (double)footprint.sum / samples : 0, footprint.peak, footprint.latest,
               samples ? (double)resident.sum / samples : 0, resident.peak, resident.latest, resident_lifetime_peak);
    }
    phase_name = phase;
    samples = errors = collection_ns = 0;
    footprint = resident = (struct Metric){0};
    if (phase) {
        started = now();
        sample();
    }
    pthread_mutex_unlock(&lock);
}

void translation_memory_start(unsigned interval_ms)
{
    interval = interval_ms;
    if (!interval)
        return;
    translation_memory_phase("kernel_setup");
    if (pthread_create(&worker, NULL, monitor, NULL))
        abort();
}

void translation_memory_stop(void)
{
    if (!interval)
        return;
    pthread_mutex_lock(&lock);
    stopping = true;
    pthread_cond_signal(&condition);
    pthread_mutex_unlock(&lock);
    if (pthread_join(worker, NULL))
        abort();
    translation_memory_phase(NULL);
}
