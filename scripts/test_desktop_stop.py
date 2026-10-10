#!/usr/bin/env python3
"""在真实 Mac App 的不同启动阶段停止，检查进程退出、重启和 ext4 状态。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import signal
import subprocess
import time

from probe_lkl import check_ext4_shutdown

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / 'work/native-linux-nce'
PHASES = {
    'services': '"phase":"android_system_server_boot"',
    'first-frame': '"phase":"android_desktop_first_frame_wait"',
    'running': '"session":"android_desktop_ready"',
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', type=Path, default=WORK / 'macos/DroidOnDarwin.app')
    parser.add_argument('--data-directory', type=Path, default=WORK / 'benchmarks/device')
    parser.add_argument('--phase', choices=PHASES, action='append')
    args = parser.parse_args()
    out = WORK / 'stop-checks' / time.strftime('%Y%m%d-%H%M%S')
    out.mkdir(parents=True)
    binary = args.app.resolve() / 'Contents/MacOS/DroidOnDarwin'
    info = plistlib.loads((args.app / 'Contents/Info.plist').read_bytes())
    report = {'build': info['DroidOnDarwinBuildID'], 'data_directory': str(args.data_directory.resolve()),
              'configuration': dict(nterp=True, aot=False, boot_image=False, diagnostics=False), 'runs': []}
    env = dict(os.environ, NCE_ART_NTERP='1', NCE_ART_AOT='0', NCE_ART_BOOT_IMAGE='0',
               NCE_ART_EXPANDED_JIT='0', NCE_ART_FAST_BASELINE='0', NCE_MAP_PROFILE='0',
               NCE_DIRECT_FAULT='1', NCE_FAULT_PREFETCH='0', NCE_SPARSE_SCAN='1')
    env.pop('NCE_DIAGNOSTICS', None)
    for index, phase in enumerate(args.phase or PHASES):
        row = {'phase': phase}
        report['runs'].append(row)
        log = out / f'{index}-{phase}.log'
        stopped = False
        with (out / f'{index}-{phase}-host.log').open('w') as host:
            process = subprocess.Popen([binary, '--run-device', '--data-directory', args.data_directory.resolve(),
                                        '--session-log', log], stdout=host, stderr=subprocess.STDOUT, env=env)
            try:
                deadline = time.monotonic() + 360
                while time.monotonic() < deadline:
                    if process.poll() is not None:
                        raise RuntimeError(f'App 在目标阶段前退出：{process.returncode}')
                    text = log.read_text(errors='replace') if log.exists() else ''
                    if PHASES[phase] in text:
                        break
                    time.sleep(0.1)
                else:
                    raise RuntimeError('目标阶段等待到期：' + phase)
                row['ready_before_stop'] = PHASES['running'] in text
                if phase != 'running' and row['ready_before_stop']:
                    raise RuntimeError('停止前已进入桌面，未覆盖指定等待阶段')
                start = time.monotonic()
                process.send_signal(signal.SIGTERM)
                stopped = True
                row['exit_code'] = process.wait(timeout=90)
                row['stop_seconds'] = time.monotonic() - start
                assert row['exit_code'] == 0, row
                text = log.read_text(errors='replace')
                for marker in ['android_system_process_groups_stopped', 'android_desktop_stopped',
                               'linux_data_readonly_shutdown', '"result":"passed"']:
                    assert marker in text, marker
                if phase != 'running':
                    assert PHASES['running'] not in text
                if phase == 'first-frame':
                    assert 'android_surface_wait_cancelled' in text
                disk = args.data_directory / 'android-data.img'
                check_ext4_shutdown(disk)
                with (out / f'{index}-{phase}-fsck.log').open('w') as fsck:
                    subprocess.run(['/opt/homebrew/opt/e2fsprogs/sbin/e2fsck', '-fn', disk],
                                   stdout=fsck, stderr=subprocess.STDOUT, check=True, timeout=60)
                row['passed'] = True
                print(json.dumps(row), flush=True)
            except Exception as error:
                row['error'] = str(error)
                raise
            finally:
                if process.poll() is None:
                    if not stopped:
                        process.send_signal(signal.SIGTERM)
                    try:
                        process.wait(timeout=30)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=10)
                        row['forced_exit'] = True
                if log.exists():
                    row['log_sha256'] = hashlib.sha256(log.read_bytes()).hexdigest()
                (out / 'summary.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(out / 'summary.json')


if __name__ == '__main__':
    main()
