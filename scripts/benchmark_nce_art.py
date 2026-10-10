#!/usr/bin/env python3
"""交替运行同一构建的 ART 解释器配置，保留配置、回归结果和分项耗时。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import time

from analyze_nce_profile import log_summary
from probe_lkl import check_ext4_shutdown

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'work/native-linux-nce/processes'


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repeat', type=int, default=3)
    parser.add_argument('--desktop', action='store_true', help='同时执行完整桌面与应用回归')
    parser.add_argument('--storage-image', type=Path, default=BUILD / 'desktop-fresh-data.img')
    parser.add_argument('--output', type=Path, default=ROOT / 'work/native-linux-nce/art-benchmark')
    args = parser.parse_args()
    if args.repeat < 1:
        parser.error('--repeat 必须大于零')
    if not args.storage_image.is_file():
        parser.error('--storage-image 需要已有且未被占用的数据盘')
    summary = json.loads((BUILD / 'lkl-summary.json').read_text())
    for name, expected in summary['input_sha256'].items():
        if digest(ROOT / name) != expected:
            raise SystemExit('请先重新运行 probe_lkl.py：' + name)
    if digest(BUILD / 'lkl-image.bin') != summary['kernel_sha256']:
        raise SystemExit('内核与已验证构建不一致')
    folder = args.output / time.strftime('%Y%m%d-%H%M%S')
    folder.mkdir(parents=True, exist_ok=False)
    report = {
        'platform': platform.platform(),
        'hardware': subprocess.check_output(['sysctl', '-n', 'machdep.cpu.brand_string'], text=True).strip(),
        'host_ram_bytes': int(subprocess.check_output(['sysctl', '-n', 'hw.memsize'], text=True)),
        'workload': 'full-desktop-regressions' if args.desktop else 'ART-and-platform-regressions',
        'build_inputs': summary['input_sha256'],
        'art_build': summary['android_builds']['art-build'],
        'files': {name: digest(BUILD / name) for name in ('lkl-probe', 'lkl-image.bin', 'system.img')},
        'storage_image': str(args.storage_image.resolve()),
        'data_reset_between_runs': False,
        'host_file_cache_reset': False,
        'timing_scope': 'Whole diagnostic process, ART method benchmarks, and logged component timers; excludes user interaction measurements.',
        'runs': [],
    }
    env = dict(os.environ)
    for name in ('NCE_DESKTOP', 'NCE_DIAGNOSTICS', 'NCE_TRACE', 'NCE_FAULT_TRACE', 'NCE_TRACE_SYSTEMUI', 'NCE_SYSTEM_SERVER_PROBE'):
        env.pop(name, None)
    env.update(NCE_MM_MODE='demand', NCE_MAP_PROFILE='0', NCE_DIRECT_FAULT='1', NCE_SPARSE_SCAN='1',
               NCE_STORAGE_IMAGE=str(args.storage_image.resolve()), NCE_SYSTEM_IMAGE=str(BUILD / 'system.img'),
               DYLD_LIBRARY_PATH=str(ROOT / 'work/angle-macos/prefix/lib'), ANGLE_DEFAULT_PLATFORM='metal')
    if args.desktop:
        env['NCE_SYSTEM_SERVER_PROBE'] = '1'
    report['configuration'] = {key: value for key, value in env.items() if key.startswith('NCE_')}
    destination = folder / 'results.json'
    for repeat in range(args.repeat):
        for enabled in ((0, 1) if repeat % 2 == 0 else (1, 0)):
            label = f'{len(report["runs"]):02d}-nterp-{enabled}'
            output, error = folder / (label + '.jsonl'), folder / (label + '.log')
            print(f'{label}: starting', flush=True)
            started = time.monotonic()
            with output.open('w') as stdout, error.open('w') as stderr:
                result = subprocess.run([BUILD / 'lkl-probe', BUILD / 'lkl-image.bin'],
                                        env={**env, 'NCE_ART_NTERP': str(enabled)},
                                        stdout=stdout, stderr=stderr, timeout=480)
            elapsed = time.monotonic() - started
            records = [json.loads(line) for line in output.read_text().splitlines()]
            entries = [r for r in records if r.get('test') == 'android_art_interpreter_entry']
            passed = result.returncode == 0 and bool(records) and records[-1].get('result') == 'passed'
            passed = passed and len(entries) == 3 and all(r.get('nterp') == bool(enabled) for r in entries)
            run = dict(nterp=bool(enabled), elapsed_seconds=elapsed, passed=passed, returncode=result.returncode,
                       stdout=str(output), stderr=str(error),
                       art=[r for r in records if r.get('test', '').startswith('android_art_')],
                       components=log_summary(error.read_text(errors='replace')))
            report['runs'].append(run)
            destination.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
            if not passed:
                raise SystemExit('回归失败，结果已保存：' + str(destination))
            check_ext4_shutdown(args.storage_image)
            print(f'{label}: passed in {elapsed:.3f}s', flush=True)
    comparison = {}
    for enabled in (False, True):
        runs = [r for r in report['runs'] if r['nterp'] == enabled]
        measurements = {'diagnostic_process_seconds': [r['elapsed_seconds'] for r in runs]}
        for test in ('android_art_jit_arithmetic', 'android_art_intrinsics_benchmark'):
            measurements[test + '_interpreter_ms'] = [a['interpreter_ms'] for r in runs for a in r['art'] if a['test'] == test]
        comparison[str(enabled)] = {name: dict(median=statistics.median(values), min=min(values), max=max(values), samples=values)
                                    for name, values in measurements.items()}
    report['comparison'] = comparison
    destination.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(destination)


if __name__ == '__main__':
    main()
