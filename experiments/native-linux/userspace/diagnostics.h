/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NCE_DIAGNOSTICS_H
#define NCE_DIAGNOSTICS_H
#include <unistd.h>

// CLI regressions run diagnostics; interactive apps opt in before boot.
static inline bool nce_diagnostics()
{
    return access("/data/nce/desktop-session", F_OK) != 0 || access("/data/nce/diagnostics", F_OK) == 0;
}
#endif
