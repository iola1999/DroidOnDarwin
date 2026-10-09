/* SPDX-License-Identifier: GPL-2.0-or-later */
PHDRS { code PT_LOAD FILEHDR PHDRS FLAGS(5); }
SECTIONS {
    . = SIZEOF_HEADERS;
    .text : { *(.text*) } :code
    .rodata : { *(.rodata*) } :code
    .dynsym : { *(.dynsym) } :code
    .dynstr : { *(.dynstr) } :code
    .hash : { *(.hash) } :code
    .gnu.hash : { *(.gnu.hash) } :code
    .dynamic : { *(.dynamic) } :code
}
