/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "probe.h"
#include <dispatch/dispatch.h>
extern "C" {
#include "../translation/linux-memory.h"
}
int main(int argc, char **argv)
{
    CHECK(argc == 4);
    CHECK(!std::strcmp(argv[2], "cli") || !std::strcmp(argv[2], "gcd"));
    setbuf(stdout, nullptr);
    std::printf("{\"mode\":\"%s\",\"entry\":\"%s\",\"memory_interval_ms\":%s,\"cache_bytes\":16777216}\n", argv[1], argv[2], argv[3]);
    translation_memory_start((unsigned)std::strtoul(argv[3], nullptr, 10));
    auto run = [](void *opaque) {
        auto mode = static_cast<const char *>(opaque);
        if (!std::strcmp(mode, "tests")) {
            translation_memory_phase("boundaries");
            direct_tests();
        } else
            direct_benchmark(mode);
    };
    if (!std::strcmp(argv[2], "gcd"))
        dispatch_sync_f(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), argv[1], run);
    else
        run(argv[1]);
    translation_memory_stop();
    std::puts("{\"result\":\"passed\"}");
}
