#!/usr/bin/env python3
"""检查实际候选镜像的来源、清单与文件变化拒绝。"""
import argparse
from copy import deepcopy
import json
from pathlib import Path
import subprocess
import tempfile

from prepare_stock_android import verify


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    source = args.directory.resolve()
    actual = verify(source)
    rejected = []
    with tempfile.TemporaryDirectory(prefix='stock-android-check-', dir=source.parent) as temporary:
        directory = Path(temporary)
        image = directory / 'system.img'
        subprocess.run(['cp', '-c', source / 'system.img', image], check=True, timeout=30)
        baseline = deepcopy(actual)
        baseline['image']['path'] = str(image)

        def check(name, change):
            report = deepcopy(baseline)
            change(report)
            (directory / 'manifest.json').write_text(json.dumps(report))
            try:
                verify(directory)
            except (OSError, ValueError, KeyError) as error:
                rejected.append({'case': name, 'error': str(error)})
            else:
                raise AssertionError('Changed candidate accepted: ' + name)

        check('base_report_changed', lambda report: report['base_report'].update(sha256='0' * 64))
        check('prepared_manifest_changed', lambda report: report['prepared_manifest'].update(sha256='0' * 64))
        check('generator_changed', lambda report: report['generator'].update(sha256='0' * 64))
        check('replacement_removed', lambda report: report['replacements'].pop(next(iter(report['replacements']))))
        check('runtime_stage_changed', lambda report: report.update(runtime_executed=True))
        check('export_unchecked', lambda report: report.update(export_verified=False))
        with image.open('r+b') as target:
            target.seek(-1, 2)
            original = target.read(1)
            target.seek(-1, 2)
            target.write(bytes([original[0] ^ 1]))
        check('image_bytes_changed', lambda report: None)
    print(json.dumps({'actual_candidate_accepted': True, 'negative_cases': rejected,
                      'scope': 'Candidate input validation only; Android not executed.'}, ensure_ascii=False))


if __name__ == '__main__':
    main()
