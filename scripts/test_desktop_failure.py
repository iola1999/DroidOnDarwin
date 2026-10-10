#!/usr/bin/env python3
"""让真实 ART 辅助程序以非零状态退出，验证 CLI/GCD 的失败清理。"""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
import zipfile

from probe_lkl import check_ext4_shutdown

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / 'work/native-linux-nce'
INPUT = WORK / 'processes'
E2FS = Path('/opt/homebrew/opt/e2fsprogs/sbin')


def run(command, **kwargs):
    return subprocess.run([str(v) for v in command], timeout=kwargs.pop('timeout', 60), check=True, **kwargs)


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    summary = json.loads((INPUT / 'lkl-summary.json').read_text())
    for name, expected in summary['input_sha256'].items():
        assert digest(ROOT / name) == expected, name
    out = WORK / 'failure-checks' / time.strftime('%Y%m%d-%H%M%S')
    out.mkdir(parents=True)
    java = Path('/opt/homebrew/opt/openjdk@17/bin')
    sdk = Path('/opt/homebrew/share/android-commandlinetools')
    source = out / 'NceSurface.java'
    source.write_text('public class NceSurface { public static void main(String[] args) {\n'
                      ' System.err.println("NCE controlled surface failure: exit 7"); System.exit(7);\n} }\n')
    run([java / 'javac', '-source', '8', '-target', '8', '-bootclasspath',
         sdk / 'platforms/android-35/android.jar', '-d', out, source])
    classes = [p for p in sorted((INPUT / 'framework-classes').glob('*.class'))
               if not p.name.startswith('NceSurface')]
    run([java / 'java', '-cp', sdk / 'build-tools/36.0.0/lib/d8.jar', 'com.android.tools.r8.D8',
         '--min-api', '35', '--lib', sdk / 'platforms/android-35/android.jar', '--output', out,
         *classes, out / 'NceSurface.class'])
    jar = out / 'nce-framework.jar'
    with zipfile.ZipFile(jar, 'w') as archive:
        archive.writestr(zipfile.ZipInfo('classes.dex'), (out / 'classes.dex').read_bytes())
    image, data = out / 'system.img', out / 'data.img'
    # Only this generated fixture receives the failing Java class; packaged resources stay intact.
    run(['cp', '-c', INPUT / 'system.img', image])
    run(['cp', '-c', INPUT / 'android-data-seed.img', data])
    commands = out / 'fixture.debugfs'
    commands.write_text(f'rm /system/framework/nce-framework.jar\nwrite {jar} /system/framework/nce-framework.jar\n')
    with (out / 'fixture.log').open('w') as log:
        run([E2FS / 'debugfs', '-w', '-f', commands, image], stdout=log, stderr=subprocess.STDOUT)
    extracted = out / 'installed.jar'
    run([E2FS / 'debugfs', '-R', f'dump /system/framework/nce-framework.jar {extracted}', image],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    assert digest(extracted) == digest(jar)
    env = dict(os.environ, NCE_MM_MODE='demand', NCE_DESKTOP='1', NCE_ART_NTERP='1',
               NCE_ART_AOT='0', NCE_ART_BOOT_IMAGE='0', NCE_ART_EXPANDED_JIT='0', NCE_ART_FAST_BASELINE='0',
               NCE_MAP_PROFILE='0', NCE_STORAGE_IMAGE=str(data), NCE_SYSTEM_IMAGE=str(image),
               DYLD_LIBRARY_PATH=str(ROOT / 'work/angle-macos/prefix/lib'), ANGLE_DEFAULT_PLATFORM='metal')
    for key in ['NCE_DIAGNOSTICS', 'NCE_GPU_OUTPUT', 'NCE_GRAPHICS_OUTPUT']:
        env.pop(key, None)
    report = {'kernel_sha256': summary['kernel_sha256'], 'inputs': summary['input_sha256'],
              'fixture_jar_sha256': digest(jar), 'fixture_source': source.read_text(),
              'configuration': dict(interactive=True, nterp=True, aot=False, boot_image=False), 'runs': []}
    for entry in ['lkl-probe', 'dispatch-probe']:
        output, error = out / (entry + '.jsonl'), out / (entry + '.log')
        with output.open('w') as stdout, error.open('w') as stderr:
            result = subprocess.run([INPUT / entry, INPUT / 'lkl-image.bin'], env=env,
                                    stdout=stdout, stderr=stderr, timeout=360)
        rows = [json.loads(line) for line in output.read_text().splitlines()]
        text = error.read_text(errors='replace')
        assert result.returncode == 1, result.returncode
        assert 'NCE controlled surface failure: exit 7' in text
        assert 'surface-test child=' in text and 'status=1792' in text
        assert any(row.get('phase') == 'android_desktop_first_frame_wait' for row in rows)
        assert not any(row.get('session') == 'android_desktop_ready' for row in rows)
        assert any(row.get('test') == 'linux_data_readonly_shutdown' and row.get('passed') for row in rows)
        lifetime = next(row for row in rows if row.get('test') == 'linux_nce_context_lifetime')
        assert lifetime['allocated'] == lifetime['freed']
        assert rows[-1] == {'result': 'failed'}
        check_ext4_shutdown(data)
        with (out / (entry + '-fsck.log')).open('w') as log:
            run([E2FS / 'e2fsck', '-fn', data], stdout=log, stderr=subprocess.STDOUT)
        report['runs'].append({'entry': entry, 'exit_code': result.returncode, 'lifetime': lifetime,
                               'passed': True, 'output_sha256': digest(output), 'error_sha256': digest(error)})
        (out / 'summary.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
        print(entry + ': controlled failure cleaned up', flush=True)
    image.unlink()
    data.unlink()
    report['generated_disks_removed'] = True
    (out / 'summary.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(out / 'summary.json')


if __name__ == '__main__':
    main()
