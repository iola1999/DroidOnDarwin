/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "husk-arm64-fastmem.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <mach/mach.h>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>

HuskArm64Fastmem::HuskArm64Fastmem()
{
    profile = std::getenv("HUSK_ARM64_STATS") != nullptr;
    host_page = static_cast<size_t>(getpagesize());
    if (host_page != 4096 && host_page != 16384)
        return;
    void *reservation = mmap(nullptr, segment_size, PROT_NONE,
                             MAP_PRIVATE | MAP_ANON, -1, 0);
    if (reservation == MAP_FAILED)
        return;
    guard = reinterpret_cast<uintptr_t>(reservation);
    for (auto &window : windows)
        window.bases.fill(guard);
}

HuskArm64Fastmem::~HuskArm64Fastmem()
{
    if (!guard)
        return;
    for (auto &window : windows)
        for (uintptr_t base : window.bases)
            if (base != guard)
                munmap(reinterpret_cast<void *>(base), segment_size);
    munmap(reinterpret_cast<void *>(guard), segment_size);
}

bool HuskArm64Fastmem::map(uint64_t address, bool write, HuskArm64Memory memory, bool ignore_top_byte)
{
    bool result = map_validated(address, write, memory, ignore_top_byte);
    installed += result;
    ++attempts;
    if (result && installed == 1)
        std::fprintf(stderr, "[husk-fastmem] first RAM alias installed\n");
    if (profile && !(attempts & (attempts - 1)))
        std::fprintf(stderr, "[husk-fastmem] attempts=%llu installed=%llu revoked=%llu last=%s\n",
                     (unsigned long long)attempts, (unsigned long long)installed,
                     (unsigned long long)revoked, result ? "mapped" : rejection);
    return result;
}

bool HuskArm64Fastmem::map_validated(uint64_t address, bool write, HuskArm64Memory memory, bool ignore_top_byte)
{
    uint64_t tag = ignore_top_byte ? address & UINT64_C(0xff00000000000000) : 0;
    address &= ~tag;
    uint64_t original_page = address & ~(uint64_t(host_page) - 1);
    if (!guard || !memory.map_page || !compact_address(address, false))
        return false;
    uint64_t page = address & ~(uint64_t(host_page) - 1);
    uintptr_t physical = 0;
    rejection = "mmu";
    for (size_t offset = 0; offset < host_page; offset += 4096) {
        auto host = reinterpret_cast<uintptr_t>(memory.map_page(memory.opaque, (original_page + offset) | tag, write));
        if (!host)
            return false;
        if (!offset)
            physical = host;
        if (physical & (host_page - 1)) {
            rejection = "alignment";
            return false;
        }
        if (host != physical + offset) {
            rejection = "fragmented";
            return false;
        }
    }
    auto &window = windows[write];
    if (std::any_of(window.mapped.begin(), window.mapped.end(),
                    [page](const auto &p) { return p.guest == page; })) {
        rejection = "already-mapped";
        return false;
    }
    size_t index = page >> segment_bits;
    auto &base = window.bases[index];
    if (base == guard) {
        void *reservation = mmap(nullptr, segment_size, PROT_NONE,
                                 MAP_PRIVATE | MAP_ANON, -1, 0);
        if (reservation == MAP_FAILED) {
            rejection = "reservation";
            return false;
        }
        base = reinterpret_cast<uintptr_t>(reservation);
    }
    vm_address_t target = base + (page & (segment_size - 1));
    vm_address_t actual = target;
    vm_prot_t current, maximum;
    /* Reserve bookkeeping before changing permissions, so allocation failure
     * cannot leave an untracked writable mapping. */
    window.mapped.push_back({page, physical});
    kern_return_t result = vm_remap(mach_task_self(), &actual, host_page, 0,
                                    VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(), physical, false,
                                    &current, &maximum, VM_INHERIT_NONE);
    if (result != KERN_SUCCESS) {
        rejection = "remap";
        window.mapped.pop_back();
        return false;
    }
    vm_prot_t permissions = VM_PROT_READ | (write ? VM_PROT_WRITE : 0);
    if (actual != target || (maximum & permissions) != permissions ||
        vm_protect(mach_task_self(), actual, host_page, false, permissions) != KERN_SUCCESS)
        throw std::runtime_error("fastmem mapping permissions failed");
    return true;
}

void HuskArm64Fastmem::reset()
{
    for (auto &window : windows) {
        for (auto page : window.mapped)
            revoke(window, page.guest);
        window.mapped.clear();
    }
}

void HuskArm64Fastmem::revoke(Window &window, uint64_t page)
{
    vm_address_t address = window.bases[page >> segment_bits] + (page & (segment_size - 1));
    if (vm_protect(mach_task_self(), address, host_page, false, VM_PROT_NONE) != KERN_SUCCESS)
        throw std::runtime_error("fastmem mapping revocation failed");
    ++revoked;
}

void HuskArm64Fastmem::protect_code(const void *code)
{
    uintptr_t physical = reinterpret_cast<uintptr_t>(code);
    auto &window = windows[1];
    std::erase_if(window.mapped, [&](const auto &p) {
        if (p.host < physical + 4096 && p.host + host_page > physical) {
            revoke(window, p.guest);
            return true;
        }
        return false;
    });
}
