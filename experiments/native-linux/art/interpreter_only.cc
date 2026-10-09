/* SPDX-License-Identifier: Apache-2.0 */
#include "jit_create.h"
#include <stdio.h>
#include <stdlib.h>

namespace art::jit {
JitCompilerInterface* jit_create()
{
    fputs("NCE ART: this build requires -Xint; compiled reference accesses are pending\n", stderr);
    abort();
}
}
