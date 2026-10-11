/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "direct.h"
#include <unicorn/unicorn.h>
#include <unicorn/arm64.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
constexpr uint64_t CodeVA = 0x10000, DataVA = 0x20000;
constexpr size_t DataBytes = 65536;
#define CHECK(x) \
    do { \
        if (!(x)) { \
            std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); \
            std::exit(1); \
        } \
    } while (0)
inline void checked(uc_err error)
{
    if (error != UC_ERR_OK) {
        std::fprintf(stderr, "Unicorn: %s\n", uc_strerror(error));
        std::exit(1);
    }
}
struct Probe {
    alignas(16384) std::array<uint32_t, 8192> code{};
    alignas(16384) std::array<uint64_t, DataBytes / 8> data{};
    alignas(16384) std::array<uint64_t, DataBytes / 8> reference{};
    uc_engine *uc = nullptr;
    NceDirect *direct = nullptr;
    NceDirectConfig config{};
    bool readable = true, writable = true, alias_code = false;
    bool executable = true;
    size_t alias_offset = 0;
    uint64_t extra_code_va = 0;
    uint64_t epoch = 1, fills = 0;
    explicit Probe(const char *mode = "tests");
    ~Probe();
    NceDirectMemory memory()
    {
        return {this, Map, Code, epoch, 12, nullptr, false};
    }
    static void *Map(void *, uint64_t, bool);
    static const void *Code(void *, uint64_t);
    static bool Fill(uc_engine *, uint64_t, uc_mem_type, uc_tlb_entry *, void *);
    void load(const void *bytes, size_t size);
    void reference_run(NceDirectState &, uint64_t stop);
    NceDirectExit run(NceDirectState &state, uint64_t budget = 1000000, uint64_t *fault = nullptr);
    void compare(NceDirectState, size_t instructions);
};
void direct_tests();
void direct_page_table_tests();
void direct_benchmark(const char *engine);
