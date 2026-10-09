/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Read-only estimate of 16 KiB fastmem candidates in a 4 KiB Linux process.
 * This does not validate MMU state, physical write aliases or DMA ownership. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

enum { GUEST_PAGE = 4096,
       HOST_PAGE = 16384,
       ENTRIES = 512 };
#define PFN_MASK ((UINT64_C(1) << 55) - 1)

struct counts {
    uint64_t virtual_pages, present_pages, hidden_pfns, edge_pages;
    uint64_t full_groups, contiguous_groups, aligned_groups;
    uint64_t writable_groups, executable_groups;
    uint64_t minimum, maximum;
};

int main(int argc, char **argv)
{
    if (argc != 2 || getpagesize() != GUEST_PAGE) {
        fprintf(stderr, "usage: android-mapping-coverage PID|--allocation-sample (requires 4 KiB guest pages)\n");
        return 2;
    }
    char *end;
    errno = 0;
    bool sample = !strcmp(argv[1], "--allocation-sample");
    long pid = sample ? getpid() : strtol(argv[1], &end, 10);
    if (errno || (!sample && *end) || pid <= 0 || pid > INT32_MAX)
        return 2;
    size_t sample_bytes = sample ? 16 * 1024 * 1024 : 0;
    void *allocation = NULL;
    if (sample) {
        allocation = mmap(NULL, sample_bytes, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (allocation == MAP_FAILED) {
            perror("mmap sample");
            return 1;
        }
        for (size_t i = 0; i < sample_bytes; i += GUEST_PAGE)
            ((volatile unsigned char *)allocation)[i] = 1;
    }
    char path[96];
    snprintf(path, sizeof(path), "/proc/%ld/maps", pid);
    FILE *maps = fopen(path, "r");
    snprintf(path, sizeof(path), "/proc/%ld/pagemap", pid);
    int pagemap = open(path, O_RDONLY | O_CLOEXEC);
    if (!maps || pagemap < 0) {
        perror("open proc maps/pagemap");
        if (maps)
            fclose(maps);
        if (pagemap >= 0)
            close(pagemap);
        return 1;
    }
    struct counts c = {.minimum = UINT64_MAX};
    char *line = NULL;
    size_t capacity = 0;
    int result = 0;
    while (getline(&line, &capacity, maps) >= 0) {
        uint64_t low, high;
        char mode[5];
        if (sscanf(line, "%" SCNx64 "-%" SCNx64 " %4s", &low, &high, mode) != 3 ||
            high <= low || low % GUEST_PAGE || high % GUEST_PAGE) {
            result = 1;
            break;
        }
        if (mode[0] != 'r')
            continue;
        if (low < c.minimum)
            c.minimum = low;
        if (high > c.maximum)
            c.maximum = high;
        c.virtual_pages += (high - low) / GUEST_PAGE;
        uint64_t start = (low + HOST_PAGE - 1) & ~(uint64_t)(HOST_PAGE - 1);
        uint64_t finish = high & ~(uint64_t)(HOST_PAGE - 1);
        if (start >= finish) {
            c.edge_pages += (high - low) / GUEST_PAGE;
            continue;
        }
        c.edge_pages += (start - low + high - finish) / GUEST_PAGE;
        while (start < finish) {
            uint64_t entries[ENTRIES];
            size_t count = (finish - start) / GUEST_PAGE;
            if (count > ENTRIES)
                count = ENTRIES;
            size_t bytes = count * sizeof(uint64_t);
            ssize_t got;
            do {
                got = pread(pagemap, entries, bytes, (off_t)(start / GUEST_PAGE * 8));
            } while (got < 0 && errno == EINTR);
            if (got != (ssize_t)bytes) {
                fprintf(stderr, "pagemap short read for pid %ld at 0x%" PRIx64 "\n", pid, start);
                result = 1;
                break;
            }
            for (size_t i = 0; i < count; i += 4) {
                bool present = true, visible = true, contiguous = true;
                uint64_t first = entries[i] & PFN_MASK;
                for (unsigned j = 0; j < 4; ++j) {
                    bool exists = (entries[i + j] >> 63) != 0;
                    uint64_t pfn = entries[i + j] & PFN_MASK;
                    c.present_pages += exists;
                    c.hidden_pfns += exists && !pfn;
                    present &= exists;
                    visible &= pfn != 0;
                    contiguous &= pfn == first + j;
                }
                c.full_groups += present;
                if (!present || !visible || !contiguous)
                    continue;
                ++c.contiguous_groups;
                if (first & 3)
                    continue;
                ++c.aligned_groups;
                c.writable_groups += mode[1] == 'w';
                c.executable_groups += mode[2] == 'x';
            }
            start += count * GUEST_PAGE;
        }
        if (result)
            break;
    }
    if (ferror(maps))
        result = 1;
    free(line);
    fclose(maps);
    close(pagemap);
    if (result)
        return result;
    if (c.minimum == UINT64_MAX)
        c.minimum = 0;
    printf("{\"pid\":%ld,\"page_bytes\":4096,\"allocation_sample_bytes\":%zu,\"readable_virtual_pages\":%" PRIu64
           ",\"sampled_present_pages\":%" PRIu64 ",\"hidden_pfns\":%" PRIu64
           ",\"vma_edge_pages_skipped\":%" PRIu64 ",\"fully_present_16k_groups\":%" PRIu64
           ",\"contiguous_16k_groups\":%" PRIu64 ",\"aligned_16k_candidates\":%" PRIu64
           ",\"writable_candidates\":%" PRIu64 ",\"executable_candidates\":%" PRIu64
           ",\"minimum_va\":%" PRIu64 ",\"maximum_va\":%" PRIu64
           ",\"candidate_fraction_of_sampled_resident\":%.6f}\n",
           pid, sample_bytes, c.virtual_pages, c.present_pages, c.hidden_pfns, c.edge_pages,
           c.full_groups, c.contiguous_groups, c.aligned_groups,
           c.writable_groups, c.executable_groups, c.minimum, c.maximum,
           c.present_pages ? 4.0 * c.aligned_groups / c.present_pages : 0.0);
    /* Linux can hide PFNs without returning an error. Refuse to report that
     * case as measured physical fragmentation. */
    if (sample)
        munmap(allocation, sample_bytes);
    return c.hidden_pfns ? 3 : 0;
}
