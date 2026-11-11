#!/usr/bin/env python3
"""使用标准 AVD 的应用结果检查 APK 阶段、摘要和数据持久化解析边界。"""
import copy
import json
from pathlib import Path

from apk_fixture_validation import validate_results

ROOT = Path(__file__).resolve().parents[1]


def main():
    record = json.loads((ROOT / 'experiments/native-linux/results-apk-fixture-avd-20261112.json').read_text())
    samples = []
    for result in record['results']:
        application = result['application']
        samples.append({'test': 'android_apk_fixture', 'stage': result['stage'],
                        'artifact': f'{application["mode"]}-v{application["version"]}.apk',
                        'installed_sha256': result['installed_apk_sha256'],
                        'application': application, 'passed': True})
    fixture = {'artifacts': [
        {'apk': f'{mode}-v{version}.apk', 'package': f'app.droidondarwin.fixture.{mode}',
         'version': version, 'expected_value': 100 + version,
         'sha256': samples[index]['installed_sha256']}
        for index, (version, mode) in enumerate((version, mode) for version in (1, 2) for mode in ('java', 'jni'))
    ]}
    first = samples[:4] + [{'test': 'android_apk_fixture_stage', 'stage': 'install-update', 'installs': 4, 'passed': True}]
    restarted = samples[4:] + [{'test': 'android_apk_fixture_stage', 'stage': 'verify-restart', 'installs': 0, 'passed': True}]
    assert validate_results(first, 'install-update', fixture)
    assert validate_results(restarted, 'verify-restart', fixture, first)
    rejected = 0

    def reject(rows, stage, mutation, previous=()):
        nonlocal rejected
        changed = copy.deepcopy(rows)
        mutation(changed)
        assert not validate_results(changed, stage, fixture, previous), changed
        rejected += 1

    for rows, stage, previous in ((first, 'install-update', ()), (restarted, 'verify-restart', first)):
        reject(rows, stage, lambda items: items.pop(0), previous)
        reject(rows, stage, lambda items: items.insert(0, copy.deepcopy(items[0])), previous)
        reject(rows, stage, lambda items: items.reverse(), previous)
        for key, value in (('installed_sha256', '0' * 64), ('passed', False), ('passed', 1), ('stage', 'fresh-v1-invalid')):
            reject(rows, stage, lambda items, k=key, v=value: items[0].__setitem__(k, v), previous)
        for key, value in (('status', 'failed'), ('package', 'wrong.package'), ('value', 999), ('version', True),
                           ('created_version', 2), ('launch_count', 0), ('marker', ''), ('created_now', 0)):
            reject(rows, stage, lambda items, k=key, v=value: items[0]['application'].__setitem__(k, v), previous)
        reject(rows, stage, lambda items: items[-1].__setitem__('installs', 1), previous)
        reject(rows, stage, lambda items: items[-1].__setitem__('passed', 1), previous)
        reject(rows, stage, lambda items: items.append({'test': 'android_apk_fixture_unknown', 'passed': True}), previous)
    reject(first, 'install-update', lambda items: items[2]['application'].__setitem__('marker', '00000000-0000-0000-0000-000000000000'))
    reject(first, 'install-update', lambda items: items[2]['application'].__setitem__('launch_count', 1))
    reject(restarted, 'verify-restart', lambda items: items[0]['application'].__setitem__('marker', '00000000-0000-0000-0000-000000000000'), first)
    reject(restarted, 'verify-restart', lambda items: items[0]['application'].__setitem__('launch_count', 2), first)
    assert not validate_results(restarted, 'verify-restart', fixture)
    print(json.dumps({'accepted_stages': 2, 'rejected_mutations': rejected, 'missing_baseline_rejected': True, 'passed': True}))


if __name__ == '__main__':
    main()
