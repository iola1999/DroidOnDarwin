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
from apk_fixture_validation import inputs_unchanged as apk_inputs_unchanged, read_fixture, unobserved_creations, validate_results as validate_apk_results

ROOT = Path(__file__).resolve().parents[1]
EXP = ROOT / 'experiments/native-linux'
WORK = ROOT / 'work/native-linux-nce'
KERNEL = WORK / 'processes'
DEFAULT_OUT = WORK / 'lkl-translation'


def run(command, **kwargs):
    return subprocess.run(list(map(str, command)), check=True, timeout=120, **kwargs)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate_android_image(root, directory):
    """拒绝使用尚未包含当前用户态源码的 Android 系统镜像。"""
    manifest = json.loads((directory / 'lkl-summary.json').read_text())
    prefix = 'experiments/native-linux/userspace/'
    fixed = {'scripts/build_nce_userspace.py', 'experiments/native-linux/input.h',
             'experiments/native-linux/interaction-profile.h', 'experiments/native-linux/services/NceApkInstall.java'}
    expected = manifest['input_sha256']
    current = {str(p.relative_to(root)) for p in (root / prefix).glob('*') if p.is_file()}
    names = current | fixed | {name for name in expected if name.startswith(prefix)}
    stale = sorted(name for name in names if not (root / name).is_file() or
                   expected.get(name) != sha(root / name))
    if manifest.get('generated_sha256', {}).get('system.img') != sha(directory / 'system.img'):
        stale.append('system.img')
    if stale:
        raise ValueError('Android 系统镜像与当前用户态源码不一致，请先通过 scripts/probe_lkl.py 重建并验证：' + ', '.join(stale))


def validate_guest_layout(rows, requested, flat=False, guest_page_bytes=16384, *, sparse_end=1 << 36):
    records = [row for row in rows if row.get('config') == 'translation_guest_layout']
    if len(records) != 1 or requested not in ('host', 'low', 'sparse') or guest_page_bytes not in (4096, 16384) or type(sparse_end) is not int or sparse_end not in (1 << 36, 1 << 39):
        return False
    row = records[0]
    fields = ('guest_base', 'guest_end', 'guest_span_bytes', 'flat_coverage_bytes',
              'physical_base', 'ram_base', 'ram_bytes', 'reservation_base', 'reservation_bytes')
    if (set(row) != {'config', 'mode', *fields} or row['mode'] != requested or
            any(type(row.get(key)) != int or not 0 <= row[key] < 1 << 64 for key in fields)):
        return False
    base, end, reserve = row['guest_base'], row['guest_end'], row['reservation_base']
    physical, ram = row['physical_base'], row['ram_base']
    ram_size, reserve_size = row['ram_bytes'], row['reservation_bytes']
    coverage = 1 << 32
    if (not base or end <= base or row['guest_span_bytes'] != end - base or
            row['flat_coverage_bytes'] != (coverage if flat else 0) or ram_size != 1 << 30 or
            not ram or any(row[key] % 16384 for key in fields) or
            not reserve or reserve >= 1 << (39 if guest_page_bytes == 4096 else 40) or
            reserve_size > (1 << (39 if guest_page_bytes == 4096 else 40)) - reserve or
            ram_size > (1 << 64) - ram or (requested == 'host' and ram < end and base < ram + ram_size) or
            (ram < reserve + reserve_size and reserve < ram + ram_size)):
        return False
    if requested in ('low', 'sparse'):
        return ((flat or guest_page_bytes == 4096) and base == 0x10000 and end == (sparse_end if requested == 'sparse' else coverage) and physical == reserve and
                reserve_size == ram_size + (64 << 20))
    return (base == reserve and end == base + coverage and physical == end and
            reserve_size == coverage + ram_size + (64 << 20))


def validate_standard_jit(rows, data_tbi=False, *, guest_page_bytes=16384):
    if [row for row in rows if row.get('config') == 'linux_standard_jit'] != [
            {'config': 'linux_standard_jit', 'page_bytes': guest_page_bytes}]:
        return False
    values = {'anon_rw_rx': 42, 'anon_rewrite': 91, 'mremap_move': 91,
              'anon_rx_map': 1, 'anon_rwx': 74, 'ordinary_file_rx': 57,
              'readonly_shared_write': 13, 'private_cow_parent': 41, 'private_file_cow': 41,
              'memfd_alias_updates': 16, 'memfd_pwrite': 97, 'memfd_shared_fork': 111,
              'memfd_mremap_clone': 111, 'unicorn_alias_update': 201,
              'unicorn_pwrite_update': 202, 'unicorn_unseen_pwrite': 203,
              'same_va_new_backing': 87, 'sealed_write': 1, 'unicorn_self_page_flush': 67,
              'exec_only_vma': 58, 'noexec_mmap': 1, 'noexec_mprotect': 13, 'unpopulated_exec_alias': 59}
    if data_tbi:
        values['unicorn_tagged_pwrite_updates'] = 3
    faults = {'rx_write': 2, 'remove_execute': 2, 'mremap_old_unmapped': 1, 'unmapped_execute': 1}
    records = [row for row in rows if row.get('test') == 'linux_standard_jit']
    if len(records) != len(values) + len(faults) or {row.get('case') for row in records} != values.keys() | faults.keys():
        return False
    for row in records:
        name = row['case']
        if row.get('passed') is not True:
            return False
        if name in values:
            if set(row) != {'test', 'case', 'passed', 'observed'} or type(row['observed']) != int or row['observed'] != values[name]:
                return False
        else:
            if (set(row) != {'test', 'case', 'passed', 'signal', 'si_code', 'address', 'expected_address'} or
                    any(type(row[key]) != int for key in ('signal', 'si_code', 'address', 'expected_address')) or
                    row['signal'] != 11 or row['si_code'] != faults[name] or
                    not 0 < row['address'] < 1 << 64 or row['address'] % 4 or row['address'] != row['expected_address']):
                return False
    return True


def validate(rows, memory_interval, mapping="window", workload="core", tlb_invalidation=None, execution=None, direct_config=None, cpus=1, parallel_profile=None, uaccess_stop=None, code_lookup_limit=None, code_memory=None, online_cpus=None, guest_layout=None, guest_page_bytes=16384, data_tbi=None, stock_art_jit_mode="default", *, sparse_end=1 << 36):
    if guest_page_bytes not in (4096, 16384):
        return False
    if guest_page_bytes == 4096:
        if mapping != 'pte' or execution not in ('unicorn', 'direct') or cpus != 1 or guest_layout not in ('low', 'sparse') or (execution == 'direct' and (not direct_config or direct_config['memory'] not in ('pte', 'flat'))):
            return False
        if [row for row in rows if row.get('config') == 'translation_page_sizes'] != [
                {'config': 'translation_page_sizes', 'guest_page_bytes': 4096,
                 'host_page_bytes': 16384, 'host_address_limit': 1 << 39}]:
            return False
        layouts = [row for row in rows if row.get('config') == 'translation_guest_layout']
        if len(layouts) != 1 or not validate_guest_layout(rows, guest_layout, execution == 'direct' and direct_config['memory'] == 'flat', guest_page_bytes, sparse_end=sparse_end):
            return False
        layout = layouts[0]
        physical = layout['physical_base']
        end = layout['reservation_base'] + layout['reservation_bytes']
        ram = layout['ram_base']
        requests = [row for row in rows if 'host_alias_request' in row]
        if not requests:
            return False
        for row in requests:
            if (set(row) != {'host_alias_request', 'target', 'source', 'bytes', 'accepted'} or
                    row['host_alias_request'] not in ('map', 'revoke', 'reclaim') or row['accepted'] is not True or
                    any(type(row[key]) != int or not 0 <= row[key] < 1 << 64 or row[key] % 16384
                        for key in ('target', 'source', 'bytes'))):
                return False
            target, source, size = row['target'], row['source'], row['bytes']
            if not size or not physical <= target <= end or size > end - target:
                return False
            if row['host_alias_request'] == 'revoke':
                if source:
                    return False
            elif not ram <= source <= ram + (1 << 30) or size > ram + (1 << 30) - source:
                return False
            if row['host_alias_request'] == 'reclaim' and target - physical != source - ram:
                return False
        if requests[0] != {'host_alias_request': 'map', 'target': physical, 'source': ram,
                            'bytes': 1 << 30, 'accepted': True}:
            return False
    if data_tbi is not None:
        tbi_config = [row for row in rows if row.get('config') == 'translation_data_tbi']
        if (tbi_config != [{'config': 'translation_data_tbi', 'data': data_tbi, 'fetch': False, 'tagged_syscall_abi': False}] or
                any(type(row[key]) is not bool for row in tbi_config for key in ('data', 'fetch', 'tagged_syscall_abi')) or
                (data_tbi and (cpus != 1 or (direct_config and direct_config['memory'] == 'view')))):
            return False
        tbi_stats = [row for row in rows if row.get('translation_data_tbi')]
        if (len(tbi_stats) != cpus or {row.get('cpu') for row in tbi_stats} != set(range(cpus)) or
                any(type(row.get(key)) is not int or row[key] < 0 or (not data_tbi and row[key] != 0)
                    for row in tbi_stats for key in ('unicorn_tagged_fills', 'alias_flushes'))):
            return False
    if guest_layout is not None:
        flat = execution == 'direct' and direct_config is not None and direct_config['memory'] in ('flat', 'view')
        if not validate_guest_layout(rows, guest_layout, flat, guest_page_bytes, sparse_end=sparse_end):
            return False
        if guest_layout in ('low', 'sparse') and (mapping != 'pte' or cpus != 1 or
                (guest_page_bytes == 16384 and (execution != 'direct' or direct_config['memory'] != 'flat')) or
                (guest_page_bytes == 4096 and execution == 'direct' and direct_config['memory'] not in ('pte', 'flat'))):
            return False
        if workload == 'core' and mapping == 'pte':
            if data_tbi:
                for name in ('translated_data_tbi_and_unicorn_alias_invalidation', 'translated_tagged_syscall_abi_rejected', 'translated_tagged_fetch_rejected'):
                    if [row for row in rows if row.get('test') == name] != [{'test': name, 'passed': True}]:
                        return False
                tbi = [row for row in rows if row.get('translation_data_tbi')]
                if (len(tbi) != cpus or any(type(row.get(key)) is not int or row[key] <= 0
                                           for row in tbi for key in ('unicorn_tagged_fills', 'alias_flushes'))):
                    return False
                if execution == 'direct' and not {0xf8400949, 0xf8000949} <= {row.get('direct_fallback') for row in rows if row.get('count', 0) > 0}:
                    return False
            if guest_layout == 'sparse':
                if not {'translated_sparse_host_pointer_sources', 'translated_sparse_pte_cow_shared_uaccess',
                        'translated_sparse_reservation_materialization', 'translated_same_host_va_cow_shared'} <= {row.get('test') for row in rows if row.get('passed')}:
                    return False
                pointers = [row for row in rows if row.get('config') == 'translation_host_pointer_sources']
                if (len(pointers) != 1 or set(pointers[0]) != {'config', 'low', 'high', 'stack', 'text'} or
                        any(type(pointers[0][k]) is not int or not 0 < pointers[0][k] < 1 << 64 for k in ('low', 'high', 'stack', 'text')) or
                        not 0x10000 <= pointers[0]['low'] <= sparse_end - guest_page_bytes or pointers[0]['high'] < sparse_end):
                    return False
            if [row for row in rows if row.get('test') == 'translated_guest_layout_boundaries'] != [
                    {'test': 'translated_guest_layout_boundaries', 'passed': True, 'layout': guest_layout}]:
                return False
            if [row for row in rows if row.get('test') == 'translated_host_pointer_copy'] != [
                    {'test': 'translated_host_pointer_copy', 'passed': True}]:
                return False
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
    expected = {'linux_simd_fallback_continuation', 'linux_dynamic_code', 'linux_signal_suite', 'linux_signal_register_restore',
                'linux_signal_context_redirect',
                'linux_signal_eintr_restart', 'linux_signal_timer_preemption', 'linux_mm_exchange_integrity',
                'translated_pthread_tls_mutex', 'translated_signals_protection_restore', 'translated_timer_boundaries',
                'translated_runtime_elf_exec', 'translated_runtime_libraries', 'translated_context_cleanup',
                'linux_ram_capacity_cow', 'linux_brk_growth_cow', 'linux_fault_contiguous_data_boundaries',
                'linux_exec_symlink', 'linux_aarch64_file_open_flags', 'linux_stack_growth_and_limit',
                'linux_android_elf_reservation_boundaries', 'linux_sparse_reservation_cow',
                'linux_mmap_byte_offset', 'linux_fault_register_state', 'linux_user_memory_boundaries',
                'linux_dynamic_link_dlopen_constructors', 'linux_dynamic_tls_threads', 'linux_dynamic_fork_signal'}
    if workload == 'core' and mapping == 'pte':
        expected.remove('linux_dynamic_code')
        if not validate_standard_jit(rows, data_tbi=data_tbi, guest_page_bytes=guest_page_bytes):
            return False
    elif workload == 'stock-art':
        from stock_art import validate as validate_stock_art
        if not validate_stock_art(rows, guest_page_bytes=guest_page_bytes, jit_mode=stock_art_jit_mode):
            return False
        expected = {'stock_art_standard_jni', 'translated_context_cleanup'}
    elif workload == 'bionic':
        from stock_bionic import validate as validate_stock
        if not validate_stock(rows, guest_page_bytes):
            return False
        expected = {'stock_bionic_observation_complete', 'translated_context_cleanup'}
    elif workload == 'android':
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
            if selected[0].get('data_page_bytes') != guest_page_bytes:
                return False
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
    parser.add_argument('--high-res-timers', action='store_true', help='使用独立的高分辨率定时器内核变体')
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
    parser.add_argument('--direct-simd-math', type=int, choices=(0, 1), default=0)
    parser.add_argument('--direct-vector-table', type=int, choices=(0, 1), default=0)
    parser.add_argument('--direct-native-exclusive', type=int, choices=(0, 1), default=0)
    parser.add_argument('--direct-retain-routes', type=int, choices=(0, 1), default=0)
    parser.add_argument('--direct-route-epochs', type=int, choices=(0, 1), default=0)
    parser.add_argument('--direct-memory', choices=('cached', 'pte', 'refill', 'flat', 'view'), default='cached')
    parser.add_argument('--build-only', action='store_true')
    parser.add_argument('--reuse-engine-build', action='store_true', help='校验摘要后复用已构建的 Unicorn，用于执行器成组对照')
    parser.add_argument('--mapping', choices=('window', 'pte'), default='window')
    parser.add_argument('--data-tbi', type=int, choices=(0, 1), default=0)
    parser.add_argument('--guest-layout', choices=('host', 'low', 'sparse'), default='host')
    parser.add_argument('--guest-page-bytes', type=int, choices=(4096, 16384), default=16384)
    parser.add_argument('--data-seed', type=Path, help='服务和桌面检查使用的初始 ext4 数据盘')
    parser.add_argument('--persistent-data', action='store_true', help='各入口与重复轮次连续使用同一数据盘，检查重启持久化')
    parser.add_argument('--apk-fixture-dir', type=Path, help='运行时传入同签名 Java/JNI APK，首轮安装更新，后续启动只检查已安装 v2')
    parser.add_argument('--workload', choices=('core', 'bionic', 'stock-art', 'android', 'services', 'desktop'), default='core')
    parser.add_argument('--stock-art-directory', type=Path, help='prepare_stock_art.py 已核对的原始 ART 组件目录')
    parser.add_argument('--system-image-directory', type=Path, help='prepare_stock_android.py 已核对的原始运行库系统盘目录')
    parser.add_argument('--runtime-source', choices=('sdk36', 'hyperos'))
    parser.add_argument('--runtime-directory', type=Path)
    parser.add_argument('--runtime-manifest', type=Path)
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
    parser.add_argument('--stock-art-observe-jit', type=int, choices=(0, 1), default=0)
    parser.add_argument('--stock-art-jit-mode', choices=('default', 'first-use', 'background'), default='default')
    parser.add_argument('--preemption', choices=('instructions', 'timer'), default='instructions')
    parser.add_argument('--output', type=Path, default=DEFAULT_OUT)
    parser.add_argument('--memory-interval-ms', type=int, default=100)
    args = parser.parse_args()
    if args.stock_art_jit_mode != 'default' and args.workload != 'stock-art':
        parser.error('JIT 诊断模式仅用于 stock-art')
    if args.stock_art_observe_jit and (args.workload != 'stock-art' or args.execution != 'direct' or args.cpus != 1 or args.guest_page_bytes != 16384):
        parser.error('JIT 执行观测需要单 CPU、16 KiB 的 Direct stock-art')
    if args.guest_page_bytes == 4096 and (args.mapping != 'pte' or args.execution not in ('unicorn', 'direct') or
            (args.execution == 'direct' and args.direct_memory not in ('pte', 'flat')) or
            args.guest_layout not in ('low', 'sparse') or args.cpus != 1 or args.workload not in ('core', 'bionic', 'stock-art')):
        parser.error('4 KiB 实验需要单 CPU 的 low或sparse/pte，Direct 使用 pte/flat 访存，负载仅限 core/bionic/stock-art')
    if args.guest_page_bytes == 16384 and args.guest_layout in ('low', 'sparse') and (args.mapping != 'pte' or args.execution != 'direct' or args.direct_memory != 'flat' or args.cpus != 1):
        parser.error('低客体地址实验需要单 CPU 的 pte/direct/flat')
    if args.data_tbi and args.execution == 'direct' and args.direct_memory == 'view':
        parser.error('用户数据 TBI 暂不支持 read-view')
    if args.data_tbi and args.cpus != 1:
        parser.error('用户数据 TBI 当前仅验证单 CPU')
    if args.high_res_timers and args.mapping != 'pte':
        parser.error('高分辨率定时器变体需要 --mapping pte')
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
    apk_fixture = None
    if args.apk_fixture_dir:
        if (args.execution != 'direct' or args.workload != 'desktop' or not args.persistent_data
                or args.repeat * len(entries) < 2 or args.build_only):
            parser.error('APK 检查需要 Direct desktop、persistent-data 和至少两轮实际启动')
        directory = args.apk_fixture_dir.resolve()
        if not directory.is_relative_to(WORK):
            parser.error('APK fixture 目录需要位于 work/native-linux-nce 中')
        apk_fixture = read_fixture(directory)
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
                     'route_epochs': bool(args.direct_route_epochs), 'retain_routes': bool(args.direct_retain_routes), 'memory_groups': bool(args.direct_memory_groups), 'native_exclusive': bool(args.direct_native_exclusive), 'vector_table': bool(args.direct_vector_table), 'simd_math': bool(args.direct_simd_math)} if args.execution == 'direct' else None
    if args.execution != 'direct' and (args.direct_mapping_entries != 256 or args.direct_route_entries != 256 or args.direct_selective_writes or args.direct_reuse_code_checks or args.direct_code_watch or args.direct_cross_page or args.direct_route_epochs or args.direct_retain_routes or args.direct_memory_groups or args.direct_native_exclusive or args.direct_vector_table or args.direct_simd_math or args.direct_memory != 'cached'):
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
    system_image = KERNEL / 'system.img'
    system_candidate = None
    if args.system_image_directory:
        if (args.workload not in ('services', 'desktop') or args.mapping != 'pte' or
                args.execution != 'direct' or args.guest_layout != 'sparse' or
                not args.data_tbi or args.cpus != 1):
            parser.error('原始运行库系统盘需要单 CPU、sparse、TBI、Direct/PTE 的 services 或 desktop')
        args.system_image_directory = args.system_image_directory.resolve()
        if not args.system_image_directory.is_relative_to(WORK):
            parser.error('候选系统盘必须在 work/native-linux-nce 内')
        from prepare_stock_android import verify as verify_system_candidate
        try:
            system_candidate = verify_system_candidate(args.system_image_directory)
        except (OSError, ValueError, KeyError) as error:
            parser.error(str(error))
        if Path(system_candidate['base_image']['path']) != system_image:
            parser.error('候选系统盘必须使用当前探针的基准镜像')
        system_image = args.system_image_directory / 'system.img'
    if args.workload in ('android', 'services', 'desktop'):
        try:
            validate_android_image(ROOT, KERNEL)
        except (OSError, ValueError, KeyError) as error:
            parser.error(str(error))
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
    stock_art = None
    if args.workload == 'stock-art':
        if not args.stock_art_directory or args.mapping != 'pte' or args.cpus != 1 or args.entry == 'window':
            parser.error('stock-art 需要已准备目录和单核 PTE CLI/GCD')
        from stock_art import attach
        stock_art = attach(args.stock_art_directory, out, guest_page_bytes=args.guest_page_bytes)
        (out / 'stock-art-jit-mode').write_text(args.stock_art_jit_mode + '\n')
    elif args.stock_art_directory:
        parser.error('--stock-art-directory 仅用于 stock-art')
    stock_runtime = None
    if args.workload == 'bionic':
        if (not all((args.runtime_source, args.runtime_directory, args.runtime_manifest)) or
                args.mapping != 'pte' or args.cpus != 1 or args.entry == 'window'):
            parser.error('bionic 需要原始 runtime 的来源、目录与清单，以及单核 PTE CLI/GCD')
        from stock_bionic import prepare as prepare_stock
        stock_runtime = prepare_stock(args.runtime_source, args.runtime_directory, args.runtime_manifest, out)
    elif any((args.runtime_source, args.runtime_directory, args.runtime_manifest)):
        parser.error('runtime 输入仅用于 bionic 组件检查')
    llvm = Path('/opt/homebrew/opt/llvm/bin')
    kernel = KERNEL
    if args.mapping == 'pte':
        from build_lkl_translation_kernel import build
        kernel = build(args.cpus, args.high_res_timers, args.guest_page_bytes)
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
    inputs = [EXP / 'translation/kernel-image.c', EXP / 'translation/linux-elf.c', EXP / 'translation/linux-execution.c', EXP / 'translation/interaction-profile.c', EXP / 'translation/linux-runner.c', EXP / 'translation/linux-memory.c', EXP / 'lkl-host.c']
    commands = []
    page_flag = f'-DNCE_GUEST_PAGE_SHIFT={args.guest_page_bytes.bit_length() - 1}'
    direct_link, direct_files = [], []
    if args.execution == 'direct':
        from probe_direct_execution import build_engine as build_direct
        direct_objects, direct_files = build_direct(out / 'direct', args.code_memory, guest_page_bytes=args.guest_page_bytes)
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
        inputs += [EXP / 'input.c', EXP / 'session.c', EXP / 'session-profile.c']
        graphics_link = ['-DNCE_GRAPHICS', out / 'host-graphics.o', '-L', prefix / 'lib', '-lgfxstream_backend', '-lc++',
                         '-Wl,-rpath,' + str(prefix / 'lib'), '-Wl,-rpath,' + str(angle)]
        graphics_files = [manifest_path, EXP / 'graphics/host-graphics.cpp', EXP / 'graphics/host-graphics.h',
                          EXP / 'input.h', EXP / 'session.h', EXP / 'session-internal.h', EXP / 'interaction-profile.h', prefix / 'lib/libgfxstream_backend.dylib',
                          angle / 'libANGLE-shared.dylib']
    build_entries = ('cli', 'gcd', 'window') if args.entry == 'window' else ('cli', 'gcd')
    for mode in build_entries:
        command = ['xcrun', 'clang', '-O2', '-g', '-DNCE_PROCESS', f'-DNCE_TRANSLATION_CPUS={args.cpus}', page_flag, '-Wall', '-Wextra', '-Werror',
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
    guest_main = (EXP / 'stock-art/supervisor.c' if stock_art else
                  EXP / 'translation' / ('stock-bionic-supervisor.c' if stock_runtime else 'linux-guest.c'))
    command = [llvm / 'clang', '--target=aarch64-linux-musl', '-mno-outline-atomics', '-fPIE', '-O2', '-g',
               '-Wall', '-Wextra', '-Werror', '-nostdinc', '-isystem', libc / 'include', '-isystem', resource,
               f'-DNCE_TRANSLATION_CPUS={args.cpus}', f'-DNCE_TRANSLATION_HIGH_RES_TIMERS={int(args.high_res_timers)}', page_flag,
               '-c', guest_main, '-o', out / 'guest.o']
    if args.mapping == 'pte' and not stock_runtime and not stock_art:
        command.append('-DNCE_TRANSLATION_STANDARD_JIT')
    run(command)
    commands.append(list(map(str, command)))
    guest_objects = []
    guest_sources = [EXP / 'userspace' / name for name in
                     ('signal-tests.c', 'signal-registers.S', 'code-tests.c', 'mm-bench.c', 'mm-tests.c', 'file-tests.c')]
    guest_sources.append(EXP / 'translation/timer-tests.c')
    guest_sources.append(EXP / 'translation/sparse-guest.c')
    guest_sources.append(EXP / 'translation/tbi-guest.c')
    if stock_runtime or stock_art:
        guest_sources = []
    elif args.mapping == 'pte':
        guest_sources.append(EXP / 'translation/jit-guest.c')
    for source in guest_sources:
        obj = out / (source.stem + '.o')
        compile_command = [llvm / 'clang', '--target=aarch64-linux-musl', '-mno-outline-atomics', '-fPIE', '-O2', '-g',
                           '-Wall', '-Wextra', '-Werror', '-ffunction-sections', '-Dmain=unused_mm_main', page_flag, '-nostdinc', '-isystem', libc / 'include', '-isystem', resource,
                           '-c', source, '-o', obj]
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
    if stock_runtime:
        ndk = Path('/opt/homebrew/share/android-ndk')
        if 'Pkg.Revision = 29.0.14206865' not in (ndk / 'source.properties').read_text():
            raise RuntimeError('组件检查需要 NDK 29.0.14206865')
        command = [ndk / 'toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android35-clang',
                   '-O2', '-g', '-mno-outline-atomics', '-Wall', '-Wextra', '-Werror',
                   '-Wl,-z,max-page-size=16384,-z,common-page-size=16384',
                   page_flag, EXP / 'translation/stock-bionic-guest.c', '-ldl', '-o', out / 'bionic-test.elf']
        run(command)
        commands.append(list(map(str, command)))
    elif not stock_art:
        commands.append(build_bionic_test(out))
    if not stock_art:
        runtime_files += ('bionic-test.elf',)
    android_files = []
    if args.workload in ('android', 'services', 'desktop'):
        for name, source in [('NceWorkload.dex', KERNEL / 'java-classes/classes.dex'),
                             ('ld.config.txt', EXP / 'art/ld.config.txt')]:
            shutil.copyfile(source, out / name)
            android_files.append(out / name)
        android_files.extend([KERNEL / 'system.img', KERNEL / 'lkl-summary.json'])
        if system_candidate:
            android_files.extend([system_image, args.system_image_directory / 'manifest.json',
                                  ROOT / 'scripts/prepare_stock_android.py'])
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
             EXP / 'translation/unicorn-code-memory.h', EXP / 'execute-check.h', EXP / 'translation/linux-elf.h', EXP / 'translation/linux-execution.h', EXP / 'translation/interaction-profile.h', EXP / 'translation/linux-memory.h', guest_main, Path(__file__),
             EXP / 'dispatch-test.c', EXP / 'layout.h', EXP / 'guest-registers.h',
             *guest_sources, *android_files, *graphics_files,
             kernel / 'lkl-image.bin', kernel / 'lkl-image.h', kernel / 'lkl.so', kernel / 'lkl-host-ops.h',
             EXP / 'translation/kernel-api.h', EXP / 'translation/kernel-image.h', EXP / 'code-memory.h', EXP / 'nce.h', out / 'translation-symbols.h',
             ROOT / 'scripts/build_translation_engine.py', manifest,
             library / 'lib/libunicorn.a', *[out / name for name in (*build_entries, 'guest.elf', 'late.elf', *runtime_files)]]
    files += [EXP / 'translation/stock-art-runner.h', EXP / 'translation/execution-observation.h']
    if stock_runtime or stock_art:
        files.append(EXP / 'translation/component-supervisor.h')
    if stock_art:
        files += [ROOT / 'scripts/stock_art.py', ROOT / 'scripts/stock_art_jit.py', ROOT / 'scripts/prepare_stock_art.py',
                  out / 'stock-art-files.tsv', out / 'stock-art-boot-classpath', out / 'stock-art-jit-mode']
    if stock_runtime:
        from stock_bionic import FILES
        files += [ROOT / 'scripts/stock_bionic.py', EXP / 'translation/stock-bionic-guest.c',
                  out / 'stock-ld.config.txt', out / 'stock-runtime/source-manifest.json',
                  *[out / 'stock-runtime' / name for name in FILES]]
    if apk_fixture:
        files += [ROOT / 'scripts/apk_fixture_validation.py', ROOT / 'scripts/build_apk_fixture.py']
    if args.mapping == 'pte':
        files += [ROOT / 'scripts/build_lkl_translation_kernel.py', ROOT / 'scripts/lkl_translation_patch.py',
                  EXP / 'lkl-nce-mmu.c', kernel / 'build.json']
    if args.guest_page_bytes == 4096:
        files.append(ROOT / 'scripts/lkl_4k_vmalloc_patch.py')
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
    for name in ('NCE_APK_FIXTURE_DIR', 'NCE_APK_FIXTURE_STAGE'):
        environment.pop(name, None)
    if apk_fixture:
        environment['NCE_APK_FIXTURE_DIR'] = apk_fixture['directory']
    environment.pop('NCE_STOCK_ART_DIRECTORY', None)
    environment.pop('NCE_STOCK_ART_RESULT_DIR', None)
    if stock_art:
        environment['NCE_STOCK_ART_DIRECTORY'] = stock_art['directory']
    environment['NCE_TRANSLATION_FILES'] = str(out)
    environment['NCE_TRANSLATION_GUEST_LAYOUT'] = args.guest_layout
    environment['NCE_TRANSLATION_DATA_TBI'] = str(args.data_tbi)
    environment['NCE_STOCK_ART_OBSERVE_JIT'] = str(args.stock_art_observe_jit)
    environment['NCE_TRANSLATION_ONLINE_CPUS'] = str(args.online_cpus)
    environment.update(NCE_ART_NTERP=str(args.art_nterp), NCE_ART_EXPANDED_JIT=str(args.art_expanded_jit),
                       NCE_ART_FAST_BASELINE=str(args.art_fast_baseline), NCE_ART_AOT='0', NCE_ART_BOOT_IMAGE='0', NCE_SCHED_SLICE_US='0')
    environment['NCE_TRANSLATION_BATCH'] = str(args.batch_instructions)
    environment['NCE_TRANSLATION_PARALLEL_PROFILE'] = str(args.parallel_profile)
    environment['NCE_TRANSLATION_UACCESS_STOP'] = args.smp_uaccess_stop
    environment['NCE_TRANSLATION_PREEMPTION'] = args.preemption
    environment['NCE_TRANSLATION_MEMORY_MS'] = str(args.memory_interval_ms)
    environment['NCE_TRANSLATION_WORKLOAD'] = args.workload
    environment['NCE_TRANSLATION_SYSTEM'] = str(system_image)
    environment['NCE_TRANSLATION_CACHE'] = args.cache_invalidation
    environment['NCE_TRANSLATION_EXEC_CACHE'] = args.exec_cache
    environment['NCE_TRANSLATION_TLB'] = args.tlb_invalidation
    environment['NCE_TRANSLATION_LINUX_FASTPATH'] = str(args.linux_fastpath)
    environment['NCE_TRANSLATION_CACHE_MIB'] = str(args.cache_mib)
    environment.update(NCE_DIRECT_MAPPING_ENTRIES=str(args.direct_mapping_entries),
                       NCE_DIRECT_ROUTE_ENTRIES=str(args.direct_route_entries), NCE_DIRECT_SELECTIVE_WRITES=str(args.direct_selective_writes),
                       NCE_DIRECT_REUSE_CODE_CHECKS=str(args.direct_reuse_code_checks), NCE_DIRECT_MEMORY=args.direct_memory,
                       NCE_DIRECT_CODE_WATCH=str(args.direct_code_watch), NCE_DIRECT_CROSS_PAGE=str(args.direct_cross_page),
                       NCE_DIRECT_ROUTE_EPOCHS=str(args.direct_route_epochs), NCE_DIRECT_RETAIN_ROUTES=str(args.direct_retain_routes), NCE_DIRECT_MEMORY_GROUPS=str(args.direct_memory_groups), NCE_DIRECT_NATIVE_EXCLUSIVE=str(args.direct_native_exclusive), NCE_DIRECT_VECTOR_TABLE=str(args.direct_vector_table), NCE_DIRECT_SIMD_MATH=str(args.direct_simd_math))
    for name in ('NCE_DESKTOP', 'NCE_SYSTEM_SERVER_PROBE', 'NCE_DIAGNOSTICS', 'NCE_TRACE_SYSTEMUI'):
        environment.pop(name, None)
    if args.workload == 'desktop':
        environment.update(DYLD_LIBRARY_PATH=str(angle), ANGLE_DEFAULT_PLATFORM='metal')
    results = []
    for iteration in range(args.repeat):
        for mode in entries:
            stem = f'{mode}-{iteration}'
            if system_candidate and verify_system_candidate(args.system_image_directory) != system_candidate:
                raise RuntimeError('原始运行库系统盘在运行前发生变化')
            apk_stage = 'verify-restart' if results else 'install-update'
            if apk_fixture:
                if not apk_inputs_unchanged(apk_fixture):
                    raise RuntimeError('APK 输入在运行前发生变化')
                environment['NCE_APK_FIXTURE_STAGE'] = apk_stage
            if stock_art:
                from stock_art import unchanged as stock_art_unchanged
                if not stock_art_unchanged(stock_art):
                    raise RuntimeError('原始 ART 输入在运行前发生变化')
                component_output = out / (stem + '-art')
                component_output.mkdir(exist_ok=False)
                environment['NCE_STOCK_ART_RESULT_DIR'] = str(component_output)
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
            rows, parse_errors = [], []
            for line_number, line in enumerate((out / f'{stem}.jsonl').read_text().splitlines(), 1):
                if not line.startswith('{'):
                    continue
                try:
                    rows.append(json.loads(line))
                except json.JSONDecodeError as error:
                    if not stock_art:
                        raise
                    parse_errors.append({'line': line_number, 'error': str(error)})
            passed = not parse_errors and returncode == 0 and validate(rows, args.memory_interval_ms, args.mapping, args.workload, args.tlb_invalidation, args.execution, direct_config, args.cpus, bool(args.parallel_profile), args.smp_uaccess_stop, code_memory=args.code_memory, online_cpus=args.online_cpus, guest_layout=args.guest_layout, data_tbi=bool(args.data_tbi), guest_page_bytes=args.guest_page_bytes, stock_art_jit_mode=args.stock_art_jit_mode)
            apk_validated = None
            if apk_fixture:
                previous = [row for result in results for row in result['results']]
                apk_validated = apk_inputs_unchanged(apk_fixture) and validate_apk_results(rows, apk_stage, apk_fixture, previous)
                passed = passed and apk_validated
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
            if stock_art:
                from stock_art import artifacts, unchanged as stock_art_unchanged
                results[-1]['parse_errors'] = parse_errors
                results[-1]['stock_art_maps'] = artifacts(component_output, rows)
                results[-1]['stock_art_inputs_unchanged'] = stock_art_unchanged(stock_art)
                passed = passed and results[-1]['stock_art_inputs_unchanged'] and all(
                    item['validated'] and item['present'] for item in results[-1]['stock_art_maps'])
                if args.stock_art_observe_jit:
                    from stock_art_jit import observe
                    results[-1]['stock_art_jit_observation'] = observe(component_output, rows)
                    passed = passed and results[-1]['stock_art_jit_observation']['jit_execution_confirmed']
                results[-1]['validated'] = passed
            if apk_fixture:
                results[-1]['apk_fixture'] = {'stage': apk_stage, 'validated': apk_validated,
                                            'unobserved_creations': unobserved_creations(rows, previous) if apk_validated else None}
            if not passed:
                break
        if not results[-1]['validated']:
            break
    if input_hashes != {str(path.relative_to(ROOT)): sha(path) for path in files}:
        raise SystemExit('本轮验证期间源码或二进制发生变化，请重跑')
    if system_candidate and verify_system_candidate(args.system_image_directory) != system_candidate:
        raise RuntimeError('原始运行库系统盘在运行期间发生变化')
    report = {'scope': f'Native {args.cpus}-CPU LKL with translated ARM64 userspace; selected workload only; iPhone unverified',
              'data_seed': {'path': str(data_seed.relative_to(ROOT)), 'sha256': sha(data_seed)} if data_seed else None,
              'platform': platform.platform(), 'cpu': subprocess.check_output(['sysctl', '-n', 'machdep.cpu.brand_string'], text=True).strip(),
              'system_build': subprocess.check_output(['sw_vers', '-buildVersion'], text=True).strip(),
              'dependency': dependency, 'commands': commands, 'host_unchanged_after_guest_build': True,
              'configuration': {'data_tbi': bool(args.data_tbi), 'guest_layout': args.guest_layout, 'code_memory': args.code_memory, 'execution': args.execution, 'direct': direct_config, 'mapping': args.mapping, 'sparse_scan': True, 'fault_prefetch': False, 'guest_page_bytes': args.guest_page_bytes, 'host_page_bytes': 16384,
                                'workload': args.workload, 'entries': entries, 'art_nterp': args.art_nterp,
                                'cpus': args.cpus, 'high_res_timers': args.high_res_timers, 'online_cpus': args.online_cpus, 'uaccess_stop': args.smp_uaccess_stop, 'parallel_profile': bool(args.parallel_profile), 'art_expanded_jit': args.art_expanded_jit, 'art_fast_baseline': args.art_fast_baseline,
                                'cache_invalidation': args.cache_invalidation,
                                'exec_cache': args.exec_cache,
                                'tlb_invalidation': args.tlb_invalidation,
                                'linux_fastpath': args.linux_fastpath,
                                'guest_ram_bytes': 1073741824, 'translation_cache_bytes': args.cache_mib * 1024 * 1024,
                                'instruction_batch': args.batch_instructions, 'memory_interval_ms': args.memory_interval_ms,
                                'preemption': args.preemption, 'timer_interval_ns': 2000000},
              'measurement': 'Guest clock times 128 pipe round trips per memory case; excludes initial COW materialization. Selected preemption, translation and selected Linux mapping implementation included. Memory means are arithmetic sample means, including phase boundaries; sampled peaks can miss short peaks. Resident lifetime peak is separate.',
              'sha256': input_hashes, 'runs': results}
    if system_candidate:
        report['system_candidate'] = system_candidate
        report['measurement'] = 'Original SDK36 runtime service/desktop compatibility only; no performance comparison.'
    if stock_art:
        report['stock_art'] = stock_art
        report['stock_art']['execution_attempted'] = True
        report['stock_art']['observe_jit'] = bool(args.stock_art_observe_jit)
        report['stock_art']['jit_mode'] = args.stock_art_jit_mode
        report['measurement'] = 'Original ART component functionality; JIT execution requires completed-PC correlation; no performance comparison.'
    if stock_runtime:
        from stock_bionic import unchanged
        if not unchanged(stock_runtime):
            raise RuntimeError('原始 runtime 输入在运行期间发生变化')
        report['stock_runtime'] = stock_runtime
        report['measurement'] = 'Component compatibility observation only; concurrent builds allowed; no performance comparison.'
        for result in results:
            stages = [row for row in result['results'] if row.get('component_stage') == 'dynamic']
            result['component_compatible'] = result['validated'] and len(stages) == 1 and stages[0].get('success') is True
            result['component_stderr'] = (out / f"{result['entry']}-{result['iteration']}.log").read_text()
    if apk_fixture:
        report['apk_fixture'] = apk_fixture
        report['apk_fixture']['scope'] = 'Original signed Java/JNI fixture installation, update and cold restart in Direct Android; no performance claim'
    (out / 'summary.json').write_text(json.dumps(report, indent=2) + '\n')
    print(out / 'summary.json')
    if len(results) != args.repeat * len(entries) or any(not row['validated'] for row in results):
        raise SystemExit('Linux 翻译原型失败，查看对应 jsonl/log')


if __name__ == '__main__':
    main()
