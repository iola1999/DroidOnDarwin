#!/usr/bin/env python3
"""为 ARM64 翻译原型构建支持翻译块边界抢占的 Unicorn 2.1.4。"""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / 'work/native-linux-nce'
ARCHIVE_SHA = 'ea8863f095a0136388694e5a6063afd9bb7650e30243dd6251af59c5ce5601f4'


def build():
    archive = WORK / 'unicorn-2.1.4.tar.gz'
    source = WORK / 'unicorn-2.1.4'
    output = WORK / 'unicorn-tb-stop'
    prefix = output / 'prefix'
    output.mkdir(parents=True, exist_ok=True)
    if not archive.exists():
        urllib.request.urlretrieve('https://codeload.github.com/unicorn-engine/unicorn/tar.gz/refs/tags/2.1.4', archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != ARCHIVE_SHA:
        raise RuntimeError('Unicorn 源码压缩包摘要不一致')
    with tarfile.open(archive) as tar:
        if not source.exists():
            tar.extractall(WORK, filter='data')

        patched = {}

        def edit(name, before, after):
            original = patched.get(name)
            if original is None:
                original = tar.extractfile('unicorn-2.1.4/' + name).read().decode()
            if original.count(before) != 1:
                raise RuntimeError('Unicorn source mismatch: ' + name)
            patched[name] = original.replace(before, after)

        edit('include/uc_priv.h', '    bool stop_request;', '    bool nce_stop_at_tb;\n    bool nce_linux_memory;\n    bool stop_request;')
        edit('uc.c', 'UNICORN_EXPORT\nuc_err uc_emu_stop(uc_engine *uc)', '''/* The host serializes requests against engine close. Only the execution
 * thread consumes this flag and changes CPU exit state, at a TB boundary.
 * Synchronous callbacks continue to use uc_emu_stop. */
UNICORN_EXPORT
uc_err nce_uc_emu_stop_at_tb(uc_engine *uc)
{
    if (!uc || !uc->cpu) {
        return UC_ERR_HANDLE;
    }
    __atomic_store_n(&uc->nce_stop_at_tb, true, __ATOMIC_RELEASE);
    return UC_ERR_OK;
}

UNICORN_EXPORT
uc_err uc_emu_stop(uc_engine *uc)''')
        edit('uc.c', 'UNICORN_EXPORT\nuc_err uc_emu_stop(uc_engine *uc)', '''/* This private ARM64 probe maps one RAM block at physical address zero.
 * The engine must be stopped; offsets identify that RAM block directly. */
extern void tb_invalidate_phys_range_aarch64(struct uc_struct *, uint64_t, uint64_t);
UNICORN_EXPORT
uc_err nce_uc_invalidate_ram(uc_engine *uc, uint64_t start, uint64_t length)
{
    if (!uc || uc->arch != UC_ARCH_ARM64 || !length || start + length < start) {
        return UC_ERR_ARG;
    }
    UC_INIT(uc);
    tb_invalidate_phys_range_aarch64(uc, start, start + length);
    restore_jit_state(uc);
    return UC_ERR_OK;
}

/* The Linux adapter stops on failed TLB fills through cpu_loop_exit_restore.
 * It has no physical-memory hooks or asynchronous instruction-local stop. */
UNICORN_EXPORT
uc_err nce_uc_set_linux_memory(uc_engine *uc, bool enabled)
{
    if (!uc || uc->arch != UC_ARCH_ARM64 || uc->nested_level) {
        return UC_ERR_ARG;
    }
    UC_INIT(uc);
    uc->tb_flush(uc);
    uc->nce_linux_memory = enabled;
    restore_jit_state(uc);
    return UC_ERR_OK;
}

UNICORN_EXPORT
uc_err uc_emu_stop(uc_engine *uc)''')
        edit('qemu/tcg/tcg-op.c', '    if (tcg_ctx->uc->no_exit_request) {',
             '    if (tcg_ctx->uc->no_exit_request || tcg_ctx->uc->nce_linux_memory) {')
        edit('qemu/accel/tcg/translate-all.c', 'void tb_flush(CPUState *cpu)', '''/* Read existing counters only while the host has stopped the engine. */
void nce_uc_cache_stats_aarch64(struct uc_struct *uc, uint64_t values[5])
{
    TCGContext *context = uc->tcg_ctx;
    values[0] = context->tb_ctx.tb_flush_count;
    values[1] = context->tb_phys_invalidate_count;
    values[2] = tcg_nb_tbs(context);
    values[3] = tcg_code_size(context);
    values[4] = context->code_gen_buffer_size;
}

void tb_flush(CPUState *cpu)''')
        edit('uc.c', '    uc->stop_request = false;',
             '    __atomic_store_n(&uc->nce_stop_at_tb, false, __ATOMIC_RELEASE);\n    uc->stop_request = false;')
        edit('uc.c', '    uc->stop_request = true;',
             '    __atomic_store_n(&uc->nce_stop_at_tb, false, __ATOMIC_RELEASE);\n    uc->stop_request = true;')
        edit('qemu/accel/tcg/tcg-runtime.c', '    if (cpu_loop_exit_requested(uc->cpu) && !in_delay_slot) {',
             '''    /* Bit 1 marks the TB entry; bit 0 retains the delay-slot rule. */
    if ((in_delay_slot & 2) &&
        __atomic_load_n(&uc->nce_stop_at_tb, __ATOMIC_ACQUIRE) &&
        __atomic_exchange_n(&uc->nce_stop_at_tb, false, __ATOMIC_ACQ_REL)) {
        uc->stop_request = true;
        cpu_exit(uc->cpu);
    }
    if (cpu_loop_exit_requested(uc->cpu) && !(in_delay_slot & 1)) {''')
        edit('qemu/include/exec/gen-icount.h',
             '    TCGv_ptr puc = tcg_const_ptr(tcg_ctx, tcg_ctx->uc);\n    TCGv_i32 tmp = tcg_const_i32(tcg_ctx, 0);',
             '''    if (tcg_ctx->uc->nce_linux_memory) {
        /* ARM64 has no delay slots. At TB entry no instruction-local TCG
         * temporary crosses this branch. Call the helper only on a request. */
        TCGLabel *ready = gen_new_label(tcg_ctx);
        TCGv_ptr address = tcg_const_ptr(tcg_ctx, tcg_ctx->uc);
        TCGv_i32 pending = tcg_temp_new_i32(tcg_ctx);
        TCGv_i32 requested = tcg_temp_new_i32(tcg_ctx);
        tcg_gen_ld8u_i32(tcg_ctx, pending, address, offsetof(struct uc_struct, nce_stop_at_tb));
        tcg_temp_free_ptr(tcg_ctx, address);
        address = tcg_const_ptr(tcg_ctx, &cpu_neg(tcg_ctx->uc->cpu)->icount_decr.u32);
        tcg_gen_ld_i32(tcg_ctx, requested, address, 0);
        tcg_gen_sari_i32(tcg_ctx, requested, requested, 31);
        tcg_gen_or_i32(tcg_ctx, pending, pending, requested);
        tcg_gen_brcondi_i32(tcg_ctx, TCG_COND_EQ, pending, 0, ready);
        tcg_temp_free_i32(tcg_ctx, pending);
        tcg_temp_free_i32(tcg_ctx, requested);
        tcg_temp_free_ptr(tcg_ctx, address);
        TCGv_ptr callback = tcg_const_ptr(tcg_ctx, tcg_ctx->uc);
        TCGv_i32 flags = tcg_const_i32(tcg_ctx, 2);
        gen_helper_check_exit_request(tcg_ctx, callback, flags);
        tcg_temp_free_i32(tcg_ctx, flags);
        tcg_temp_free_ptr(tcg_ctx, callback);
        gen_set_label(tcg_ctx, ready);
        return;
    }
    TCGv_ptr puc = tcg_const_ptr(tcg_ctx, tcg_ctx->uc);
    TCGv_i32 tmp = tcg_const_i32(tcg_ctx, 2);''')
    for name, content in patched.items():
        path = source / name
        if path.read_text() != content:
            path.write_text(content)
    commands = [
        ['cmake', '-S', source, '-B', output / 'build', '-DCMAKE_BUILD_TYPE=Release', '-DUNICORN_ARCH=aarch64',
         '-DBUILD_SHARED_LIBS=OFF', '-DUNICORN_BUILD_TESTS=OFF', '-DUNICORN_INSTALL=ON', f'-DCMAKE_INSTALL_PREFIX={prefix}'],
        ['cmake', '--build', output / 'build', '-j8'], ['cmake', '--install', output / 'build']]
    env = os.environ.copy()
    env['ZERO_AR_DATE'] = '1'
    with (output / 'build.log').open('w') as log:
        for command in commands:
            subprocess.run(list(map(str, command)), env=env, check=True, stdout=log, stderr=subprocess.STDOUT, timeout=600)
    files = [Path(__file__), archive, source / 'include/uc_priv.h', source / 'uc.c',
             source / 'qemu/accel/tcg/tcg-runtime.c', source / 'qemu/include/exec/gen-icount.h',
             source / 'qemu/tcg/tcg-op.c',
             source / 'qemu/accel/tcg/translate-all.c',
             prefix / 'lib/libunicorn.a', prefix / 'include/unicorn/unicorn.h']
    manifest = {'version': '2.1.4', 'patch': 'nce-tb-stop', 'path': str(prefix), 'ZERO_AR_DATE': '1',
                'commands': [list(map(str, command)) for command in commands],
                'sha256': {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest() for path in files}}
    (output / 'build.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return manifest


if __name__ == '__main__':
    print(build()['path'])
