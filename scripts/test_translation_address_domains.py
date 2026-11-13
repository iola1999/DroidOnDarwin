#!/usr/bin/env python3
"""复用成功的 sparse 构建，在实际重叠的宿主/客体数值地址范围执行核心或 Bionic 检查。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

from probe_lkl_translation import SPARSE_GUEST_END, validate
from stock_bionic import unchanged

ROOT = Path(__file__).resolve().parents[1]
EXP = ROOT / 'experiments/native-linux'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate_domains(rows, config, *, kernel_log='', exec_aslr_fixtures=None):
    product_rows = [row for row in rows if 'address_domain_region' not in row and row.get('test') != 'translated_host_guest_numeric_overlap']
    if not validate(product_rows, config['memory_interval_ms'], config['mapping'], config['workload'],
                    config['tlb_invalidation'], config['execution'], config['direct'], config['cpus'], config['parallel_profile'],
                    config['uaccess_stop'], code_memory=config['code_memory'], online_cpus=config['online_cpus'],
                    guest_layout=config['guest_layout'], data_tbi=config['data_tbi'],
                    guest_page_bytes=config.get('guest_page_bytes', 16384), kernel_log=kernel_log,
                    exec_aslr_fixtures=exec_aslr_fixtures):
        return False
    layouts = [row for row in rows if row.get('config') == 'translation_guest_layout']
    regions = [row for row in rows if 'address_domain_region' in row]
    domain = [row for row in rows if row.get('test') == 'translated_host_guest_numeric_overlap']
    if len(layouts) != 1 or len(domain) != 1:
        return False
    layout = layouts[0]
    expected = [{'address_domain_region': i, 'address': layout[key], 'bytes': layout[size], 'releases': 1, 'unmapped': True}
                for i, key, size in ((0, 'reservation_base', 'reservation_bytes'), (1, 'ram_base', 'ram_bytes'))]
    overlap = all(layout['guest_base'] <= row['address'] <= layout['guest_end'] - row['bytes'] for row in expected)
    if regions != expected or rows[-3:] != regions + domain or not overlap:
        return False
    for row in regions:
        if (any(type(row[key]) is not int for key in ('address_domain_region', 'address', 'bytes', 'releases')) or
                type(row['unmapped']) is not bool):
            return False
    summary = domain[0]
    return (set(summary) == {'test', 'passed', 'created', 'vmalloc_maps', 'vmalloc_revocations', 'ram_reclaims'} and
            summary['passed'] is True and summary['created'] == 2 and
            all(type(summary[key]) is int and 0 <= summary[key] < 1 << 32 for key in ('created', 'vmalloc_maps', 'vmalloc_revocations', 'ram_reclaims')) and
            summary['vmalloc_maps'] > 0 and summary['vmalloc_revocations'] > 0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    source = args.report.resolve()
    report = json.loads(source.read_text())
    config = report['configuration']
    if (config['guest_layout'] != 'sparse' or config['workload'] not in ('core', 'bionic') or
            config['cpus'] != 1 or config['execution'] != 'direct' or config['direct']['memory'] != 'flat' or
            config['mapping'] != 'pte' or config['code_memory'] != 'dual' or
            not report['runs'] or not all(run['validated'] for run in report['runs'])):
        parser.error('需要成功的 sparse 单 CPU、PTE/Direct flat 双映射核心或 Bionic 结果')
    if 'stock_runtime' in report and not unchanged(report['stock_runtime']):
        parser.error('原始 Bionic 输入已变化')
    for name, digest in report['sha256'].items():
        if sha(ROOT / name) != digest:
            parser.error('输入已变化：' + name)
    out = args.output.resolve()
    if not out.is_relative_to(ROOT / 'work/native-linux-nce') or out == source.parent:
        parser.error('输出需要独立的 work/native-linux-nce 子目录')
    out.mkdir(parents=True, exist_ok=True)
    execution = out / 'linux-execution.c'
    execution_source = (EXP / 'translation/linux-execution.c').read_text()
    execution.write_text('#define mach_vm_map nce_test_domain_map\n'
                         '#define vm_remap nce_test_domain_remap\n'
                         '#define mmap nce_test_domain_mmap\n'
                         '#define munmap nce_test_domain_unmap\n'
                         + execution_source)
    keys = {'GUEST_LAYOUT': 'guest_layout', 'DATA_TBI': 'data_tbi', 'ONLINE_CPUS': 'online_cpus',
            'BATCH': 'instruction_batch', 'PARALLEL_PROFILE': 'parallel_profile', 'UACCESS_STOP': 'uaccess_stop',
            'PREEMPTION': 'preemption', 'MEMORY_MS': 'memory_interval_ms', 'WORKLOAD': 'workload',
            'CACHE': 'cache_invalidation', 'EXEC_CACHE': 'exec_cache', 'TLB': 'tlb_invalidation', 'LINUX_FASTPATH': 'linux_fastpath'}
    selected = {'NCE_TRANSLATION_' + key: str(int(config[value]) if type(config[value]) is bool else config[value]) for key, value in keys.items()}
    selected.update({'NCE_DIRECT_' + key.upper(): str(int(value) if type(value) is bool else value) for key, value in config['direct'].items()})
    selected.update(NCE_TRANSLATION_FILES=str(out), NCE_TRANSLATION_CACHE_MIB=str(config['translation_cache_bytes'] // (1 << 20)),
                    NCE_ART_NTERP=str(config['art_nterp']), NCE_ART_EXPANDED_JIT=str(config['art_expanded_jit']),
                    NCE_ART_FAST_BASELINE=str(config['art_fast_baseline']), NCE_ART_AOT='0', NCE_ART_BOOT_IMAGE='0', NCE_SCHED_SLICE_US='0')
    environment = {**{key: value for key, value in os.environ.items() if not key.startswith('NCE_')}, **selected}
    result = {key: report[key] for key in ('platform', 'cpu', 'system_build', 'configuration', 'dependency')}
    if 'stock_runtime' in report:
        result['stock_runtime'] = report['stock_runtime']
    if 'exec_aslr_fixtures' in report:
        result['exec_aslr_fixtures'] = report['exec_aslr_fixtures']
    result.update(scope='Formal 512 GiB sparse guest with host-lifecycle test inputs; functional checks only; iOS unverified',
                  test_guest_end=SPARSE_GUEST_END,
                  source_report=str(source), source_report_sha256=sha(source), test_environment=selected,
                  commands=[], runs=[], sha256=dict(report['sha256']))
    for path in (Path(__file__).resolve(), EXP / 'translation/address-domain-test.c', execution):
        result['sha256'][str(path.relative_to(ROOT))] = sha(path)
    for path in source.parent.iterdir():
        if path.suffix in ('.elf', '.so') or path.name in ('stock-runtime', 'stock-ld.config.txt'):
            target = out / path.name
            if not target.exists():
                target.symlink_to(path)
    for entry in ('cli', 'gcd'):
        command = next(list(cmd) for cmd in report['commands'] if '-o' in cmd and Path(cmd[cmd.index('-o') + 1]).name == entry)
        includes = [command[i + 1] for i, value in enumerate(command[:-1]) if value == '-I']
        kernel = next(Path(path) / 'lkl-image.bin' for path in includes if (Path(path) / 'lkl-image.bin').is_file())
        command[command.index(str(EXP / 'translation/linux-execution.c'))] = str(execution)
        command[command.index('-o') + 1] = str(out / entry)
        command.extend(['-iquote', str(EXP / 'translation'), str(EXP / 'translation/address-domain-test.c')])
        subprocess.run(command, check=True, timeout=120)
        result['commands'].append(command)
        result['sha256'][str((out / entry).relative_to(ROOT))] = sha(out / entry)
        with (out / f'{entry}.jsonl').open('w') as stdout, (out / f'{entry}.log').open('w') as stderr:
            try:
                proc = subprocess.run([out / entry, kernel], env=environment, stdout=stdout, stderr=stderr, timeout=120)
                returncode = proc.returncode
            except subprocess.TimeoutExpired:
                returncode = 'timeout'
        rows = [json.loads(line) for line in (out / f'{entry}.jsonl').read_text().splitlines() if line.startswith('{')]
        stderr = (out / f'{entry}.log').read_text()
        valid = returncode == 0 and validate_domains(rows, config, kernel_log=stderr,
                                                   exec_aslr_fixtures=report.get('exec_aslr_fixtures'))
        run = {'entry': entry, 'returncode': returncode, 'validated': valid, 'results': rows,
               'stderr': stderr, 'stdout_sha256': sha(out / f'{entry}.jsonl'), 'stderr_sha256': sha(out / f'{entry}.log')}
        if 'stock_runtime' in report:
            stages = [row for row in rows if row.get('component_stage') == 'dynamic']
            run['component_compatible'] = valid and len(stages) == 1 and stages[0].get('success') is True
            run['validated'] = valid and unchanged(report['stock_runtime']) and run['component_compatible']
        result['runs'].append(run)
        (out / 'summary.json').write_text(json.dumps(result, indent=2) + '\n')
        if not run['validated']:
            raise SystemExit('宿主/客体数值重叠检查失败：' + str(out / 'summary.json'))
    if any(sha(ROOT / name) != digest for name, digest in result['sha256'].items()):
        raise SystemExit('检查期间输入发生变化')
    print(out / 'summary.json')


if __name__ == '__main__':
    main()
