#!/usr/bin/env python3
"""用软件 TLB 执行真实 LKL 用户进程，记录 CLI/GCD 功能与内存结果。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
EXP = ROOT / 'experiments/native-linux'
WORK = ROOT / 'work/native-linux-nce'
KERNEL = WORK / 'processes'
DEFAULT_OUT = WORK / 'lkl-translation'


def run(command, **kwargs):
    return subprocess.run(list(map(str, command)), check=True, timeout=120, **kwargs)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate(rows, memory_interval, mapping="window", workload="core", tlb_invalidation=None, execution=None, direct_config=None, cpus=1, parallel_profile=None, uaccess_stop=None, code_lookup_limit=None, code_memory=None, online_cpus=None):
    if online_cpus is not None:
        if not 1 <= online_cpus <= cpus:
            return False
        if [row for row in rows if row.get('config') == 'translation_online_cpus'] != [
                {'config': 'translation_online_cpus', 'capacity': cpus, 'requested': online_cpus,
                 'online_mask': (1 << online_cpus) - 1}]:
            return False
    active_cpus = cpus if online_cpus is None else online_cpus
    if code_memory is not None:
        settings = [row for row in rows if row.get('config') == 'translation_code_memory']
        if settings != [{'config': 'translation_code_memory', 'kernel': 'dual' if code_memory == 'dual' else 'single',
                         'direct': code_memory if execution == 'direct' else 'unused'}]:
            return False
        if code_memory == 'dual' and [row for row in rows if row.get('test') == 'translation_prepared_code_execution'] != [
                {'test': 'translation_prepared_code_execution', 'passed': True}]:
            return False
    if parallel_profile is not None:
        settings = [row for row in rows if row.get('config') == 'translation_parallel']
        if len(settings) != 1 or settings[0] != {'config': 'translation_parallel', 'cpus': cpus,
                                               'profile': parallel_profile, 'uaccess_stop': uaccess_stop}:
            return False
        profiles = [row for row in rows if row.get('parallel_execution_profile')]
        if len(profiles) != int(parallel_profile):
            return False
        if profiles:
            record = profiles[0]
            keys = ('union_wall_ns', 'overlap_ns', 'thread_cpu_ns', 'cpu0_ns', 'cpu1_ns')
            if record.get('cpus') != cpus or any(type(record.get(key)) != int or record[key] < 0 for key in keys):
                return False
            if (not record['union_wall_ns'] or not record['thread_cpu_ns'] or
                    record['overlap_ns'] > record['union_wall_ns'] or
                    record['thread_cpu_ns'] != record['cpu0_ns'] + record['cpu1_ns'] or
                    (active_cpus == 1 and (record['overlap_ns'] or record['cpu1_ns']))):
                return False
    if cpus > 1:
        selected = [row for row in rows if row.get('config') == 'translation_execution']
        if len(selected) != cpus or {row.get('cpu') for row in selected} != set(range(cpus)):
            return False
        if workload == 'core' and active_cpus == 2 and not {'translated_smp_private_mm_compute', 'translated_smp_code_alias_updates', 'translated_smp_copy_unmap_protect'} <= {row.get('test') for row in rows if row.get('passed')}:
            return False
        if workload == 'core' and online_cpus is not None and [row for row in rows if row.get('test') == 'translated_smp_online_cpus'] != [
                {'test': 'translated_smp_online_cpus', 'passed': True, 'online': active_cpus}]:
            return False
        for cpu in range(active_cpus, cpus):
            idle = [row for row in rows if row.get('cpu') == cpu and row.get('direct_execution')]
            if len(idle) != 1 or any(idle[0][key] for key in ('calls', 'fallbacks', 'compiles', 'code_lookups', 'code_bytes')):
                return False
        if execution == 'direct':
            watch = [row for row in rows if row.get('direct_code_watch')]
            if len(watch) != cpus or {row.get('cpu') for row in watch} != set(range(cpus)):
                return False
            totals = [{key: value for key, value in row.items() if key != 'cpu'} for row in watch]
            if any(row != totals[0] for row in totals[1:]):
                return False
            if totals[0]['enabled'] and totals[0].get('scope') != 'process':
                return False
            code_lookup_limit = sum(row['code_lookups'] for row in rows if row.get('direct_execution'))
        return all(validate([row for row in rows if row.get('cpu', cpu) == cpu], memory_interval,
                            mapping, workload, tlb_invalidation, execution, direct_config,
                            code_lookup_limit=code_lookup_limit, code_memory=code_memory)
                   for cpu in range(active_cpus))
    expected = {'linux_dynamic_code', 'linux_signal_suite', 'linux_signal_register_restore',
                'linux_signal_eintr_restart', 'linux_signal_timer_preemption', 'linux_mm_exchange_integrity',
                'translated_pthread_tls_mutex', 'translated_signals_protection_restore',
                'translated_runtime_elf_exec', 'translated_runtime_libraries', 'translated_context_cleanup',
                'linux_ram_capacity_cow', 'linux_brk_growth_cow', 'linux_fault_contiguous_data_boundaries',
                'linux_exec_symlink', 'linux_aarch64_file_open_flags', 'linux_stack_growth_and_limit',
                'linux_android_elf_reservation_boundaries', 'linux_sparse_reservation_cow',
                'linux_mmap_byte_offset', 'linux_fault_register_state', 'linux_user_memory_boundaries',
                'linux_dynamic_link_dlopen_constructors', 'linux_dynamic_tls_threads', 'linux_dynamic_fork_signal'}
    if workload == 'android':
        expected = {'android_system_image_readonly', 'android_bionic_threads_allocator_tls',
                    'android_bionic_many_threads', 'android_scudo_class_growth', 'linux_proc_concurrent_exit',
                    'android_cfi_library_reload', 'android_apk_native_libraries', 'android_cfi_bounded_shadow',
                    'android_art_library_loaded', 'android_art_java_workload', 'android_art_jit_arithmetic',
                    'android_art_jit_objects', 'android_art_jit_calls', 'android_art_jit_allocations',
                    'android_art_jit_automatic', 'android_art_jit_runtime_features', 'android_art_jit_patterns',
                    'android_art_jit_strings_classes', 'android_art_jit_types', 'android_art_jit_intrinsics',
                    'android_art_proxy_references', 'android_art_vm_destroyed'}
    elif workload in ('services', 'desktop'):
        expected = {'android_system_image_readonly', 'android_data_ext4_mounted', 'android_proc_pid_namespace',
                    'linux_selinux_service_policy', 'android_platform_binder_cleanup', 'android_framework_runtime',
                    'android_vintf_build_consistency', 'android_aconfig_storage', 'android_framework_file_observer',
                    'android_system_suspend_wakelocks', 'android_installd_app_data', 'android_apex_registry',
                    'android_zygote_command_server', 'android_framework_graphics', 'android_crypto',
                    'linux_data_readonly_shutdown'}
        if workload == 'desktop':
            expected.remove('android_zygote_command_server')
            expected.update({'android_system_boot_completed', 'android_desktop_launch_commands',
                             'android_system_process_groups_stopped', 'android_gralloc_cpu_gpu_roundtrip',
                             'native_graphics_channels_released'})
            activities = {row.get('package') for row in rows
                          if row.get('test') == 'android_desktop_visible_activity' and row.get('passed')}
            if not {'com.android.launcher3', 'com.android.deskclock', 'com.google.android.documentsui'} <= activities:
                return False
            expected.update({'android_application_data_persistence', 'android_keystore_persistence',
                             'android_clock_controls_ready', 'android_clock_alarm_activity', 'android_home_navigation',
                             'android_preview_clock_data_preserved'})
    if not rows or rows[-1].get('result') != 'passed' or any(row.get('passed') is False for row in rows):
        return False
    if not expected <= {row.get('test') for row in rows if row.get('passed')}:
        return False
    cleanup = [row for row in rows if row.get('test') == 'translated_context_cleanup']
    if len(cleanup) != 1 or (mapping == 'pte' and (cleanup[0]['maps'] or cleanup[0]['revocations'])):
        return False
    if execution is not None:
        selected = [row for row in rows if row.get('config') == 'translation_execution']
        if len(selected) != 1 or selected[0].get('engine') != execution:
            return False
        if execution == 'direct':
            if direct_config is not None and any(selected[0].get(k) != v for k, v in direct_config.items()):
                return False
            direct = [row for row in rows if row.get('direct_execution')]
            if len(direct) != 1 or not direct[0]['calls'] or not direct[0]['compiles']:
                return False
            if direct_config is not None and (direct[0]['read_mapping_misses'] + direct[0]['write_mapping_misses'] != direct[0]['mapping_misses']):
                return False
            if direct_config is not None and (direct[0]['code_validation_hits'] > direct[0]['code_lookups'] or
                    (not direct_config['reuse_code_checks'] and direct[0]['code_validation_hits'])):
                return False
            if direct_config is not None and ((direct[0]['pte_read_refills'] > 0) != (direct_config.get('memory') == 'refill')):
                return False
            watch = [row for row in rows if row.get('direct_code_watch')]
            checks = [row for row in rows if row.get('direct_code_validation')]
            if len(watch) != 1 or len(checks) != 1 or watch[0]['enabled'] != selected[0]['code_watch']:
                return False
            if direct_config is not None and 'cross_page' in direct_config:
                crossing = [row for row in rows if row.get('direct_cross_page')]
                if len(crossing) != 1:
                    return False
                crossing = crossing[0]
                if crossing['exits'] != crossing['completed'] + crossing['fallbacks'] + crossing['faults']:
                    return False
                if not direct_config['cross_page'] and (crossing['completed'] or crossing['faults'] or crossing['bytes']):
                    return False
                if not crossing['completed'] <= crossing['bytes'] <= 64 * crossing['completed']:
                    return False
            if direct_config is not None and 'route_epochs' in direct_config:
                routes = [row for row in rows if row.get('direct_code_routes')]
                if len(routes) != 1 or not routes[0]['invalidations']:
                    return False
                cleared = routes[0]['cleared_entries']
                if direct_config['route_epochs']:
                    if cleared:
                        return False
                elif cleared != routes[0]['invalidations'] * direct_config['route_entries']:
                    return False
                if direct_config.get('retain_routes') is not None:
                    if bool(routes[0]['reused_runs']) != direct_config['retain_routes']:
                        return False
                if routes[0]['branch_exits'] > direct[0]['code_lookups']:
                    return False
            if direct_config is not None and 'memory_groups' in direct_config:
                groups = [row for row in rows if row.get('direct_memory_groups')]
                if len(groups) != 1 or bool(groups[0]['groups']) != direct_config['memory_groups']:
                    return False
                if not 2 * groups[0]['groups'] <= groups[0]['instructions'] <= 8 * groups[0]['groups']:
                    return False
            if checks[0]['content_checks'] + checks[0]['version_hits'] > direct[0]['code_lookups']:
                return False
            if watch[0]['enabled']:
                if not watch[0]['queries'] or not watch[0]['protections'] or not checks[0]['version_hits']:
                    return False
                if watch[0]['protection_failures'] or watch[0]['queries'] > (direct[0]['code_lookups'] if code_lookup_limit is None else code_lookup_limit):
                    return False
            elif checks[0]['version_hits'] or any(watch[0][key] for key in
                    ('queries', 'protections', 'write_faults', 'invalidations', 'protection_failures', 'excluded_pages')):
                return False
            flat = [row for row in rows if row.get('direct_flat_tables')]
            enabled = selected[0].get('memory') in ('flat', 'view')
            if len(flat) != 1 or flat[0]['enabled'] != enabled or flat[0]['live_virtual_bytes']:
                return False
            if enabled and (not flat[0]['updates'] or flat[0]['peak_virtual_bytes'] < 4194304):
                return False
            if not enabled and any(flat[0][key] for key in ('updates', 'refresh_pages', 'peak_virtual_bytes')):
                return False
            views = [row for row in rows if row.get('direct_read_views')]
            if views or selected[0].get('memory') == 'view':
                enabled = selected[0].get('memory') == 'view'
                if len(views) != 1 or views[0]['enabled'] != enabled or views[0]['live_virtual_bytes']:
                    return False
                view = views[0]
                if view['created'] != view['destroyed']:
                    return False
                if enabled and (not view['created'] or not view['compiled_reads'] or
                                view['peak_virtual_bytes'] < 4294967296 or
                                view['maps'] + view['revocations'] != view['read_changes']):
                    return False
                if not enabled and any(view[key] for key in ('created', 'maps', 'revocations', 'compiled_reads', 'read_faults', 'peak_virtual_bytes')):
                    return False
            if (direct[0]['code_bytes'] > direct[0]['capacity_bytes'] or
                    direct[0]['capacity_bytes'] != selected[0]['direct_cache_bytes'] or
                    direct[0]['fallbacks'] > direct[0]['calls']):
                return False
    settings = [row for row in rows if row.get('config') == 'translation_cache_settings']
    cache = [row for row in rows if row.get('translation_engine_cache')]
    if len(settings) != 1 or len(cache) != 1:
        return False
    requested, actual = settings[0]['cache_bytes'], settings[0].get('actual_cache_bytes', 0)
    if not 0 <= requested - actual < 65536 or actual != cache[0]['capacity_bytes'] or cache[0]['used_bytes'] > actual:
        return False
    tlb_settings = [row for row in rows if row.get('config') == 'translation_tlb_invalidation']
    tlb = [row for row in rows if 'translation_tlb' in row]
    if len(tlb_settings) != 1 or len(tlb) != 1:
        return False
    mode = tlb_settings[0]['mode']
    if (mode not in ('full', 'pages') or mode != tlb[0]['translation_tlb'] or
            (tlb_invalidation is not None and mode != tlb_invalidation) or
            (mode == 'pages' and mapping != 'pte') or
            (mode == 'full' and (tlb[0]['invalidated_pages'] or tlb[0]['inactive_skips']))):
        return False
    cases = [(row['bytes'], row['shared_percent'], row['touch_all'], row['threads'])
             for row in rows if row.get('bench') == 'linux_mm_exchange']
    expected_cases = {(size * 1024 * 1024, shared, touch, 0)
                      for size in (2, 4, 8, 16) for shared in (0, 50, 100) for touch in (0, 1)}
    expected_cases.update((size * 1024 * 1024, 0, 1, 1) for size in (2, 4, 8, 16))
    if workload == 'core' and (len(cases) != 28 or set(cases) != expected_cases):
        return False
    phases = ('kernel_setup', 'guest_setup', 'guest_execution', 'cleanup')
    if workload == 'android':
        phases = ('kernel_setup', 'guest_setup', 'bionic_execution', 'cfi_execution', 'art_execution', 'cleanup')
    elif workload == 'services':
        phases = ('kernel_setup', 'guest_setup', 'services_execution', 'cleanup')
    elif workload == 'desktop':
        phases = ('kernel_setup', 'guest_setup', 'desktop_execution', 'cleanup')
    summaries = [row for row in rows if 'memory_statistics' in row]
    if not memory_interval:
        return not summaries and not any('memory_sample' in row for row in rows)
    if [row['memory_statistics'] for row in summaries] != list(phases):
        return False
    for summary in summaries:
        samples = [row for row in rows if row.get('memory_sample') == summary['memory_statistics']]
        if summary['errors'] or len(samples) != summary['samples'] or len(samples) < 2:
            return False
        if [row['monotonic_ns'] for row in samples] != sorted(row['monotonic_ns'] for row in samples):
            return False
        for metric in ('footprint', 'resident'):
            values = [row[metric + '_bytes'] for row in samples]
            if (summary[metric + '_latest_bytes'] != values[-1] or
                summary[metric + '_sample_peak_bytes'] != max(values) or
                abs(summary[metric + '_mean_bytes'] - sum(values) / len(values)) > 0.001):
                return False
        if summary['resident_lifetime_peak_bytes'] != samples[-1]['resident_lifetime_peak_bytes']:
            return False
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--parallel-profile', type=int, choices=(0, 1), default=0)
    parser.add_argument('--cpus', type=int, choices=(1, 2), default=1)
    parser.add_argument('--online-cpus', type=int, choices=(1, 2), help='启动时启用的 CPU 数量，默认启用构建中的全部 CPU')
    parser.add_argument('--smp-uaccess-stop', choices=('all', 'code'), default='all')
    parser.add_argument('--code-memory', choices=('map-jit', 'dual'), default='map-jit')
    parser.add_argument('--execution', choices=('unicorn', 'direct'), default='unicorn')
    parser.add_argument('--direct-mapping-entries', type=int, choices=(256, 1024, 4096, 16384, 65536), default=256)
    parser.add_argument('--direct-route-entries', type=int, choices=(256, 1024, 4096, 16384, 65536), default=256)
    parser.add_argument('--direct-selective-writes', type=int, choices=(0, 1), default=0)
    parser.add_argument('--direct-reuse-code-checks', type=int, choices=(0, 1), default=0)
    parser.add_argument('--direct-code-watch', type=int, choices=(0, 1), default=0)
    parser.add_argument('--direct-cross-page', type=int, choices=(0, 1), default=0)
    parser.add_argument('--direct-memory-groups', type=int, choices=(0, 1), default=0)
    parser.add_argument('--direct-vector-table', type=int, choices=(0, 1), default=0)
    parser.add_argument('--direct-native-exclusive', type=int, choices=(0, 1), default=0)
    parser.add_argument('--direct-retain-routes', type=int, choices=(0, 1), default=0)
    parser.add_argument('--direct-route-epochs', type=int, choices=(0, 1), default=0)
    parser.add_argument('--direct-memory', choices=('cached', 'pte', 'refill', 'flat', 'view'), default='cached')
    parser.add_argument('--build-only', action='store_true')
    parser.add_argument('--reuse-engine-build', action='store_true', help='校验摘要后复用已构建的 Unicorn，用于执行器成组对照')
    parser.add_argument('--mapping', choices=('window', 'pte'), default='window')
    parser.add_argument('--data-seed', type=Path, help='服务和桌面检查使用的初始 ext4 数据盘')
    parser.add_argument('--persistent-data', action='store_true', help='各入口与重复轮次连续使用同一数据盘，检查重启持久化')
    parser.add_argument('--workload', choices=('core', 'android', 'services', 'desktop'), default='core')
    parser.add_argument('--entry', choices=('both', 'cli', 'gcd', 'window'), default='both')
    parser.add_argument('--art-nterp', type=int, choices=(0, 1), default=1)
    parser.add_argument('--art-expanded-jit', type=int, choices=(0, 1), default=0)
    parser.add_argument('--art-fast-baseline', type=int, choices=(0, 1), default=0)
    parser.add_argument('--cache-invalidation', choices=('full', 'pages'), default='pages')
    parser.add_argument('--exec-cache', choices=('full', 'pages'), default='full')
    parser.add_argument('--tlb-invalidation', choices=('full', 'pages'), default='full')
    parser.add_argument('--linux-fastpath', type=int, choices=(0, 1), default=0)
    parser.add_argument('--cache-mib', type=int, choices=(16, 64, 128), default=16)
    parser.add_argument('--repeat', type=int, default=1)
    parser.add_argument('--batch-instructions', type=int, default=50000)
    parser.add_argument('--preemption', choices=('instructions', 'timer'), default='instructions')
    parser.add_argument('--output', type=Path, default=DEFAULT_OUT)
    parser.add_argument('--memory-interval-ms', type=int, default=100)
    args = parser.parse_args()
    args.online_cpus = args.cpus if args.online_cpus is None else args.online_cpus
    if args.online_cpus > args.cpus or (args.mapping != 'pte' and args.online_cpus != args.cpus):
        parser.error('在线 CPU 数量不能超过构建容量，单 CPU 对照需要 PTE 翻译内核')
    if args.cpus == 2 and (args.mapping != 'pte' or args.execution != 'direct' or
                           args.direct_memory != 'flat' or not args.direct_native_exclusive or
                           args.direct_retain_routes or
                           args.exec_cache != 'pages' or args.preemption != 'timer'):
        parser.error('双核原型需要 direct/pte/flat、原生独占、pages exec-cache 和 timer；跳转复用需关闭')
    if args.cpus == 1 and args.smp_uaccess_stop != 'all':
        parser.error('用户复制的停止策略仅用于双核原型')
    entries = ('cli', 'gcd') if args.entry == 'both' else (args.entry,)
    if args.entry == 'window' and args.workload != 'desktop':
        parser.error('可见窗口用于 desktop 负载')
    if args.exec_cache == 'pages' and (args.mapping != 'pte' or args.cache_invalidation != 'pages'):
        parser.error('exec 按页失效需要 PTE 映射与按页代码失效')
    if args.tlb_invalidation == 'pages' and args.mapping != 'pte':
        parser.error('TLB 按页失效需要 PTE 映射')
    if args.execution == 'direct' and args.mapping != 'pte':
        parser.error('Direct 执行使用 PTE 映射')
    direct_config = {'mapping_entries': args.direct_mapping_entries, 'route_entries': args.direct_route_entries,
                     'selective_writes': bool(args.direct_selective_writes), 'reuse_code_checks': bool(args.direct_reuse_code_checks),
                     'memory': args.direct_memory, 'code_watch': bool(args.direct_code_watch), 'cross_page': bool(args.direct_cross_page),
                     'route_epochs': bool(args.direct_route_epochs), 'retain_routes': bool(args.direct_retain_routes), 'memory_groups': bool(args.direct_memory_groups), 'native_exclusive': bool(args.direct_native_exclusive), 'vector_table': bool(args.direct_vector_table)} if args.execution == 'direct' else None
    if args.execution != 'direct' and (args.direct_mapping_entries != 256 or args.direct_route_entries != 256 or args.direct_selective_writes or args.direct_reuse_code_checks or args.direct_code_watch or args.direct_cross_page or args.direct_route_epochs or args.direct_retain_routes or args.direct_memory_groups or args.direct_native_exclusive or args.direct_vector_table or args.direct_memory != 'cached'):
        parser.error('Direct 缓存参数需要 --execution direct')
    if args.direct_retain_routes and not (args.direct_route_epochs and args.direct_code_watch and args.direct_memory in ('pte', 'flat', 'view')):
        parser.error('跨调用复用需要跳转序号、代码写入跟踪与 PTE/flat 访存')
    if args.direct_memory in ('pte', 'flat', 'view') and (args.direct_mapping_entries != 256 or args.direct_selective_writes):
        parser.error('pte/flat 使用地址表访存，数据缓存参数必须保持默认值')
    out = args.output.resolve()
    if not out.is_relative_to(WORK):
        parser.error('实验产物必须放在本项目 work/native-linux-nce 下')
    if platform.system() != 'Darwin' or platform.machine() != 'arm64' or args.repeat < 1:
        parser.error('需要 Apple Silicon Mac，repeat 必须为正数')
    if not 1000 <= args.batch_instructions <= 1000000 or not (args.memory_interval_ms == 0 or 10 <= args.memory_interval_ms <= 10000):
        parser.error('指令批次范围 1000…1000000；内存采样间隔为 0 或 10…10000 ms')
    manifest = WORK / ('unicorn-tb-stop-dual/build.json' if args.code_memory == 'dual' else 'unicorn-tb-stop/build.json')
    if args.reuse_engine_build:
        dependency = json.loads(manifest.read_text())
        if dependency.get('version') != '2.1.4' or dependency.get('code_memory') != args.code_memory or not dependency.get('sha256'):
            parser.error('Unicorn 构建记录无效')
        for name, expected in dependency['sha256'].items():
            if sha(ROOT / name) != expected:
                parser.error('Unicorn 构建输入或产物已变化：' + name)
    else:
        from build_translation_engine import build as build_engine
        dependency = build_engine(args.code_memory)
    library = Path(dependency['path'])
    out.mkdir(parents=True, exist_ok=True)
    llvm = Path('/opt/homebrew/opt/llvm/bin')
    kernel = KERNEL
    if args.mapping == 'pte':
        from build_lkl_translation_kernel import build
        kernel = build(args.cpus)
    else:
        native_manifest = json.loads((kernel / 'lkl-summary.json').read_text())
        if sha(kernel / 'lkl-image.bin') != native_manifest['kernel_sha256'] or any(
            sha(kernel / name) != native_manifest['generated_sha256'][name] for name in ('lkl-image.h', 'lkl-host-ops.h')):
            parser.error('内核镜像或头文件与本机验证记录不一致，请先重新构建并验证单核内核')
    symbols = subprocess.check_output([llvm / 'llvm-nm', kernel / 'lkl.so'], text=True)
    elf = (kernel / 'lkl.so').read_bytes()
    image = (kernel / 'lkl-image.bin').read_bytes()
    phoff = struct.unpack_from('<Q', elf, 32)[0]
    phsize, phcount = struct.unpack_from('<HH', elf, 54)
    exports = {'TRANSLATION_COPY_USER': 'nce_copy_user'}
    if args.mapping == 'pte':
        exports['TRANSLATION_LOOKUP'] = 'nce_translate_user'
    header = '' if args.mapping == 'pte' else '#define TRANSLATION_LOOKUP 0UL\n'
    for constant, name in exports.items():
        match = re.search(r'^([0-9a-f]+) [tT] ' + name + '$', symbols, re.M)
        if not match:
            parser.error('内核缺少 ' + name)
        symbol = int(match[1], 16)
        matched = False
        for index in range(phcount):
            kind, flags, offset, address, _, size, _, _ = struct.unpack_from('<IIQQQQQQ', elf, phoff + index * phsize)
            if kind == 1 and flags & 1 and address <= symbol and symbol + 64 <= address + size:
                matched = elf[offset + symbol - address:offset + symbol - address + 64] == image[symbol:symbol + 64]
        if not matched:
            parser.error(name + ' 的 ELF 地址与内核镜像不一致')
        header += f'#define {constant} 0x{symbol:x}UL\n'
    (out / 'translation-symbols.h').write_text(header)
    inputs = [EXP / 'translation/kernel-image.c', EXP / 'translation/linux-elf.c', EXP / 'translation/linux-execution.c', EXP / 'translation/linux-runner.c', EXP / 'translation/linux-memory.c', EXP / 'lkl-host.c']
    commands = []
    direct_link, direct_files = [], []
    if args.execution == 'direct':
        from probe_direct_execution import build_engine as build_direct
        direct_objects, direct_files = build_direct(out / 'direct', args.code_memory)
        direct_link = ['-DNCE_DIRECT_EXECUTION', *direct_objects, '-lc++']
        inputs += [EXP / 'translation/code-watch.c', EXP / 'translation/read-view.c']
        direct_files.append(EXP / 'translation/code-watch.h')
        direct_files.append(EXP / 'translation/read-view.h')
    if args.code_memory == 'dual':
        inputs.append(EXP / 'translation/unicorn-code-memory.c')
        if args.execution != 'direct':
            inputs += [EXP / 'code-memory.c', EXP / 'execute-check.c']
    graphics_link, graphics_files = [], []
    if args.workload == 'desktop':
        gfx = ROOT / 'work/gfxstream-src'
        prefix = ROOT / 'work/gfxstream-build-macos-ios-api/prefix'
        angle = ROOT / 'work/angle-macos/prefix/lib'
        manifest_path = prefix.parent / 'manifest.json'
        graphics_manifest = json.loads(manifest_path.read_text())
        for name, expected in graphics_manifest['inputs'].items():
            if sha(ROOT / name) != expected:
                raise SystemExit('gfxstream 源码发生变化，需要重新构建：' + name)
        for name, expected in graphics_manifest['libraries'].items():
            if sha(Path(name)) != expected:
                raise SystemExit('gfxstream 库与构建记录不一致：' + name)
        command = ['xcrun', 'clang++', '-std=c++17', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
                   '-Wno-return-type-c-linkage', '-isystem', gfx / 'host/include',
                   '-isystem', gfx / 'host/features/include', '-isystem', gfx / 'common/base/include',
                   '-c', EXP / 'graphics/host-graphics.cpp', '-o', out / 'host-graphics.o']
        run(command)
        commands.append(list(map(str, command)))
        inputs += [EXP / 'input.c', EXP / 'session.c']
        graphics_link = ['-DNCE_GRAPHICS', out / 'host-graphics.o', '-L', prefix / 'lib', '-lgfxstream_backend', '-lc++',
                         '-Wl,-rpath,' + str(prefix / 'lib'), '-Wl,-rpath,' + str(angle)]
        graphics_files = [manifest_path, EXP / 'graphics/host-graphics.cpp', EXP / 'graphics/host-graphics.h',
                          EXP / 'input.h', EXP / 'session.h', prefix / 'lib/libgfxstream_backend.dylib',
                          angle / 'libANGLE-shared.dylib']
    build_entries = ('cli', 'gcd', 'window') if args.entry == 'window' else ('cli', 'gcd')
    for mode in build_entries:
        command = ['xcrun', 'clang', '-O2', '-g', '-DNCE_PROCESS', f'-DNCE_TRANSLATION_CPUS={args.cpus}', '-Wall', '-Wextra', '-Werror',
                   '-I', EXP, '-I', kernel, '-I', out, '-I', KERNEL / 'host-include', '-I', library / 'include',
                   *inputs, library / 'lib/libunicorn.a', *graphics_link, *[x for x in direct_link if x != '-lc++' or '-lc++' not in graphics_link], '-o', out / mode]
        if args.code_memory == 'dual':
            command += ['-DNCE_TRANSLATION_DUAL_MAP']
        if args.cpus == 2:
            command += ['-DNCE_SMP']
        if mode == 'gcd':
            command += ['-DTRANSLATION_DISPATCH', EXP / 'dispatch-test.c']
        elif mode == 'window':
            command += ['-DTRANSLATION_DISPATCH', EXP / 'graphics/window-test.mm', '-fobjc-arc',
                        '-framework', 'AppKit', '-framework', 'Metal', '-framework', 'QuartzCore']
            graphics_files.append(EXP / 'graphics/window-test.mm')
        run(command)
        commands.append(list(map(str, command)))
    # Guest files are built after both hosts and are never linked into them.
    hosts_before = {mode: sha(out / mode) for mode in build_entries}
    libc = WORK / 'musl-build/install'
    resource = Path(subprocess.check_output([llvm / 'clang', '-print-resource-dir'], text=True).strip()) / 'include'
    command = [llvm / 'clang', '--target=aarch64-linux-musl', '-mno-outline-atomics', '-fPIE', '-O2', '-g',
               '-Wall', '-Wextra', '-Werror', '-nostdinc', '-isystem', libc / 'include', '-isystem', resource,
               f'-DNCE_TRANSLATION_CPUS={args.cpus}', '-c', EXP / 'translation/linux-guest.c', '-o', out / 'guest.o']
    run(command)
    commands.append(list(map(str, command)))
    guest_objects = []
    guest_sources = ('signal-tests.c', 'signal-registers.S', 'code-tests.c', 'mm-bench.c', 'mm-tests.c', 'file-tests.c')
    for name in guest_sources:
        obj = out / (Path(name).stem + '.o')
        compile_command = [llvm / 'clang', '--target=aarch64-linux-musl', '-mno-outline-atomics', '-fPIE', '-O2', '-g',
                           '-Wall', '-Wextra', '-Werror', '-ffunction-sections', '-Dmain=unused_mm_main', '-nostdinc', '-isystem', libc / 'include', '-isystem', resource,
                           '-c', EXP / 'userspace' / name, '-o', obj]
        run(compile_command)
        commands.append(list(map(str, compile_command)))
        guest_objects.append(obj)
    if args.cpus == 2:
        obj = out / 'smp-guest.o'
        command = [llvm / 'clang', '--target=aarch64-linux-musl', '-mno-outline-atomics', '-fPIE', '-O2', '-g',
                   '-Wall', '-Wextra', '-Werror', '-nostdinc', '-isystem', libc / 'include', '-isystem', resource,
                   '-c', EXP / 'translation/smp-guest.c', '-o', obj]
        run(command)
        commands.append(list(map(str, command)))
        guest_objects.append(obj)
    command = [shutil.which('ld.lld'), '-pie', '--gc-sections', '--no-dynamic-linker', '-z', 'max-page-size=16384', '-z', 'common-page-size=16384',
               libc / 'lib/rcrt1.o', libc / 'lib/crti.o', out / 'guest.o', *guest_objects, libc / 'lib/libc.a',
               WORK / 'compiler-rt-build/libbuiltins.a', libc / 'lib/crtn.o', '-o', out / 'guest.elf']
    run(command)
    commands.append(list(map(str, command)))
    run([llvm / 'llvm-strip', '--strip-all', out / 'guest.elf', '-o', out / 'late.elf'])
    runtime_files = ('sigreturn.elf', 'dynamic.elf', 'libc.so', 'dependency.so', 'plugin.so', 'dash.elf')
    for name in runtime_files:
        shutil.copyfile(KERNEL / name, out / name)
    from build_nce_userspace import BIONIC_TEST_SOURCES, build_bionic_test
    commands.append(build_bionic_test(out))
    runtime_files += ('bionic-test.elf',)
    android_files = []
    if args.workload != 'core':
        for name, source in [('NceWorkload.dex', KERNEL / 'java-classes/classes.dex'),
                             ('ld.config.txt', EXP / 'art/ld.config.txt')]:
            shutil.copyfile(source, out / name)
            android_files.append(out / name)
        android_files.append(KERNEL / 'system.img')
        if args.workload in ('services', 'desktop'):
            for name in ('nce-policy.cil', 'property_info'):
                shutil.copyfile(KERNEL / name, out / name)
                android_files.append(out / name)
            (out / 'enabled').write_text('1')
            android_files.append(out / 'enabled')
    if hosts_before != {mode: sha(out / mode) for mode in hosts_before}:
        raise RuntimeError('客体构建后宿主文件发生变化')
    files = [*inputs, *direct_files, ROOT / 'scripts/build_nce_userspace.py',
             *[EXP / 'userspace' / name for name in BIONIC_TEST_SOURCES],
             EXP / 'translation/unicorn-code-memory.h', EXP / 'execute-check.h', EXP / 'translation/linux-elf.h', EXP / 'translation/linux-execution.h', EXP / 'translation/linux-memory.h', EXP / 'translation/linux-guest.c', Path(__file__),
             EXP / 'dispatch-test.c', EXP / 'layout.h', EXP / 'guest-registers.h',
             *[EXP / 'userspace' / name for name in guest_sources], *android_files, *graphics_files,
             kernel / 'lkl-image.bin', kernel / 'lkl-image.h', kernel / 'lkl.so', kernel / 'lkl-host-ops.h',
             EXP / 'translation/kernel-api.h', EXP / 'translation/kernel-image.h', EXP / 'code-memory.h', EXP / 'nce.h', out / 'translation-symbols.h',
             ROOT / 'scripts/build_translation_engine.py', manifest,
             library / 'lib/libunicorn.a', *[out / name for name in (*build_entries, 'guest.elf', 'late.elf', *runtime_files)]]
    if args.mapping == 'pte':
        files += [ROOT / 'scripts/build_lkl_translation_kernel.py', ROOT / 'scripts/lkl_translation_patch.py',
                  EXP / 'lkl-nce-mmu.c', kernel / 'build.json']
    if args.cpus == 2:
        files += [EXP / 'translation/smp-guest.c', ROOT / 'scripts/lkl_translation_smp_patch.py']
    data_seed = None
    if args.data_seed and args.workload not in ('services', 'desktop'):
        parser.error('--data-seed 仅用于服务和桌面检查')
    if args.persistent_data and (args.workload != 'desktop' or args.repeat * len(entries) < 2):
        parser.error('--persistent-data 需要至少两轮桌面运行')
    if args.workload in ('services', 'desktop'):
        data_seed = (args.data_seed or KERNEL / 'android-data-seed.img').resolve()
        if not data_seed.is_relative_to(WORK) or not data_seed.is_file():
            parser.error('初始数据盘必须是 work/native-linux-nce 中的文件')
        files.append(data_seed)
    input_hashes = {str(path.relative_to(ROOT)): sha(path) for path in files}
    if args.build_only:
        print(out)
        return
    environment = os.environ.copy()
    environment['NCE_TRANSLATION_FILES'] = str(out)
    environment['NCE_TRANSLATION_ONLINE_CPUS'] = str(args.online_cpus)
    environment.update(NCE_ART_NTERP=str(args.art_nterp), NCE_ART_EXPANDED_JIT=str(args.art_expanded_jit),
                       NCE_ART_FAST_BASELINE=str(args.art_fast_baseline), NCE_ART_AOT='0', NCE_ART_BOOT_IMAGE='0', NCE_SCHED_SLICE_US='0')
    environment['NCE_TRANSLATION_BATCH'] = str(args.batch_instructions)
    environment['NCE_TRANSLATION_PARALLEL_PROFILE'] = str(args.parallel_profile)
    environment['NCE_TRANSLATION_UACCESS_STOP'] = args.smp_uaccess_stop
    environment['NCE_TRANSLATION_PREEMPTION'] = args.preemption
    environment['NCE_TRANSLATION_MEMORY_MS'] = str(args.memory_interval_ms)
    environment['NCE_TRANSLATION_WORKLOAD'] = args.workload
    environment['NCE_TRANSLATION_SYSTEM'] = str(KERNEL / 'system.img')
    environment['NCE_TRANSLATION_CACHE'] = args.cache_invalidation
    environment['NCE_TRANSLATION_EXEC_CACHE'] = args.exec_cache
    environment['NCE_TRANSLATION_TLB'] = args.tlb_invalidation
    environment['NCE_TRANSLATION_LINUX_FASTPATH'] = str(args.linux_fastpath)
    environment['NCE_TRANSLATION_CACHE_MIB'] = str(args.cache_mib)
    environment.update(NCE_DIRECT_MAPPING_ENTRIES=str(args.direct_mapping_entries),
                       NCE_DIRECT_ROUTE_ENTRIES=str(args.direct_route_entries), NCE_DIRECT_SELECTIVE_WRITES=str(args.direct_selective_writes),
                       NCE_DIRECT_REUSE_CODE_CHECKS=str(args.direct_reuse_code_checks), NCE_DIRECT_MEMORY=args.direct_memory,
                       NCE_DIRECT_CODE_WATCH=str(args.direct_code_watch), NCE_DIRECT_CROSS_PAGE=str(args.direct_cross_page),
                       NCE_DIRECT_ROUTE_EPOCHS=str(args.direct_route_epochs), NCE_DIRECT_RETAIN_ROUTES=str(args.direct_retain_routes), NCE_DIRECT_MEMORY_GROUPS=str(args.direct_memory_groups), NCE_DIRECT_NATIVE_EXCLUSIVE=str(args.direct_native_exclusive), NCE_DIRECT_VECTOR_TABLE=str(args.direct_vector_table))
    for name in ('NCE_DESKTOP', 'NCE_SYSTEM_SERVER_PROBE', 'NCE_DIAGNOSTICS', 'NCE_TRACE_SYSTEMUI'):
        environment.pop(name, None)
    if args.workload == 'desktop':
        environment.update(DYLD_LIBRARY_PATH=str(angle), ANGLE_DEFAULT_PLATFORM='metal')
    results = []
    for iteration in range(args.repeat):
        for mode in entries:
            stem = f'{mode}-{iteration}'
            data = None
            if args.workload in ('services', 'desktop'):
                data_path = out / ('persistent-data.img' if args.persistent_data else f'{stem}-data.img')
                if data_path.resolve() == data_seed:
                    parser.error('初始盘不能同时作为运行期间的数据盘')
                fresh = not args.persistent_data or not results
                if fresh:
                    run(['cp', '-c', data_seed, data_path])
                environment['NCE_TRANSLATION_DATA'] = str(data_path)
                data = {'path': str(data_path.relative_to(ROOT)), 'initial_sha256': sha(data_path), 'fresh': fresh}
                if not fresh and data['initial_sha256'] != results[-1]['data_image']['final_sha256']:
                    raise RuntimeError('连续运行的数据盘在两轮之间变化')
            with (out / f'{stem}.jsonl').open('w') as output, (out / f'{stem}.log').open('w') as error:
                try:
                    process = subprocess.run([out / mode, kernel / 'lkl-image.bin'], env=environment,
                                             stdout=output, stderr=error, timeout=480 if args.workload == 'desktop' else 120)
                    returncode = process.returncode
                except subprocess.TimeoutExpired:
                    returncode = 'timeout'
            rows = [json.loads(line) for line in (out / f'{stem}.jsonl').read_text().splitlines() if line.startswith('{')]
            passed = returncode == 0 and validate(rows, args.memory_interval_ms, args.mapping, args.workload, args.tlb_invalidation, args.execution, direct_config, args.cpus, bool(args.parallel_profile), args.smp_uaccess_stop, code_memory=args.code_memory, online_cpus=args.online_cpus)
            if data is not None:
                data['final_sha256'] = sha(data_path)
                metadata = subprocess.run(['/opt/homebrew/opt/e2fsprogs/sbin/dumpe2fs', '-h', data_path],
                                          capture_output=True, text=True, timeout=30)
                data['clean_shutdown'] = (metadata.returncode == 0 and
                                          re.search(r'^Filesystem state:\s+clean$', metadata.stdout, re.M) is not None and
                                          'needs_recovery' not in metadata.stdout and 'orphan_present' not in metadata.stdout)
                passed = passed and data['clean_shutdown']
                if not data['fresh']:
                    persisted = {row.get('test') for row in rows if row.get('passed') and row.get('restored')}
                    passed = passed and {'android_application_data_persistence', 'android_keystore_persistence'} <= persisted
            results.append({'entry': mode, 'iteration': iteration, 'returncode': returncode, 'results': rows,
                            'validated': passed, 'data_image': data,
                            'stdout_sha256': sha(out / f'{stem}.jsonl'), 'stderr_sha256': sha(out / f'{stem}.log')})
            if not passed:
                break
        if not results[-1]['validated']:
            break
    if input_hashes != {str(path.relative_to(ROOT)): sha(path) for path in files}:
        raise SystemExit('本轮验证期间源码或二进制发生变化，请重跑')
    report = {'scope': f'Native {args.cpus}-CPU LKL with translated ARM64 userspace; selected workload only; iPhone unverified',
              'data_seed': {'path': str(data_seed.relative_to(ROOT)), 'sha256': sha(data_seed)} if data_seed else None,
              'platform': platform.platform(), 'cpu': subprocess.check_output(['sysctl', '-n', 'machdep.cpu.brand_string'], text=True).strip(),
              'system_build': subprocess.check_output(['sw_vers', '-buildVersion'], text=True).strip(),
              'dependency': dependency, 'commands': commands, 'host_unchanged_after_guest_build': True,
              'configuration': {'code_memory': args.code_memory, 'execution': args.execution, 'direct': direct_config, 'mapping': args.mapping, 'sparse_scan': True, 'fault_prefetch': False, 'guest_page_bytes': 16384,
                                'workload': args.workload, 'entries': entries, 'art_nterp': args.art_nterp,
                                'cpus': args.cpus, 'online_cpus': args.online_cpus, 'uaccess_stop': args.smp_uaccess_stop, 'parallel_profile': bool(args.parallel_profile), 'art_expanded_jit': args.art_expanded_jit, 'art_fast_baseline': args.art_fast_baseline,
                                'cache_invalidation': args.cache_invalidation,
                                'exec_cache': args.exec_cache,
                                'tlb_invalidation': args.tlb_invalidation,
                                'linux_fastpath': args.linux_fastpath,
                                'guest_ram_bytes': 1073741824, 'translation_cache_bytes': args.cache_mib * 1024 * 1024,
                                'instruction_batch': args.batch_instructions, 'memory_interval_ms': args.memory_interval_ms,
                                'preemption': args.preemption, 'timer_interval_ns': 2000000},
              'measurement': 'Guest clock times 128 pipe round trips per memory case; excludes initial COW materialization. Selected preemption, translation and selected Linux mapping implementation included. Memory means are arithmetic sample means, including phase boundaries; sampled peaks can miss short peaks. Resident lifetime peak is separate.',
              'sha256': input_hashes, 'runs': results}
    (out / 'summary.json').write_text(json.dumps(report, indent=2) + '\n')
    print(out / 'summary.json')
    if len(results) != args.repeat * len(entries) or any(not row['validated'] for row in results):
        raise SystemExit('Linux 翻译原型失败，查看对应 jsonl/log')


if __name__ == '__main__':
    main()
