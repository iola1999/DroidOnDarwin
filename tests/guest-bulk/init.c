/* SPDX-License-Identifier: GPL-2.0-or-later */
typedef unsigned long u64;
typedef long s64;
static s64 call(u64 number, u64 a, u64 b, u64 c, u64 d, u64 e, u64 f)
{
    register u64 x8 __asm__("x8") = number;
    register u64 x0 __asm__("x0") = a;
    register u64 x1 __asm__("x1") = b;
    register u64 x2 __asm__("x2") = c;
    register u64 x3 __asm__("x3") = d;
    register u64 x4 __asm__("x4") = e;
    register u64 x5 __asm__("x5") = f;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2),
                                          "r"(x3), "r"(x4), "r"(x5) : "memory", "cc");
    return x0;
}

static void print(const char *s)
{
    u64 n = 0;
    while (s[n])
        ++n;
    call(64, 1, (u64)s, n, 0, 0, 0);
}

void guest_main(void)
{
    int success = call(40, (u64) "sysfs", (u64) "/sys", (u64) "sysfs", 0, 0, 0) == 0;
    success &= call(40, (u64) "proc", (u64) "/proc", (u64) "proc", 0, 0, 0) == 0;
    for (unsigned sample = 0; success && sample < 3; ++sample) {
        for (unsigned mode = 0; mode < 3; ++mode) {
            print(mode == 0 ? "BEGIN stock\n" : mode == 1 ? "BEGIN disabled\n"
                                                          : "BEGIN enabled\n");
            s64 child = call(220, 17, 0, 0, 0, 0, 0);
            if (child < 0) {
                success = 0;
                break;
            }
            if (!child) {
                const char *argv[] = {"/system/bin/husk-bulk-bench", 0};
                const char *env[] = {"LD_PRELOAD=/libhusk_bulk.so", "HUSK_BULK_STATS=1",
                                     mode == 1 ? "HUSK_BULK_DISABLE=1" : "HUSK_BULK_DISABLE=0", 0};
                const char *empty[] = {0};
                call(221, (u64)argv[0], (u64)argv, (u64)(mode ? env : empty), 0, 0, 0);
                print("EXEC failed\n");
                call(93, 127, 0, 0, 0, 0, 0);
                for (;;) {
                }
            }
            int status = -1;
            success &= call(260, child, (u64)&status, 0, 0, 0, 0) == child && status == 0;
            print(success ? "END PASS\n" : "END FAIL\n");
            if (!success)
                break;
        }
    }
    print(success ? "HUSK-BIONIC PASS\n" : "HUSK-BIONIC FAIL\n");
    call(142, 0xfee1dead, 672274793, 0x4321fedc, 0, 0, 0);
    for (;;)
        call(124, 0, 0, 0, 0, 0, 0);
}

__attribute__((naked, noreturn)) void _start(void)
{
    __asm__ volatile("bl guest_main\nb .");
}
