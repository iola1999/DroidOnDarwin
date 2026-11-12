#!/usr/bin/env python3
"""检查 metadata 的原盘保护、参数冲突与实际副本开盘顺序。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

from probe_stock_init import ROOT


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--prior-report', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists():
        parser.error('--output 需要新目录')
    output.mkdir(parents=True)
    source, prior = args.report.resolve(), args.prior_report.resolve()
    report = json.loads(source.read_text())
    old = json.loads(prior.read_text())
    original = Path(report['input']['metadata_original']['path'])
    result = {'test': 'stock_init_metadata_input_contract', 'passed': False, 'cases': [],
              'scope': '参数拒绝与内核启动前的实际副本开盘顺序；不启动 Android 客体。',
              'sha256': {str(path): sha(path) for path in (source, prior, original, Path(__file__).resolve(), ROOT / 'scripts/probe_stock_init.py')}}
    cases = []
    try:
        for entry in ('cli', 'gcd'):
            for version, current in (('prior', old), ('current', report)):
                binary = (prior if version == 'prior' else source).parent / entry
                build = next(cmd for cmd in current['commands'] if '-o' in cmd and cmd[cmd.index('-o') + 1] == str(binary))
                includes = [Path(build[i + 1]) for i, item in enumerate(build[:-1]) if item == '-I']
                kernel = next(path / 'lkl-image.bin' for path in includes if (path / 'lkl-image.bin').exists())
                for workload in ('bionic', 'services'):
                    env = {key: value for key, value in os.environ.items() if not key.startswith('NCE_')}
                    env.update(current['test_environment'])
                    env.update(NCE_TRANSLATION_WORKLOAD=workload, NCE_STOCK_INIT_LP='1',
                               NCE_STOCK_INIT_METADATA='/__nce_missing_metadata__.img',
                               NCE_STOCK_INIT_VENDOR='/__nce_missing_vendor__.img',
                               NCE_TRANSLATION_SYSTEM='/__nce_missing_system__.img',
                               NCE_TRANSLATION_DATA='/__nce_missing_data__.img')
                    command = [str(binary), str(kernel)]
                    completed = subprocess.run(command, env=env, capture_output=True, text=True, timeout=60)
                    label = version + '-' + entry + '-' + workload
                    (output / (label + '.stdout')).write_text(completed.stdout)
                    (output / (label + '.stderr')).write_text(completed.stderr)
                    conflict_first = '!(getenv("NCE_STOCK_INIT_METADATA")' in completed.stderr
                    # prior/services 的实际分支先尝试缺失 system 文件，检查区分两种顺序。
                    expected = version == 'current' or workload == 'bionic'
                    case = {'case': label, 'command': command, 'returncode': completed.returncode,
                            'metadata_conflict_first': conflict_first, 'expected_conflict_first': expected,
                            'passed': completed.returncode != 0 and conflict_first == expected and
                                      'host_disk_close' not in completed.stdout}
                    cases.append(case)
        fake = ['missing-report.json', '--output', str(output / 'not-created')]
        commands = [('metadata_vendor_conflict', ['--metadata-image', 'missing.img', '--hyperos-manifest', 'missing.json'], '共用 slot 2'),
                    ('metadata_requires_sdk_lp', ['--runtime', 'missing', '--desktop', 'missing', '--root-metadata', 'missing',
                                                   '--metadata-image', 'missing.img'], '--sdk-partitions')]
        for name, arguments, expected in commands:
            command = [sys.executable, '-B', str(ROOT / 'scripts/probe_stock_init.py'), *fake, *arguments]
            completed = subprocess.run(command, capture_output=True, text=True, timeout=30)
            (output / (name + '.stderr')).write_text(completed.stderr)
            cases.append({'case': name, 'command': command, 'returncode': completed.returncode,
                          'passed': completed.returncode == 2 and expected in completed.stderr and not (output / 'not-created').exists()})
        metadata_root = Path(report['input']['files']['/init.environ.rc']['source']).parent
        guarded = output / 'original-guard'
        command = [sys.executable, '-B', str(ROOT / 'scripts/probe_stock_init.py'), report['source_report'],
                   '--runtime', str(Path(report['input']['runtime_manifest']).with_name('android36')),
                   '--desktop', str(Path(report['input']['desktop_manifest']).parent),
                   '--root-metadata', str(metadata_root), '--system-image', report['input']['original_system_image']['path'],
                   '--sdk-partitions', '--metadata-image', str(original), '--output', str(guarded)]
        completed = subprocess.run(command, capture_output=True, text=True, timeout=120)
        (output / 'original-guard.stderr').write_text(completed.stderr)
        cases.append({'case': 'original_metadata_writable_rejected', 'command': command, 'returncode': completed.returncode,
                      'passed': completed.returncode == 2 and 'metadata 可写盘需要与原盘' in completed.stderr and
                                not (guarded / 'cli').exists() and sha(original) == result['sha256'][str(original)]})
        result['cases'] = cases
        result['inputs_unchanged'] = all(sha(Path(path)) == digest for path, digest in result['sha256'].items())
        result['passed'] = result['inputs_unchanged'] and all(case['passed'] for case in cases)
    except (OSError, ValueError, KeyError, IndexError, subprocess.SubprocessError) as error:
        result['cases'] = cases
        result['error'] = str(error)
    (output / 'result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps({'passed': result['passed'], 'cases': len(cases), 'result': str(output / 'result.json')}))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
