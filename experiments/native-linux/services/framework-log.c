/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <android/log.h>

/* app_process starts through exec, so configure its logger in that process. */
__attribute__((constructor)) static void framework_logger(void)
{
    __android_log_set_logger(__android_log_stderr_logger);
}
