#!/usr/bin/env python3
"""使用 AVD 结果和 Direct 重建日志检查 APK 阶段、摘要与私有数据。"""
import copy
import json
from pathlib import Path

from apk_fixture_validation import unobserved_creations, validate_results

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
    assert unobserved_creations(first) == [] and unobserved_creations(restarted, first) == []
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

    # These two JNI records are unchanged excerpts from the failing Direct run.
    source = json.loads((ROOT / 'tests/native-linux/fixtures/apk-activity-recreation.json').read_text())
    observed = [json.loads(row['text'].split('DroidApkFixture: ', 1)[1]) for row in source['lines']
                if 'DroidApkFixture: ' in row['text']]
    assert len(observed) == 2 and [row['launch_count'] for row in observed] == [1, 2]
    recreated = copy.deepcopy(first)
    recreated[1]['application'] = observed[0]
    second = copy.deepcopy(recreated[1])
    second['application'] = observed[1]
    recreated.insert(2, second)
    recreated[4]['application'].update(marker=observed[0]['marker'], launch_count=3)
    after_restart = copy.deepcopy(restarted)
    after_restart[1]['application'].update(marker=observed[0]['marker'], launch_count=4)
    assert validate_results(recreated, 'install-update', fixture)
    assert validate_results(after_restart, 'verify-restart', fixture, recreated)
    assert unobserved_creations(recreated) == [] and unobserved_creations(after_restart, recreated) == []

    reject(recreated, 'install-update', lambda items: items.insert(4, items.pop(2)))
    reject(recreated, 'install-update', lambda items: items.insert(4, copy.deepcopy(items[1])))
    reject(recreated, 'install-update', lambda items: items.insert(2, items.pop()))
    reject(recreated, 'install-update', lambda items: items.__setitem__(slice(0, 3), [items[1], items[2], items[0]]))
    reject(recreated, 'install-update', lambda items: items.pop(1))
    reject(recreated, 'install-update', lambda items: items.insert(2, copy.deepcopy(items[1])))
    for key, value in (('launch_count', 1), ('launch_count', 3), ('launch_count', True), ('launch_count', 1 << 31),
                       ('created_now', True), ('created_version', 2), ('status', 'failed'),
                       ('marker', '00000000-0000-0000-0000-000000000000'),
                       ('version', 2), ('value', 102), ('package', 'wrong.package')):
        reject(recreated, 'install-update', lambda items, k=key, v=value: items[2]['application'].__setitem__(k, v))

    # Update and restart windows may also contain several observed onCreate calls.
    update_multiple = copy.deepcopy(recreated)
    extra = copy.deepcopy(update_multiple[3])
    extra['application']['launch_count'] = 3
    update_multiple.insert(4, extra)
    restart_multiple = copy.deepcopy(after_restart)
    restart_multiple[0]['application']['launch_count'] = 4
    extra = copy.deepcopy(restart_multiple[1])
    extra['application']['launch_count'] = 5
    restart_multiple.insert(2, extra)
    assert validate_results(update_multiple, 'install-update', fixture)
    assert validate_results(restart_multiple, 'verify-restart', fixture, update_multiple)
    reject(update_multiple, 'install-update', lambda items: items[4]['application'].__setitem__('created_now', True))
    reject(update_multiple, 'install-update', lambda items: items[4]['application'].__setitem__('launch_count', 4))
    reject(restart_multiple, 'verify-restart', lambda items: items[2]['application'].__setitem__('created_now', True), update_multiple)
    reject(restart_multiple, 'verify-restart', lambda items: items[2]['application'].__setitem__('launch_count', 4), update_multiple)
    reject(restart_multiple, 'verify-restart', lambda items: items[0]['application'].__setitem__('created_now', True), update_multiple)

    gaps = copy.deepcopy(recreated)
    gaps[4]['application']['launch_count'] = 6
    gap_restart = copy.deepcopy(after_restart)
    gap_restart[1]['application']['launch_count'] = 9
    assert validate_results(gaps, 'install-update', fixture)
    assert validate_results(gap_restart, 'verify-restart', fixture, gaps)
    assert unobserved_creations(gaps) == [{'mode': 'jni', 'after_stage': 'fresh-v1', 'before_stage': 'updated-v2',
                                         'first_launch_count': 3, 'last_launch_count': 5, 'count': 3}]
    assert unobserved_creations(gap_restart, gaps) == [{'mode': 'jni', 'after_stage': 'updated-v2', 'before_stage': 'restarted-v2',
                                                      'first_launch_count': 7, 'last_launch_count': 8, 'count': 2}]
    print(json.dumps({'accepted_stages': 8, 'rejected_mutations': rejected, 'missing_baseline_rejected': True,
                      'real_recreation_records': len(observed), 'cross_window_gaps_reported': True,
                      'scope': 'Parser checks; update/restart multi-record sequences are constructed, not new Android runs.',
                      'passed': True}))


if __name__ == '__main__':
    main()
