/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../../experiments/native-linux/translation/linux-execution.c"
#include <signal.h>

static void rejected(int signal_number)
{
    (void)signal_number;
    _Exit(86);
}

int main(int argc, char **argv)
{
    if (argc != 5)
        return 2;
    signal(SIGABRT, rejected);
    physical_base = reservation_base = 0x1000000000UL;
    reservation_size = NCE_RAM_SIZE + NCE_VMALLOC_SIZE;
    ram = (void *)0x2000000000UL;
    host_alias_request(argv[1], strtoul(argv[2], NULL, 0), strtoul(argv[3], NULL, 0), strtoul(argv[4], NULL, 0));
    return 0;
}
