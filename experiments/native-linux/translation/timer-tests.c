/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#define CHECK(c) \
    do { \
        if (!(c)) { \
            dprintf(2, "translation-timer:%d errno=%d\n", __LINE__, errno); \
            _exit(95); \
        } \
    } while (0)

static int64_t now(void)
{
    struct timespec time;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &time) == 0);
    return (int64_t)time.tv_sec * 1000000000 + time.tv_nsec;
}

void translation_timer_tests(int high_resolution)
{
    struct timespec resolution;
    CHECK(clock_getres(CLOCK_MONOTONIC, &resolution) == 0);
    int64_t resolution_ns = (int64_t)resolution.tv_sec * 1000000000 + resolution.tv_nsec;
    printf("{\"timer_resolution_ns\":%" PRId64 ",\"high_res_requested\":%s}\n", resolution_ns, high_resolution ? "true" : "false");
    CHECK(resolution_ns > 0 && (!high_resolution || resolution_ns == 1));
    // Record actual waits; scheduler load can extend them. Returning before
    // the requested deadline is a correctness failure in either variant.
    const long waits[] = {1000000, 4000000, 16666666};
    for (unsigned kind = 0; kind < sizeof(waits) / sizeof(waits[0]); ++kind) {
        printf("{\"timer_wait\":\"nanosleep\",\"requested_ns\":%ld,\"resolution_ns\":%" PRId64 ",\"elapsed_ns\":[", waits[kind], resolution_ns);
        for (unsigned sample = 0; sample < 8; ++sample) {
            struct timespec request = {.tv_nsec = waits[kind]};
            int64_t begin = now();
            CHECK(nanosleep(&request, NULL) == 0);
            int64_t elapsed = now() - begin;
            CHECK(elapsed >= waits[kind]);
            printf("%s%" PRId64, sample ? "," : "", elapsed);
        }
        puts("]}");
    }
    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    CHECK(fd >= 0);
    uint64_t expirations;
    CHECK(read(fd, &expirations, sizeof(expirations)) == -1 && errno == EAGAIN);
    int64_t deadline = now() + 4000000;
    struct itimerspec timer = {.it_value = {.tv_sec = deadline / 1000000000, .tv_nsec = deadline % 1000000000}};
    CHECK(timerfd_settime(fd, TFD_TIMER_ABSTIME, &timer, NULL) == 0);
    struct pollfd event = {.fd = fd, .events = POLLIN};
    CHECK(poll(&event, 1, 1000) == 1 && (event.revents & POLLIN));
    CHECK(read(fd, &expirations, sizeof(expirations)) == sizeof(expirations) && expirations == 1);
    CHECK(now() >= deadline);
    CHECK(read(fd, &expirations, sizeof(expirations)) == -1 && errno == EAGAIN);

    timer = (struct itimerspec){.it_value = {.tv_nsec = 1000000}, .it_interval = {.tv_nsec = 1000000}};
    CHECK(timerfd_settime(fd, 0, &timer, NULL) == 0);
    // hrtimer_forward clamps periodic intervals to hrtimer_resolution.
    // Allow multiple effective periods in the low-resolution control too.
    struct timespec accumulate = {.tv_nsec = 8 * (resolution_ns > 1000000 ? resolution_ns : 1000000)};
    CHECK(nanosleep(&accumulate, NULL) == 0);
    CHECK(poll(&event, 1, 1000) == 1 && (event.revents & POLLIN));
    CHECK(read(fd, &expirations, sizeof(expirations)) == sizeof(expirations));
    printf("{\"timerfd_accumulated_expirations\":%" PRIu64 "}\n", expirations);
    CHECK(expirations >= 5);
    timer = (struct itimerspec){0};
    CHECK(timerfd_settime(fd, 0, &timer, NULL) == 0);
    CHECK(timerfd_gettime(fd, &timer) == 0);
    CHECK(!timer.it_value.tv_sec && !timer.it_value.tv_nsec && !timer.it_interval.tv_sec && !timer.it_interval.tv_nsec);
    CHECK(nanosleep(&accumulate, NULL) == 0);
    CHECK(read(fd, &expirations, sizeof(expirations)) == -1 && errno == EAGAIN);
    CHECK(close(fd) == 0);
    printf("{\"test\":\"translated_timer_boundaries\",\"passed\":true,\"high_res_timers\":%s,\"resolution_ns\":%" PRId64 "}\n",
           high_resolution ? "true" : "false", resolution_ns);
}
