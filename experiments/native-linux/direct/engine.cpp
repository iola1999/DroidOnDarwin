/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "internal.h"
#include <cstring>
#include <atomic>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <sys/mman.h>
#include <sys/sysctl.h>
using namespace nce_direct;

struct NceDirect {
    uint32_t *arena;
    NceDirectStats stats{};
    uint32_t supported = 0;
    std::unordered_map<uint64_t, std::unique_ptr<Page>> pages;
    std::array<Mapping, 256> mappings{};
    std::array<CodeRoute, 256> routes{};
    std::unordered_set<const void *> active_code;
    NceDirectMemory previous{};
    std::atomic<uint32_t> stop_request{0};
    static_assert(std::atomic<uint32_t>::is_always_lock_free);

    explicit NceDirect(size_t capacity)
    {
        if (capacity < 2 * Page::max_bytes || capacity > 256 * Page::max_bytes || capacity % 16384)
            throw std::invalid_argument("Direct cache must be 2..256 MiB, aligned to 16 KiB");
        arena = static_cast<uint32_t *>(mmap(nullptr, capacity, PROT_READ | PROT_WRITE | PROT_EXEC,
                                             MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0));
        if (arena == MAP_FAILED)
            throw std::bad_alloc();
        stats.code_capacity = capacity;
        mappings.fill(EmptyMapping);
        const std::pair<const char *, uint32_t> features[] = {
            {"hw.optional.arm.FEAT_LSE", NCE_DIRECT_LSE}, {"hw.optional.armv8_crc32", NCE_DIRECT_CRC}, {"hw.optional.arm.FEAT_AES", NCE_DIRECT_AES}, {"hw.optional.arm.FEAT_SHA1", NCE_DIRECT_SHA1}, {"hw.optional.arm.FEAT_SHA256", NCE_DIRECT_SHA256}};
        for (auto feature : features) {
            int available = 0;
            size_t size = sizeof(available);
            if (!sysctlbyname(feature.first, &available, &size, nullptr, 0) && available)
                supported |= feature.second;
        }
    }
    ~NceDirect()
    {
        munmap(arena, stats.code_capacity);
    }
    void reset_routes()
    {
        routes.fill({UINT64_MAX, nullptr, nullptr, 0});
    }
    void clear()
    {
        reset_routes();
        pages.clear();
        stats.code_bytes = 0;
        ++stats.flushes;
    }
    Page *resolve(uint64_t pc, uint32_t flags, NceDirectMemory memory)
    {
        ++stats.code_lookups;
        uint64_t address = pc & ~UINT64_C(4095);
        const void *bytes = memory.map_code(memory.opaque, address);
        if (!bytes || (uintptr_t(bytes) & 4095))
            return nullptr;
        // A newly executable alias must not retain a previously writable mapping.
        if (active_code.insert(bytes).second)
            for (auto &mapping : mappings)
                mapping.write_tag = UINT64_MAX;
        auto item = pages.find(address);
        if (item != pages.end() &&
            (item->second->flags != flags || std::memcmp(item->second->source.data(), bytes, 4096))) {
            reset_routes();
            pages.erase(item);
            ++stats.code_changes;
        }
        size_t index = (pc & 4095) / 4;
        item = pages.find(address);
        if (item == pages.end() || !item->second->entries[index]) {
            if (stats.code_capacity - stats.code_bytes < Page::max_bytes)
                clear();
            auto &owner = pages[address];
            if (!owner)
                owner = std::make_unique<Page>();
            Page pending = *owner;
            pending.flags = flags;
            auto *destination = arena + stats.code_bytes / 4;
            size_t bytes_used = pending.compile(address, bytes, index, destination, destination,
                                                flags, stats.code_capacity - stats.code_bytes,
                                                owner->branch_entries.data());
            *owner = std::move(pending);
            stats.code_bytes += (bytes_used + 15) & ~size_t(15);
            ++stats.compiles;
        }
        auto *page = pages.at(address).get();
        routes[(address >> 12) % routes.size()] = {address >> 12, page->branch_entries.data(), page, 0};
        return page;
    }
    NceDirectExit run(NceDirectState &state, NceDirectMemory memory, uint64_t budget, uint64_t &fault)
    {
        if (!memory.map_page || !memory.map_code || !budget || budget > INT64_MAX)
            return NCE_DIRECT_ERROR;
        if (memory.page_bits && memory.page_bits != 12 && memory.page_bits != 14)
            return NCE_DIRECT_ERROR;
        unsigned page_bits = memory.page_bits ? memory.page_bits : 12;
        uint64_t page_mask = (UINT64_C(1) << page_bits) - 1;
        if ((state.pc & 3) || (state.fpcr & ~DIRECT_FPCR_MASK))
            return NCE_DIRECT_UNSUPPORTED;
        uint32_t flags = state.access_flags & 0xf;
        if (page_bits == 14)
            flags |= CompileData16K;
        flags &= ~uint32_t(NCE_DIRECT_LSE);
        uint32_t allowed = supported & state.access_flags;
        flags |= allowed & (NCE_DIRECT_LSE | NCE_DIRECT_CRC);
        if (supported & NCE_DIRECT_LSE)
            flags |= CompileExclusive;
        if (allowed & NCE_DIRECT_AES)
            flags |= CompileAes;
        if (allowed & NCE_DIRECT_SHA1)
            flags |= CompileSha1;
        if (allowed & NCE_DIRECT_SHA256)
            flags |= CompileSha256;
        if (state.access_flags & NCE_DIRECT_DC_ZVA64)
            flags |= CompileZva64;
        if (state.access_flags & NCE_DIRECT_PAC_HINT_NOP)
            flags |= CompilePauthNop;
        if (!memory.mapping_epoch || memory.mapping_epoch != previous.mapping_epoch ||
            memory.opaque != previous.opaque || memory.map_page != previous.map_page || memory.page_bits != previous.page_bits)
            mappings.fill(EmptyMapping);
        previous = memory;
        reset_routes();
        active_code.clear();
        stop_request.store(0, std::memory_order_release);
        Frame frame{};
        frame.state = state;
        frame.budget = budget;
        frame.code_routes = routes.data();
        frame.exit_stub = nce_direct_exit_stub;
        frame.branch_stub = nce_direct_branch_stub;
        frame.stop_request = &stop_request;
        NceDirectExit result = NCE_DIRECT_ERROR;
        Page *page = resolve(state.pc, flags, memory);
        void *entry = page ? page->branch_entries[(state.pc & 4095) / 4] : nullptr;
        while (page) {
            frame.code_page = frame.state.pc & ~UINT64_C(4095);
            frame.code = page;
            nce_direct_enter(&frame, entry, mappings.data());
            page = frame.code;
            if (frame.reason == Budget) {
                result = NCE_DIRECT_BUDGET;
                break;
            }
            if (frame.reason == Fallback) {
                result = NCE_DIRECT_UNSUPPORTED;
                break;
            }
            if (frame.reason == CodeBranch) {
                if (frame.state.pc & 3) {
                    result = NCE_DIRECT_UNSUPPORTED;
                    break;
                }
                page = resolve(frame.state.pc, flags, memory);
                entry = page ? page->branch_entries[(frame.state.pc & 4095) / 4] : nullptr;
                continue;
            }
            bool write = frame.reason == WritePage;
            if (!write && frame.reason != ReadPage)
                break;
            ++stats.mapping_misses;
            uint64_t address = frame.address & ~page_mask;
            void *mapped = memory.map_page(memory.opaque, address, write);
            bool code_alias = false;
            if (mapped && write)
                for (unsigned offset = 0; offset <= page_mask; offset += 4096)
                    code_alias |= active_code.count(static_cast<char *>(mapped) + offset) != 0;
            if (!mapped || (uintptr_t(mapped) & page_mask) || code_alias) {
                fault = frame.address;
                result = write ? NCE_DIRECT_WRITE_FAULT : NCE_DIRECT_READ_FAULT;
                break;
            }
            uint64_t tag = address >> page_bits;
            auto &mapping = mappings[(tag ^ (tag >> 8)) % mappings.size()];
            if (write) {
                mapping.write_tag = tag;
                mapping.write_addend = uintptr_t(mapped) - address;
            } else {
                mapping.read_tag = tag;
                mapping.read_addend = uintptr_t(mapped) - address;
            }
            entry = page->entries[(frame.state.pc & 4095) / 4];
        }
        if (!page) {
            fault = frame.state.pc;
            result = NCE_DIRECT_FETCH_FAULT;
        }
        state = frame.state;
        return result;
    }
};
extern "C" NceDirect *nce_direct_create(size_t bytes)
{
    try {
        return new NceDirect(bytes);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "Direct create: %s\n", error.what());
        return nullptr;
    }
}
extern "C" void nce_direct_destroy(NceDirect *engine)
{
    delete engine;
}
extern "C" NceDirectExit nce_direct_run(NceDirect *engine, NceDirectState *state, NceDirectMemory memory,
                                        uint64_t budget, uint64_t *fault)
{
    if (!engine || !state || !fault)
        return NCE_DIRECT_ERROR;
    try {
        return engine->run(*state, memory, budget, *fault);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "Direct run: %s\n", error.what());
        engine->clear();
        return NCE_DIRECT_ERROR;
    }
}
extern "C" NceDirectStats nce_direct_stats(const NceDirect *engine)
{
    if (!engine)
        return {};
    NceDirectStats result = engine->stats;
    result.pages = engine->pages.size();
    return result;
}

extern "C" void nce_direct_request_stop(NceDirect *engine)
{
    engine->stop_request.store(1, std::memory_order_release);
}
