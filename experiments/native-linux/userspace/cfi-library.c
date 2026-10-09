/* SPDX-License-Identifier: GPL-2.0-or-later */
__attribute__((visibility("default"))) int nce_cfi_value(int value)
{
    return value + NCE_CFI_INCREMENT;
}
