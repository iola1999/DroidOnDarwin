/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "code-watch.h"
#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <unistd.h>

#define PAGE_BYTES 16384
struct NceCodeWatch {
    unsigned char *ram, *alias;
    size_t size;
    /* Odd versions are protected; even versions need protection before use. */
    _Atomic uint64_t *versions;
    _Atomic uint64_t write_faults;
    _Atomic uint64_t mutation_epoch;
    NceCodeWatchStats stats;
    struct sigaction previous[2];
};
static _Atomic(NceCodeWatch *) active;
static const int signals[] = {SIGBUS, SIGSEGV};
_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2 && ATOMIC_POINTER_LOCK_FREE == 2, "signal state must be lock-free");

static void changed(NceCodeWatch *watch)
{
    if (atomic_fetch_add_explicit(&watch->mutation_epoch, 1, memory_order_release) == UINT64_MAX)
        _exit(126);
}

static int protect(NceCodeWatch *watch, size_t offset, size_t size, int protection)
{
    if (mprotect(watch->alias + offset, size, protection))
        return -1;
    if (!mprotect(watch->ram + offset, size, protection))
        return 0;
    int error = errno;
    if (mprotect(watch->alias + offset, size, PROT_READ | PROT_WRITE))
        _exit(126);
    errno = error;
    return -1;
}

static void forward(NceCodeWatch *watch, int number, siginfo_t *info, void *context)
{
    struct sigaction *previous = &watch->previous[number == SIGSEGV];
    if (previous->sa_handler == SIG_IGN)
        return;
    if (previous->sa_handler == SIG_DFL) {
        sigaction(number, previous, NULL);
        raise(number);
    } else if (previous->sa_flags & SA_SIGINFO)
        previous->sa_sigaction(number, info, context);
    else
        previous->sa_handler(number);
}

static void write_fault(int number, siginfo_t *info, void *opaque)
{
    int saved_errno = errno;
    NceCodeWatch *watch = atomic_load_explicit(&active, memory_order_acquire);
    if (!watch)
        _exit(126);
    ucontext_t *context = opaque;
    unsigned esr = context->uc_mcontext->__es.__esr;
    unsigned exception_class = esr >> 26;
    unsigned fault_class = esr & 0x3c;
    uintptr_t address = (uintptr_t)info->si_addr;
    size_t offset = address - (uintptr_t)watch->ram;
    if (offset >= watch->size)
        offset = address - (uintptr_t)watch->alias;
    /* Darwin can leave the denied write unmapped, reporting translation fault. */
    if ((exception_class == 0x24 || exception_class == 0x25) && (fault_class == 0x04 || fault_class == 0x0c) &&
        (esr & (1u << 6)) && offset < watch->size) {
        size_t index = offset / PAGE_BYTES;
        uint64_t version = atomic_load_explicit(&watch->versions[index], memory_order_acquire);
        if (version && version != UINT64_MAX) {
            // A second writer may have faulted before the first restores access.
            while (version & 1) {
                if (version == UINT64_MAX)
                    _exit(126);
                if (atomic_compare_exchange_weak_explicit(&watch->versions[index], &version, version + 1,
                                                          memory_order_acq_rel, memory_order_acquire))
                    break;
            }
            if (protect(watch, index * PAGE_BYTES, PAGE_BYTES, PROT_READ | PROT_WRITE))
                _exit(126);
            atomic_fetch_add_explicit(&watch->write_faults, 1, memory_order_relaxed);
            changed(watch);
            errno = saved_errno;
            return;
        }
    }
    errno = saved_errno;
    forward(watch, number, info, opaque);
}

NceCodeWatch *nce_code_watch_create(void *ram, void *alias, size_t size)
{
    uintptr_t first = (uintptr_t)ram, second = (uintptr_t)alias;
    if (!ram || !alias || !size || (first | second | size) % PAGE_BYTES || size > UINTPTR_MAX - first || size > UINTPTR_MAX - second ||
        !(first + size <= second || second + size <= first) || getpagesize() != PAGE_BYTES) {
        errno = EINVAL;
        return NULL;
    }
    NceCodeWatch *watch = calloc(1, sizeof(*watch));
    if (!watch)
        return NULL;
    watch->ram = ram;
    watch->alias = alias;
    watch->size = size;
    atomic_init(&watch->mutation_epoch, 1);
    watch->versions = calloc(size / PAGE_BYTES, sizeof(*watch->versions));
    if (!watch->versions) {
        free(watch);
        return NULL;
    }
    NceCodeWatch *expected = NULL;
    if (!atomic_compare_exchange_strong(&active, &expected, watch)) {
        free(watch->versions);
        free(watch);
        errno = EBUSY;
        return NULL;
    }
    struct sigaction action = {.sa_sigaction = write_fault, .sa_flags = SA_SIGINFO};
    sigemptyset(&action.sa_mask);
    sigaddset(&action.sa_mask, SIGBUS);
    sigaddset(&action.sa_mask, SIGSEGV);
    unsigned installed = 0;
    for (; installed < 2; ++installed)
        if (sigaction(signals[installed], &action, &watch->previous[installed]))
            break;
    if (installed != 2) {
        int error = errno;
        while (installed) {
            --installed;
            sigaction(signals[installed], &watch->previous[installed], NULL);
        }
        atomic_store(&active, NULL);
        free(watch->versions);
        free(watch);
        errno = error;
        return NULL;
    }
    return watch;
}

uint64_t nce_code_watch_version(NceCodeWatch *watch, const void *ram_address)
{
    size_t offset = (uintptr_t)ram_address - (uintptr_t)watch->ram;
    if (offset >= watch->size)
        return 0;
    ++watch->stats.queries;
    size_t index = offset / PAGE_BYTES;
    uint64_t version = atomic_load_explicit(&watch->versions[index], memory_order_acquire);
    if (version == UINT64_MAX)
        return 0;
    if (version & 1)
        return version;
    if (protect(watch, index * PAGE_BYTES, PAGE_BYTES, PROT_READ)) {
        ++watch->stats.protection_failures;
        return 0;
    }
    ++watch->stats.protections;
    atomic_store_explicit(&watch->versions[index], version + 1, memory_order_release);
    return version + 1;
}

int nce_code_watch_invalidate(NceCodeWatch *watch, size_t offset, size_t size)
{
    if (!size || (offset | size) % PAGE_BYTES || offset >= watch->size || size > watch->size - offset) {
        errno = EINVAL;
        return -1;
    }
    if (protect(watch, offset, size, PROT_READ | PROT_WRITE))
        return -1;
    for (size_t index = offset / PAGE_BYTES; index < (offset + size) / PAGE_BYTES; ++index) {
        uint64_t version = atomic_load_explicit(&watch->versions[index], memory_order_acquire);
        if (!version || version == UINT64_MAX)
            continue;
        if (version >= UINT64_MAX - 1)
            _exit(126);
        atomic_store_explicit(&watch->versions[index], (version + 2) & ~UINT64_C(1), memory_order_release);
        ++watch->stats.invalidations;
    }
    changed(watch);
    return 0;
}

uint64_t nce_code_watch_epoch(const NceCodeWatch *watch)
{
    return atomic_load_explicit(&watch->mutation_epoch, memory_order_acquire);
}

int nce_code_watch_exclude(NceCodeWatch *watch, size_t offset, size_t size)
{
    if (nce_code_watch_invalidate(watch, offset, size))
        return -1;
    for (size_t index = offset / PAGE_BYTES; index < (offset + size) / PAGE_BYTES; ++index) {
        if (atomic_exchange_explicit(&watch->versions[index], UINT64_MAX, memory_order_acq_rel) != UINT64_MAX)
            ++watch->stats.excluded_pages;
    }
    return 0;
}

NceCodeWatchStats nce_code_watch_stats(const NceCodeWatch *watch)
{
    NceCodeWatchStats stats = watch->stats;
    stats.write_faults = atomic_load_explicit(&watch->write_faults, memory_order_relaxed);
    return stats;
}

void nce_code_watch_destroy(NceCodeWatch *watch)
{
    if (protect(watch, 0, watch->size, PROT_READ | PROT_WRITE))
        _exit(126);
    for (unsigned i = 0; i < 2; ++i)
        if (sigaction(signals[i], &watch->previous[i], NULL))
            _exit(126);
    atomic_store_explicit(&active, NULL, memory_order_release);
    free(watch->versions);
    free(watch);
}
