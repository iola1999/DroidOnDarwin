/* SPDX-License-Identifier: Apache-2.0 */
#include "reference_heap.h"
#include "oat/image.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

// 工具与 JNI 库使用同一份 libart 中的 Runtime、Thread 和锁状态。
int nce_art_dex2oat_main(int argc, char **argv);

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--nce-inspect-image")) {
        FILE *file = fopen(argv[2], "rb");
        if (!file)
            return 2;
        art::ImageHeader header;
        bool valid = fread(&header, sizeof(header), 1, file) == 1 && header.IsValid();
        fclose(file);
        if (!valid)
            return 3;
        printf("{\"phase\":\"art_boot_image_header\",\"header_bytes\":%zu,\"reference_base\":%llu,\"image_begin\":%llu,\"image_size\":%zu,\"oat_begin\":%llu,\"oat_end\":%llu}\n",
               sizeof(header), (unsigned long long)header.GetReferenceBase(),
               (unsigned long long)header.GetImageBegin(), header.GetImageSize(),
               (unsigned long long)header.GetOatFileBegin(), (unsigned long long)header.GetOatFileEnd());
        return 0;
    }
    std::vector<char *> arguments(argv, argv + argc);
    char base_option[64] = {};
    for (int i = 1; i < argc; ++i) {
        const char prefix[] = "--nce-image-offset=";
        if (strncmp(argv[i], prefix, sizeof(prefix) - 1))
            continue;
        char *end;
        errno = 0;
        unsigned long offset = strtoul(argv[i] + sizeof(prefix) - 1, &end, 16);
        if (base_option[0] || errno || *end || offset < 65536 || offset >= NCE_ART_HEAP_SIZE / 2 ||
            nce_art_heap_init()) {
            fputs("Invalid NCE image offset or unavailable reference heap\n", stderr);
            return 2;
        }
        // The file stores native addresses and the heap base separately; its
        // compressed object references remain offsets within this heap.
        uintptr_t address = (nce_art_reference_base + offset + 65535) & ~(uintptr_t)65535;
        snprintf(base_option, sizeof(base_option), "--base=%lx", (unsigned long)address);
        arguments[i] = base_option;
    }
    arguments.push_back(nullptr);
    return nce_art_dex2oat_main(argc, arguments.data());
}
