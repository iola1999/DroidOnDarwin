/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static uint64_t now(void)
{
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time))
        _exit(118);
    return (uint64_t)time.tv_sec * 1000000000 + time.tv_nsec;
}

int main(void)
{
    int fd = open("/data/nce/host-scheduling", O_RDWR);
    if (fd < 0)
        return 1;
    _Atomic uint32_t *state = mmap(NULL, 16384, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (state == MAP_FAILED || close(fd))
        return 2;
    uint64_t deadline = now() + 5000000000ULL;
    atomic_store(&state[0], 1);
    unsigned iterations = 0;
    while (atomic_load(&state[1]) != 32) {
        atomic_store(&state[2], atomic_load(&state[1]));
        // Remain runnable: host requests must complete without guest sleep.
        if (!(++iterations % 1048576) && now() > deadline)
            return 3;
    }
    atomic_store(&state[2], 32);
    if (munmap(state, 16384))
        return 4;
    puts("{\"test\":\"linux_busy_guest_host_requests\",\"passed\":true,\"rounds\":32}");
    return 0;
}
