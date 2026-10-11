/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct NceExecutionProfile NceExecutionProfile;
typedef struct {
    uint64_t wall, cpu;
} NceProfileStamp;
enum { NCE_STAGE_PREPARE,
       NCE_STAGE_DIRECT,
       NCE_STAGE_UNICORN,
       NCE_STAGE_COUNT };
bool nce_execution_profile_enabled(void);
NceExecutionProfile *nce_execution_profile_thread(long pid, long tid);
NceProfileStamp nce_execution_profile_begin(void);
/* instruction labels the first instruction of the entire Unicorn interval. */
void nce_execution_profile_end(NceExecutionProfile *thread, unsigned stage, NceProfileStamp start, uint32_t instruction);
/* Called outside guest execution, only while the session callback is registered. */
int nce_execution_profile_control(bool enabled, const char *directory);
