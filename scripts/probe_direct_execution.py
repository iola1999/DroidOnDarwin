#!/usr/bin/env python3
"""构建并比较运行时 ARM64 代码块执行器，保留独立功能检查与阶段内存。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / 'work/native-linux-nce'
SOURCE = ROOT / 'experiments/native-linux/direct'
OAKNUT = '94c726ce0338b054eb8cb5ea91de8fe6c19f4392'
ARCHIVE_SHA = 'c404db82002f9a851a412b3d55ac9be8a42a2e319fd2fc8a73e63e6ef5d4f97b'


def build_engine(out, code_memory='map-jit'):
    archive = WORK / 'oaknut-94c726c.tar.gz'
    if not archive.exists():
        urllib.request.urlretrieve(f'https://codeload.github.com/merryhime/oaknut/tar.gz/{OAKNUT}', archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != ARCHIVE_SHA:
        raise RuntimeError('Oaknut 源码摘要不一致')
    headers = WORK / f'oaknut-{OAKNUT}'
    if not headers.exists():
        with tarfile.open(archive) as tar:
            tar.extractall(WORK, filter='data')
    out.mkdir(parents=True, exist_ok=True)
    inputs = [SOURCE / name for name in ('engine.cpp', 'compile.cpp', 'decode.cpp', 'memory-groups.cpp', 'read-faults.cpp', 'cross-page.cpp', 'entry.S')]
    flags = ['-DNCE_DIRECT_DUAL_MAP'] if code_memory == 'dual' else []
    if flags:
        inputs.append(SOURCE.parent / 'code-memory.c')
    objects = []
    for path in inputs:
        obj = out / (path.stem + '.o')
        command = ['xcrun', 'clang++' if path.suffix == '.cpp' else 'clang']
        if path.suffix == '.cpp':
            command += ['-std=c++20']
        subprocess.run([*command, *flags, '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-I', str(headers / 'include'),
                        '-c', str(path), '-o', str(obj)], check=True, timeout=120)
        objects.append(obj)
    return objects, inputs + [SOURCE / name for name in ('decode.h', 'memory-groups.h', 'direct.h', 'internal.h', 'layout.h', 'page-table-layout.h')] + [SOURCE.parent / name for name in ('code-memory.h', 'nce.h', 'guest-registers.h')] + sorted((headers / 'include').rglob('*.hpp')) + [Path(__file__), archive]


def build(out, code_memory='map-jit'):
    objects, core_inputs = build_engine(out, code_memory)
    unicorn = WORK / ('unicorn-tb-stop/prefix' if code_memory == 'map-jit' else 'unicorn-tb-stop-dual/prefix')
    manifest_path = unicorn.parent / 'build.json'
    dependency = json.loads(manifest_path.read_text()) if manifest_path.exists() else {}
    valid = (dependency.get('version') == '2.1.4' and dependency.get('code_memory') == code_memory
             and dependency.get('sha256') and all((ROOT / name).is_file() and
                 hashlib.sha256((ROOT / name).read_bytes()).hexdigest() == expected
                 for name, expected in dependency['sha256'].items()))
    if not valid:
        from build_translation_engine import build as build_unicorn
        dependency = build_unicorn(code_memory)
        unicorn = Path(dependency['path'])
    core_inputs += [manifest_path, *[ROOT / name for name in dependency['sha256']]]
    out.mkdir(parents=True, exist_ok=True)
    files = [SOURCE / name for name in ('probe.cpp', 'tests.cpp', 'instruction-tests.cpp', 'cross-page-tests.cpp', 'route-tests.cpp', 'memory-group-tests.cpp', 'page-table-tests.cpp', 'parallel-tests.cpp', 'unicorn-hook-tests.cpp', 'benchmark.cpp', 'main.cpp')] + [
        ROOT / 'experiments/native-linux/translation/kernels.S',
        ROOT / 'experiments/native-linux/translation/read-view.c',
        ROOT / 'experiments/native-linux/translation/linux-memory.c']
    if code_memory == 'dual':
        files.append(ROOT / 'experiments/native-linux/translation/unicorn-code-memory.c')
    for index, path in enumerate(files):
        obj = out / f'{index}-{path.stem}.o'
        command = ['xcrun', 'clang++' if path.suffix == '.cpp' else 'clang']
        if path.suffix == '.cpp':
            command += ['-std=c++20']
        command += ['-O2', '-g', '-Wall', '-Wextra', '-Werror', '-I', str(unicorn / 'include'), '-c', str(path), '-o', str(obj)]
        subprocess.run(command, check=True, timeout=120)
        objects.append(obj)
    executable = out / 'probe'
    library = unicorn / 'lib/libunicorn.a'
    subprocess.run(['xcrun', 'clang++', *map(str, objects), str(library), '-o', str(executable)], check=True, timeout=120)
    inputs = files + core_inputs + [SOURCE / 'probe.h', ROOT / 'experiments/native-linux/translation/read-view.h', library, executable]
    hashes = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    return executable, hashes


def validate(rows, mode, interval, config=None):
    if not rows or rows[-1].get('result') != 'passed' or any(row.get('passed') is False for row in rows):
        return False
    if mode in ('tests', 'direct') and config is not None:
        selected = [r for r in rows if r.get('direct_config')]
        if len(selected) != 1 or any(selected[0].get(key) != value for key, value in config.items()):
            return False
    if mode == 'tests':
        expected = {
            'integer_gpr_sp_nzcv_65536_instructions', 'memory_all_gpr_930_combinations',
            'fp_simd_rounding_fpsr_32768_instructions', 'tls_x18_vectors',
            'write_permission_fault_and_post_index_retry', 'writable_physical_code_alias_rejected',
            'cross_page_and_read_permission_fault', 'changed_code_same_guest_address',
            'direct_and_indirect_self_branch_budget', 'cross_page_indirect_call_return',
            'changed_cross_page_branch_cache', 'pair_pre_post_index_all_base_registers',
            'exclusive_load_store_all_widths', 'asynchronous_stop_at_block_and_indirect_boundary',
            'stop_between_registration_and_entry', 'shared_code_guard_across_executors',
            'parallel_cross_page_and_remote_code_guard',
            '16k_data_page_internal_and_external_boundaries', 'bounded_cache_reuse_and_code_replacement',
            'dczid_zero_block_alignment_permission_and_prefetch', 'cached_write_alias_becomes_executable',
            'route_collision_reuse_and_next_run_fetch_permission', 'direct_pte_registers_flags_and_post_index',
            'direct_pte_read_permissions_directories_and_bounds', 'direct_pte_write_permissions_remap_and_retry',
            'direct_pte_code_alias_and_next_run_release', 'direct_pte_cross_page_permissions_retry_and_code_alias',
            'direct_pte_memory_groups_boundaries_remap_and_code_alias'}
        expected |= {name + suffix for name in expected if name.startswith('direct_pte_') for suffix in ('_refill', '_flat', '_view')}
        expected |= {'direct_flat_exclusive_pair_and_zva', 'direct_flat_cross_page_table_switch_and_configuration'}
        expected |= {'direct_read_view_handler_ownership_and_forwarding', 'direct_read_view_vector_literal_fault_and_retry'}
        expected |= {'parallel_private_same_va_compile_and_memory', 'parallel_native_exclusive_all_widths_and_orderings',
                     'parallel_tls_x18_vectors_and_replaced_code', 'parallel_targeted_stop',
                     'native_exclusive_host_return_and_remote_aba', 'native_exclusive_clrex_and_address_mismatch',
                     'native_exclusive_remote_aba_without_host_return', 'native_exclusive_interrupted_retry_fallback'}
        expected |= {'code_version_reuse_and_mutation', 'code_version_backing_permissions_and_fallback',
                     'code_version_owner_and_provider_changes'}
        expected |= {'literal_load_signed_vector_pc_and_permissions', 'simd_lane_replicate_writeback_and_faults',
                     'fp_fixed_rounding_and_widening', 'new_instruction_reserved_encodings', 'simd_table_alias_wrap_indices_and_setting'}
        expected |= {'cross_page_noncontiguous_memory_vectors_faults_12', 'cross_page_noncontiguous_memory_vectors_faults_14'}
        expected.add('unicorn_hook_state_cache_reset_and_independent_close')
        expected |= {'route_epochs_fetch_permission_and_code_replacement', 'route_epochs_flush_and_return_to_freed_page'}
        expected |= {'memory_groups_all_bases_widths_vectors_and_pairs', 'memory_groups_inner_entries_and_partial_page_faults',
                     'memory_groups_sp_alignment_and_code_write_alias', 'memory_groups_asynchronous_stop',
                     'memory_groups_fallback_base_changes_and_branch_targets'}
        expected |= {name + suffix for name in (
            'retained_routes_registers_and_cached_target', 'retained_routes_permissions_code_remap_and_flags',
            'retained_routes_untracked_alias_and_zero_epoch', 'retained_routes_cached_target_write_alias')
            for suffix in ('', '_flat', '_view')}
        if not expected <= {r.get('test') for r in rows if r.get('passed')}:
            return False
    else:
        samples = [r for r in rows if 'kernel' in r]
        if len(samples) != 9 or {(r['kernel'], r['round']) for r in samples} != {
                (kernel, iteration) for kernel in ('integer', 'memory', 'atomic') for iteration in range(3)}:
            return False
        if any(r.get('engine') != mode or r['wall_ns'] <= 0 or r['thread_cpu_ns'] <= 0 for r in samples):
            return False
    summaries = [r for r in rows if 'memory_statistics' in r]
    memory = [r for r in rows if 'memory_sample' in r]
    if not interval:
        return not summaries and not memory
    phases = ['kernel_setup', 'boundaries', 'parallel_execution'] if mode == 'tests' else ['kernel_setup', 'integer', 'memory', 'atomic', 'cleanup']
    if [r['memory_statistics'] for r in summaries] != phases:
        return False
    for summary in summaries:
        samples = [r for r in memory if r['memory_sample'] == summary['memory_statistics']]
        if summary['errors'] or len(samples) != summary['samples'] or len(samples) < 2:
            return False
        for metric in ('footprint', 'resident'):
            values = [r[metric + '_bytes'] for r in samples]
            if (summary[metric + '_latest_bytes'] != values[-1] or
                    summary[metric + '_sample_peak_bytes'] != max(values) or
                    abs(summary[metric + '_mean_bytes'] - sum(values) / len(values)) > 0.001):
                return False
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=WORK / 'direct-execution')
    parser.add_argument('--mode', choices=['tests', 'bench', 'all'], default='all')
    parser.add_argument('--memory-interval-ms', type=int, default=100)
    parser.add_argument('--build-only', action='store_true')
    parser.add_argument('--code-memory', choices=('map-jit', 'dual'), default='map-jit')
    parser.add_argument('--mapping-entries', type=int, choices=(256, 1024, 4096, 16384, 65536), default=256)
    parser.add_argument('--route-entries', type=int, choices=(256, 1024, 4096, 16384, 65536), default=256)
    parser.add_argument('--selective-writes', type=int, choices=(0, 1), default=0)
    parser.add_argument('--reuse-code-checks', type=int, choices=(0, 1), default=0)
    parser.add_argument('--route-epochs', type=int, choices=(0, 1), default=0)
    parser.add_argument('--retain-routes', type=int, choices=(0, 1), default=0)
    parser.add_argument('--memory-groups', type=int, choices=(0, 1), default=0)
    parser.add_argument('--vector-table', type=int, choices=(0, 1), default=0)
    args = parser.parse_args()
    if args.retain_routes and not args.route_epochs:
        parser.error('跨调用复用需要开启跳转序号')
    if platform.system() != 'Darwin' or platform.machine() != 'arm64':
        parser.error('该原型需要 Apple Silicon Mac')
    if not 0 <= args.memory_interval_ms <= 60000:
        parser.error('内存采样间隔须为 0..60000 ms，0 关闭')
    out = args.output.resolve()
    if not out.is_relative_to(WORK):
        parser.error('实验产物必须放在本项目 work/native-linux-nce 下')
    executable, hashes = build(out, args.code_memory)
    if args.build_only:
        print(executable)
        return
    runs = []
    config = {'mapping_entries': args.mapping_entries, 'route_entries': args.route_entries,
              'selective_writes': bool(args.selective_writes), 'reuse_code_checks': bool(args.reuse_code_checks),
              'route_epochs': bool(args.route_epochs), 'retain_routes': bool(args.retain_routes), 'memory_groups': bool(args.memory_groups), 'vector_table': bool(args.vector_table)}
    environment = dict(os.environ, NCE_DIRECT_MAPPING_ENTRIES=str(args.mapping_entries),
                       NCE_DIRECT_ROUTE_ENTRIES=str(args.route_entries), NCE_DIRECT_SELECTIVE_WRITES=str(args.selective_writes),
                       NCE_DIRECT_REUSE_CODE_CHECKS=str(args.reuse_code_checks), NCE_DIRECT_ROUTE_EPOCHS=str(args.route_epochs), NCE_DIRECT_RETAIN_ROUTES=str(args.retain_routes), NCE_DIRECT_MEMORY_GROUPS=str(args.memory_groups), NCE_DIRECT_VECTOR_TABLE=str(args.vector_table))
    modes = (['tests'] if args.mode != 'bench' else []) + (['native', 'unicorn', 'direct'] if args.mode != 'tests' else [])
    for entry in ('cli', 'gcd'):
        for mode in (modes if entry == 'cli' else list(reversed(modes))):
            path = out / f'{entry}-{mode}.jsonl'
            command = [str(executable), mode, entry, str(args.memory_interval_ms)]
            with path.open('w') as stdout, path.with_suffix('.stderr.log').open('w') as stderr:
                result = subprocess.run(command, env=environment, stdout=stdout, stderr=stderr, timeout=180)
            rows = [json.loads(line) for line in path.read_text().splitlines()]
            if [r['direct_code_memory'] for r in rows if 'direct_code_memory' in r] != [args.code_memory]:
                raise RuntimeError('Direct 执行内存变体与请求不一致')
            runs.append({'entry': entry, 'mode': mode, 'command': command, 'returncode': result.returncode, 'results': rows})
            if result.returncode or not validate(rows, mode, args.memory_interval_ms, config):
                (out / 'failed.json').write_text(json.dumps(runs, indent=2) + '\n')
                raise SystemExit(f'检查失败：{path}')
    for name, digest in hashes.items():
        if hashlib.sha256((ROOT / name).read_bytes()).hexdigest() != digest:
            raise RuntimeError(f'运行期间输入发生变化：{name}')
    report = {'scope': 'Independent runtime ARM64 blocks; no Linux/Android/iPhone integration',
              'platform': platform.platform(), 'cpu': subprocess.check_output(['sysctl', '-n', 'machdep.cpu.brand_string'], text=True).strip(),
              'system_build': subprocess.check_output(['sw_vers', '-buildVersion'], text=True).strip(),
              'measurement': 'Three warm rounds per engine in separate processes, reversed process order for GCD. Same ARM64 bytes and checked results. Direct includes one-million-instruction slice entries; Unicorn includes register exchange. Memory phases include initialization and result verification, with arithmetic sample mean, sampled peak and latest; lifetime RSS peak is separate.',
              'build_flags': ['-std=c++20', '-O2', '-g', '-Wall', '-Wextra', '-Werror'] + (['-DNCE_DIRECT_DUAL_MAP'] if args.code_memory == 'dual' else []),
              'direct_code_memory': args.code_memory, 'unicorn_code_memory': args.code_memory,
              'configuration': config,
              'dependency': {'oaknut_revision': OAKNUT, 'oaknut_archive_sha256': ARCHIVE_SHA},
              'sha256': hashes, 'runs': runs, 'comparison': {}}
    for entry in ('cli', 'gcd'):
        report['comparison'][entry] = {}
        for kernel in ('integer', 'memory', 'atomic'):
            values = {}
            for run in runs:
                samples = [r['wall_ns'] for r in run['results'] if r.get('kernel') == kernel]
                if run['entry'] == entry and samples:
                    values[run['mode']] = statistics.median(samples)
            report['comparison'][entry][kernel] = values
    (out / 'summary.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report['comparison'], indent=2))
    print(out / 'summary.json')


if __name__ == '__main__':
    main()
