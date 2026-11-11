#!/usr/bin/env python3
"""核对原始 APK 输入及 Direct 两阶段安装结果，不启动 Android。"""
import json
from pathlib import Path
import re

from build_apk_fixture import BUILD_TOOLS, digest, run

APPLICATION_KEYS = {'status', 'package', 'mode', 'version', 'value', 'marker',
                    'created_now', 'created_version', 'launch_count'}


def read_fixture(directory):
    directory = Path(directory).resolve()
    manifest = directory / 'manifest.json'
    if manifest.stat().st_size > 65536:
        raise ValueError('APK 清单超过 64 KiB')
    artifacts = json.loads(manifest.read_text())['artifacts']
    expected = {f'{mode}-v{version}.apk' for mode in ('java', 'jni') for version in (1, 2)}
    if len(artifacts) != 4 or {item['apk'] for item in artifacts} != expected:
        raise ValueError('需要 Java/JNI 的 v1/v2 四个 APK')
    hashes = {'manifest.json': digest(manifest)}
    certificates = set()
    for item in artifacts:
        name = item['apk']
        mode, version = name.split('-v')
        version = int(version.removesuffix('.apk'))
        package = f'app.droidondarwin.fixture.{mode}'
        path = directory / name
        hashes[name] = digest(path)
        if (item.get('package') != package or type(item.get('version')) is not int or item['version'] != version
                or type(item.get('expected_value')) is not int or item['expected_value'] != 100 + version
                or hashes[name] != item.get('sha256') or path.stat().st_size != item.get('bytes')):
            raise ValueError('APK 输入与清单不一致：' + name)
        verified = run([BUILD_TOOLS / 'apksigner', 'verify', '--verbose', '--print-certs', path])
        found = re.findall(r'Signer #\d+ certificate SHA-256 digest: ([0-9a-fA-F]{64})', verified)
        if len(found) != 1 or found[0].lower() != item.get('certificate_sha256'):
            raise ValueError('APK 签名证书与清单不一致：' + name)
        certificates.add(found[0].lower())
        badging = run([BUILD_TOOLS / 'aapt2', 'dump', 'badging', path])
        if f"package: name='{package}' versionCode='{version}'" not in badging:
            raise ValueError('APK 包名或版本不一致：' + name)
    if len(certificates) != 1:
        raise ValueError('APK fixture 需要同一签名证书')
    return {'directory': str(directory), 'sha256': hashes, 'artifacts': artifacts}


def inputs_unchanged(fixture):
    root = Path(fixture['directory'])
    return all((root / name).is_file() and digest(root / name) == value
               for name, value in fixture['sha256'].items())


def validate_results(rows, stage, fixture, previous=()):
    if stage not in ('install-update', 'verify-restart'):
        return False
    completion = [row for row in rows if row.get('test') == 'android_apk_fixture_stage']
    if completion != [{'test': 'android_apk_fixture_stage', 'stage': stage,
                       'installs': 4 if stage == 'install-update' else 0, 'passed': True}]:
        return False
    if completion[0]['passed'] is not True or type(completion[0]['installs']) is not int:
        return False
    samples = [row for row in rows if row.get('test') == 'android_apk_fixture']
    stages = ('fresh-v1', 'updated-v2') if stage == 'install-update' else ('restarted-v2',)
    expected = [(phase, f'{mode}-v{1 if phase == "fresh-v1" else 2}.apk')
                for phase in stages for mode in ('java', 'jni')]
    if [(row.get('stage'), row.get('artifact')) for row in samples] != expected:
        return False
    if any(isinstance(row.get('test'), str) and row['test'].startswith('android_apk_fixture')
           and row not in samples + completion for row in rows):
        return False
    artifacts = {item['apk']: item for item in fixture['artifacts']}
    last = {row['application']['mode']: row['application'] for row in previous
            if row.get('test') == 'android_apk_fixture'}
    if stage == 'verify-restart' and set(last) != {'java', 'jni'}:
        return False
    for row in samples:
        if (set(row) != {'test', 'stage', 'artifact', 'installed_sha256', 'application', 'passed'}
                or row.get('passed') is not True):
            return False
        artifact = artifacts[row['artifact']]
        application = row.get('application')
        if not isinstance(application, dict) or set(application) != APPLICATION_KEYS:
            return False
        mode = row['artifact'].split('-v')[0]
        if (row['installed_sha256'] != artifact['sha256'] or application['status'] != 'passed'
                or application['package'] != artifact['package'] or application['mode'] != mode
                or any(type(application[key]) is not int for key in ('version', 'value', 'created_version', 'launch_count'))
                or application['version'] != artifact['version'] or application['value'] != artifact['expected_value']
                or application['created_version'] != 1 or application['launch_count'] < 1
                or type(application['marker']) is not str
                or re.fullmatch(r'[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}', application['marker']) is None
                or application['created_now'] is not (row['stage'] == 'fresh-v1')):
            return False
        if row['stage'] == 'fresh-v1':
            if application['launch_count'] != 1:
                return False
        elif (mode not in last or application['marker'] != last[mode]['marker']
              or application['launch_count'] <= last[mode]['launch_count']):
            return False
        last[mode] = application
    return True
