/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "probe.h"
#include <dispatch/dispatch.h>
#include <pthread.h>
extern "C" {
#include "../translation/linux-memory.h"
}
int main(int argc, char **argv)
{
    CHECK(argc == 4);
    CHECK(!std::strcmp(argv[2], "cli") || !std::strcmp(argv[2], "gcd"));
    setbuf(stdout, nullptr);
    std::printf("{\"direct_code_memory\":\"%s\"}\n", nce_direct_code_memory());
    std::printf("{\"mode\":\"%s\",\"entry\":\"%s\",\"memory_interval_ms\":%s,\"cache_bytes\":16777216}\n", argv[1], argv[2], argv[3]);
    translation_memory_start((unsigned)std::strtoul(argv[3], nullptr, 10));
    struct Invocation {
        const char *mode;
        bool gcd;
    } invocation{argv[1], !std::strcmp(argv[2], "gcd")};
    auto run = [](void *opaque) {
        const auto &invocation = *static_cast<const Invocation *>(opaque);
        CHECK(bool(pthread_main_np()) != invocation.gcd);
        auto mode = invocation.mode;
        if (!std::strcmp(mode, "tests")) {
            translation_memory_phase("boundaries");
            direct_tests();
        } else
            direct_benchmark(mode);
    };
    if (invocation.gcd) {
        dispatch_group_t group = dispatch_group_create();
        dispatch_group_async_f(group, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), &invocation, run);
        CHECK(!dispatch_group_wait(group, DISPATCH_TIME_FOREVER));
        dispatch_release(group);
    } else
        run(&invocation);
    translation_memory_stop();
    std::puts("{\"result\":\"passed\"}");
}
