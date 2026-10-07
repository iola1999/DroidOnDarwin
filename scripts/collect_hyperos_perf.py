#!/usr/bin/env python3
"""通过 ADB 采样已启动的 HyperOS；--exercise 执行设置启动与滚动对照。"""
import argparse
import datetime
import json
import subprocess
import time
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial', default='127.0.0.1:16555')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--exercise', action='store_true')
    args = parser.parse_args()
    ready = subprocess.run(['adb', '-s', args.serial, 'shell', 'getprop', 'sys.boot_completed'],
                           capture_output=True, text=True, timeout=15)
    if ready.returncode or ready.stdout.strip() != '1':
        parser.error('Android 尚未启动完成，或 ADB 尚未连接')
    args.output.mkdir(parents=True, exist_ok=False)
    records = []

    def run(label, *command, limit=60):
        began = datetime.datetime.now(datetime.timezone.utc).isoformat()
        start = time.monotonic()
        try:
            result = subprocess.run(['adb', '-s', args.serial, 'shell', *command],
                                    capture_output=True, timeout=limit)
            output, errors, code = result.stdout, result.stderr, result.returncode
        except subprocess.TimeoutExpired as error:
            output, errors, code = error.stdout or b'', error.stderr or b'', None
        (args.output / f'{label}.txt').write_bytes(output + errors)
        record = {'label': label, 'command': command, 'began_utc': began,
                  'wall_seconds': round(time.monotonic() - start, 3), 'exit_code': code}
        records.append(record)
        (args.output / 'commands.json').write_text(json.dumps(records, indent=2) + '\n')
        print(label, record['wall_seconds'], code, flush=True)
        return code

    run('identity', 'getprop', 'ro.build.fingerprint')
    run('uptime', 'cat', '/proc/uptime')
    run('memory-before', 'cat', '/proc/meminfo')
    run('vm-before', 'cat', '/proc/vmstat')
    run('pressure-before', 'cat', '/proc/pressure/cpu', '/proc/pressure/memory', '/proc/pressure/io')
    run('processes-before', 'top', '-b', '-n', '2', '-d', '2', '-m', '30')
    run('cpusets', 'sh', '-c', "'for f in /dev/cpuset/*/cpus; do echo $f; cat $f; done'")
    run('art', 'dumpsys', 'art', limit=90)
    run('gfx-before', 'dumpsys', 'gfxinfo', 'com.android.settings', 'framestats')

    if args.exercise:
        # 输入坐标对应本实验的 360×800 客体显示；尺寸不同则停止操作。
        size = subprocess.run(['adb', '-s', args.serial, 'shell', 'wm', 'size'],
                              capture_output=True, text=True, timeout=15)
        if '360x800' not in size.stdout:
            parser.error('操作脚本要求客体显示为 360x800')
        for cycle in range(1, 4):
            run(f'{cycle}-home', 'input', 'keyevent', '3')
            run(f'{cycle}-stop-settings', 'am', 'force-stop', 'com.android.settings')
            run(f'{cycle}-cold-settings', 'am', 'start', '-W', '-a', 'android.settings.SETTINGS', limit=120)
            run(f'{cycle}-gfx-reset', 'dumpsys', 'gfxinfo', 'com.android.settings', 'reset')
            for swipe in range(4):
                coords = ('180', '650', '180', '240') if swipe % 2 == 0 else ('180', '240', '180', '650')
                run(f'{cycle}-scroll-{swipe}', 'input', 'swipe', *coords, '600')
            run(f'{cycle}-gfx-scroll', 'dumpsys', 'gfxinfo', 'com.android.settings', 'framestats')
            run(f'{cycle}-home-warm', 'input', 'keyevent', '3')
            run(f'{cycle}-warm-settings', 'am', 'start', '-W', '-a', 'android.settings.SETTINGS', limit=120)
        run('end-home', 'input', 'keyevent', '3')

    run('memory-after', 'cat', '/proc/meminfo')
    run('vm-after', 'cat', '/proc/vmstat')
    run('pressure-after', 'cat', '/proc/pressure/cpu', '/proc/pressure/memory', '/proc/pressure/io')
    run('processes-after', 'top', '-b', '-n', '2', '-d', '2', '-m', '30')


if __name__ == '__main__':
    main()
