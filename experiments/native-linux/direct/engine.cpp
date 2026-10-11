/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "internal.h"
#include <algorithm>
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
    NceDirectConfig config;
    NceDirectStats stats{};
    uint32_t supported = 0;
    std::unordered_map<uint64_t, std::unique_ptr<Page>> pages;
    std::vector<Mapping> mappings;
    std::vector<CodeRoute> routes;
    std::unordered_set<const void *> active_code;
    std::unordered_multimap<uintptr_t, unsigned> write_aliases;
    std::vector<uint64_t> code_page_runs;
    uint64_t run_id = 0;
    uint64_t route_epoch = 0;
    uint32_t previous_flags = 0;
    bool volatile_routes = false;
    NceDirectMemory previous{};
    std::atomic<uint32_t> stop_request{0};
    static_assert(std::atomic<uint32_t>::is_always_lock_free);

    static unsigned entries(unsigned count)
    {
        if (count < 256 || count > 65536 || (count & (count - 1)))
            throw std::invalid_argument("Direct entry count must be a power of two from 256 through 65536");
        return count;
    }
    explicit NceDirect(NceDirectConfig value)
        : config(value), mappings(entries(value.mapping_entries), EmptyMapping), routes(entries(value.route_entries))
    {
        if (config.retain_routes && !config.route_epochs)
            throw std::invalid_argument("Retained routes require route epochs");
        size_t capacity = config.cache_bytes;
        if (capacity < 2 * Page::max_bytes || capacity > 256 * Page::max_bytes || capacity % 16384)
            throw std::invalid_argument("Direct cache must be 2..256 MiB, aligned to 16 KiB");
        arena = static_cast<uint32_t *>(mmap(nullptr, capacity, PROT_READ | PROT_WRITE | PROT_EXEC,
                                             MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0));
        if (arena == MAP_FAILED)
            throw std::bad_alloc();
        stats.code_capacity = capacity;
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
        ++stats.route_invalidations;
        volatile_routes = false;
        if (config.route_epochs && ++route_epoch)
            return;
        std::fill(routes.begin(), routes.end(), CodeRoute{UINT64_MAX, nullptr, nullptr, 0});
        stats.route_cleared_entries += routes.size();
        route_epoch = 1;
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
        size_t index = (pc & 4095) / 4;
        auto item = pages.find(address);
        // During one run mappings are stable and stores to visited code are
        // rejected. A route-table collision can therefore reuse this check.
        if (config.reuse_code_checks && item != pages.end() &&
            item->second->validated_run == run_id && item->second->entries[index]) {
            ++stats.code_validation_hits;
            auto *page = item->second.get();
            volatile_routes |= !page->source_version;
            routes[(address >> 12) & (routes.size() - 1)] = {address >> 12, page->branch_entries.data(), page, route_epoch};
            return page;
        }
        const void *bytes = memory.map_code(memory.opaque, address);
        if (!bytes || (uintptr_t(bytes) & 4095))
            return nullptr;
        // A newly executable alias must not retain a previously writable mapping.
        if (memory.page_table && !memory.refill_reads) {
            const auto &table = *memory.page_table;
            uintptr_t offset = uintptr_t(bytes) - uintptr_t(table.ram);
            if (offset >= table.ram_size)
                return nullptr;
            code_page_runs[offset >> 14] = run_id;
        } else if (active_code.insert(bytes).second) {
            unsigned page_bits = memory.page_bits ? memory.page_bits : 12;
            uintptr_t physical_page = uintptr_t(bytes) & ~((uintptr_t(1) << page_bits) - 1);
            if (config.selective_writes) {
                auto [begin, end] = write_aliases.equal_range(physical_page);
                for (auto it = begin; it != end; ++it) {
                    mappings[it->second].write_tag = UINT64_MAX;
                    ++stats.write_invalidations;
                }
                write_aliases.erase(begin, end);
            } else {
                for (auto &mapping : mappings) {
                    if (mapping.write_tag != UINT64_MAX)
                        ++stats.write_invalidations;
                    mapping.write_tag = UINT64_MAX;
                }
            }
        }
        uint64_t version = memory.code_version ? memory.code_version(memory.opaque, bytes) : 0;
        bool content_changed = false;
        if (item != pages.end()) {
            const Page &page = *item->second;
            if (version && page.source_pointer == bytes && page.source_version == version)
                ++stats.code_version_hits;
            else {
                ++stats.code_content_checks;
                content_changed = std::memcmp(page.source.data(), bytes, 4096) != 0;
            }
        }
        if (item != pages.end() && (item->second->flags != flags || content_changed)) {
            reset_routes();
            pages.erase(item);
            ++stats.code_changes;
        }
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
            auto compiled = pending.compile(address, bytes, index, destination, flags,
                                            owner->branch_entries.data(), config.mapping_entries - 1);
            *owner = std::move(pending);
            stats.code_bytes += (compiled.bytes + 15) & ~size_t(15);
            stats.memory_groups += compiled.memory_groups;
            stats.memory_group_instructions += compiled.memory_group_instructions;
            ++stats.compiles;
        }
        auto *page = pages.at(address).get();
        page->validated_run = run_id;
        page->source_pointer = bytes;
        page->source_version = version;
        volatile_routes |= !version;
        routes[(address >> 12) & (routes.size() - 1)] = {address >> 12, page->branch_entries.data(), page, route_epoch};
        return page;
    }
    bool cross_page(Frame &frame, Page *page, NceDirectMemory memory, NceDirectExit &result, uint64_t &fault)
    {
        result = NCE_DIRECT_UNSUPPORTED;
        if (!config.cross_page)
            return false;
        uint32_t word = page->source[(frame.state.pc & 4095) / 4];
        Kind kind = decode(word);
        if (kind != Memory && kind != Pair && kind != Literal && kind != Structure && kind != StructureLane)
            return false;
        auto access = memory_access(word, kind);
        if (!access.bytes || access.bytes > 64 || frame.address > UINT64_MAX - (access.bytes - 1))
            return false;
        unsigned page_bits = memory.page_bits ? memory.page_bits : 12;
        uint64_t mask = (UINT64_C(1) << page_bits) - 1;
        unsigned first = unsigned((mask + 1) - (frame.address & mask));
        if (first >= access.bytes)
            return false;
        std::array<unsigned char *, 2> mapped{};
        uint64_t addresses[] = {frame.address, frame.address + first};
        for (unsigned index = 0; index < 2; ++index) {
            auto *ram = static_cast<unsigned char *>(memory.map_page(memory.opaque, addresses[index] & ~mask, access.write));
            bool valid = ram && !(uintptr_t(ram) & mask);
            if (valid && memory.page_table && !memory.refill_reads) {
                uintptr_t offset = uintptr_t(ram) - uintptr_t(memory.page_table->ram);
                valid = offset < memory.page_table->ram_size;
                if (valid && access.write)
                    valid = code_page_runs[offset >> 14] != run_id;
            } else if (valid && access.write) {
                for (unsigned offset = 0; offset <= mask; offset += 4096)
                    valid &= active_code.count(ram + offset) == 0;
            }
            if (!valid) {
                fault = addresses[index];
                result = access.write ? NCE_DIRECT_WRITE_FAULT : NCE_DIRECT_READ_FAULT;
                return false;
            }
            mapped[index] = ram + (addresses[index] & mask);
        }
        // Both pages are ordinary RAM and remain mapped until this run returns.
        // Validate the second page before a store can change the first one.
        std::array<unsigned char, 64> bytes{};
        if (!access.write) {
            std::memcpy(bytes.data(), mapped[0], first);
            std::memcpy(bytes.data() + first, mapped[1], access.bytes - first);
        }
        transfer_cross_page(word, kind, access, frame.state, bytes.data(), frame.address);
        if (access.write) {
            std::memcpy(mapped[0], bytes.data(), first);
            std::memcpy(mapped[1], bytes.data() + first, access.bytes - first);
        }
        frame.state.pc += 4;
        stats.cross_page_bytes += access.bytes;
        return true;
    }
    NceDirectExit run(NceDirectState &state, NceDirectMemory memory, uint64_t budget, uint64_t &fault)
    {
        if (!memory.map_page || !memory.map_code || !budget || budget > INT64_MAX)
            return NCE_DIRECT_ERROR;
        if (memory.page_bits && memory.page_bits != 12 && memory.page_bits != 14)
            return NCE_DIRECT_ERROR;
        if (memory.refill_reads && !memory.page_table)
            return NCE_DIRECT_ERROR;
        if (memory.flat_pages && (!memory.page_table || memory.refill_reads ||
                                  memory.page_table->user_size != (UINT64_C(1) << DIRECT_FLAT_USER_BITS)))
            return NCE_DIRECT_ERROR;
        if (memory.page_table) {
            const auto &table = *memory.page_table;
            if (memory.page_bits != 14 || !table.root || (uintptr_t(table.root) & 16383) ||
                !table.ram || (uintptr_t(table.ram) & 16383) || (table.physical_base & 16383) ||
                !table.ram_size || (table.ram_size & 16383) || table.ram_size > (UINT64_C(1) << 32) ||
                !table.user_size || table.user_size > UINT64_MAX - table.user_base ||
                table.user_base >= (UINT64_C(1) << 47) || table.user_size > (UINT64_C(1) << 47) - table.user_base ||
                (table.user_base & 16383) || (table.user_size & 16383) ||
                table.ram_size > UINT64_MAX - table.physical_base || table.ram_size > UINTPTR_MAX - uintptr_t(table.ram))
                return NCE_DIRECT_ERROR;
            if (!memory.refill_reads)
                code_page_runs.resize(table.ram_size >> 14);
        }
        unsigned page_bits = memory.page_bits ? memory.page_bits : 12;
        uint64_t page_mask = (UINT64_C(1) << page_bits) - 1;
        if ((state.pc & 3) || (state.fpcr & ~DIRECT_FPCR_MASK))
            return NCE_DIRECT_UNSUPPORTED;
        if (memory.code_version != previous.code_version || memory.opaque != previous.opaque)
            clear();
        uint32_t flags = state.access_flags & NCE_DIRECT_UNALIGNED;
        if (page_bits == 14)
            flags |= CompileData16K;
        if (memory.page_table)
            flags |= CompilePageTable;
        if (memory.refill_reads)
            flags |= CompilePteReadRefill;
        if (memory.flat_pages)
            flags |= CompileFlat;
        if (config.memory_groups)
            flags |= CompileMemoryGroups;
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
        bool retain = config.retain_routes && config.route_epochs && !volatile_routes &&
                      memory.page_table && !memory.refill_reads && memory.mapping_epoch &&
                      memory.code_epoch && memory.code_version && memory.code_epoch == previous.code_epoch &&
                      memory.mapping_epoch == previous.mapping_epoch && flags == previous_flags &&
                      memory.opaque == previous.opaque && memory.map_code == previous.map_code &&
                      memory.map_page == previous.map_page && memory.code_version == previous.code_version &&
                      memory.page_bits == previous.page_bits && !previous.refill_reads && previous.page_table;
        if (retain) {
            ++stats.route_reused_runs;
        } else {
            if (++run_id == 0) {
                clear();
                std::fill(code_page_runs.begin(), code_page_runs.end(), 0);
                run_id = 1;
            }
            reset_routes();
        }
        // A retained validation interval keeps its visited physical pages
        // write-protected by the generated checks, including cached branches.
        if (!memory.mapping_epoch || memory.mapping_epoch != previous.mapping_epoch ||
            memory.opaque != previous.opaque || memory.map_page != previous.map_page || memory.page_bits != previous.page_bits) {
            std::fill(mappings.begin(), mappings.end(), EmptyMapping);
            write_aliases.clear();
        }
        previous = memory;
        previous_flags = flags;
        active_code.clear();
        stop_request.store(0, std::memory_order_release);
        Frame frame{};
        frame.state = state;
        frame.budget = budget;
        frame.code_routes = routes.data();
        frame.exit_stub = nce_direct_exit_stub;
        frame.branch_stub = config.route_epochs ? nce_direct_branch_epoch_stub : nce_direct_branch_stub;
        frame.stop_request = &stop_request;
        frame.route_mask = config.route_entries - 1;
        if (memory.page_table) {
            frame.page_table = *memory.page_table;
            frame.code_page_runs = code_page_runs.data();
            frame.run_id = run_id;
            frame.read_pte_stub = nce_direct_read_pte;
            frame.write_pte_stub = nce_direct_write_pte;
            frame.pte_refill_mask = memory.refill_reads ? config.mapping_entries - 1 : 0;
        }
        NceDirectExit result = NCE_DIRECT_ERROR;
        Page *page = resolve(state.pc, flags, memory);
        void *entry = page ? page->branch_entries[(state.pc & 4095) / 4] : nullptr;
        while (page) {
            frame.code_page = frame.state.pc & ~UINT64_C(4095);
            frame.code = page;
            frame.route_epoch = route_epoch;
            nce_direct_enter(&frame, entry, memory.flat_pages ? static_cast<const void *>(memory.flat_pages) : mappings.data());
            page = frame.code;
            if (frame.reason == Budget) {
                result = NCE_DIRECT_BUDGET;
                break;
            }
            if (frame.reason == CrossPage) {
                ++stats.cross_page_exits;
                if (cross_page(frame, page, memory, result, fault)) {
                    ++stats.cross_page_completed;
                    page = resolve(frame.state.pc, flags, memory);
                    entry = page ? page->branch_entries[(frame.state.pc & 4095) / 4] : nullptr;
                    continue;
                }
                if (result == NCE_DIRECT_UNSUPPORTED)
                    ++stats.cross_page_fallbacks;
                else
                    ++stats.cross_page_faults;
                break;
            }
            if (frame.reason == Fallback || frame.reason == Alignment) {
                stats.alignment_exits += frame.reason == Alignment;
                result = NCE_DIRECT_UNSUPPORTED;
                break;
            }
            if (frame.reason == CodeBranch) {
                ++stats.branch_exits;
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
            if (write)
                ++stats.write_mapping_misses;
            else
                ++stats.read_mapping_misses;
            if (memory.page_table && !memory.refill_reads) {
                fault = frame.address;
                result = write ? NCE_DIRECT_WRITE_FAULT : NCE_DIRECT_READ_FAULT;
                break;
            }
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
            unsigned slot = (tag ^ (tag >> 8)) & (mappings.size() - 1);
            auto &mapping = mappings[slot];
            if (write) {
                if (config.selective_writes) {
                    if (mapping.write_tag != UINT64_MAX) {
                        uintptr_t old_page = mapping.write_addend + (mapping.write_tag << page_bits);
                        auto [begin, end] = write_aliases.equal_range(old_page);
                        for (auto it = begin; it != end; ++it) {
                            if (it->second == slot) {
                                write_aliases.erase(it);
                                break;
                            }
                        }
                    }
                    write_aliases.emplace(uintptr_t(mapped), slot);
                }
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
        stats.pte_read_refills += frame.pte_read_refills;
        state = frame.state;
        return result;
    }
};
extern "C" NceDirect *nce_direct_create(NceDirectConfig config)
{
    try {
        return new NceDirect(config);
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
