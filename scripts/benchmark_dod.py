#!/usr/bin/env python3
"""运行 macOS/iPhone 桌面性能实验，交替参数并收集每次运行的配置、状态和日志。"""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import platform
import plistlib
import statistics
import subprocess
import time
import uuid

from analyze_nce_profile import log_summary

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'work/native-linux-nce/benchmarks'
DEFAULT_MATRIX = {
    'configurations': [{'name': 'baseline', 'parameters': {'nterp': False}}, {'name': 'nterp', 'parameters': {'nterp': True}}],
    'actions': [{'kind': 'wait', 'milliseconds': 2000}, {'kind': 'key', 'key': 'home'},
                {'kind': 'swipe', 'x': 180, 'y': 560, 'endX': 180, 'endY': 120, 'milliseconds': 400},
                {'kind': 'wait', 'milliseconds': 2000}, {'kind': 'key', 'key': 'home'}],
}


def save(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n')


def devicectl(args, command, check=True):
    return subprocess.run(['xcrun', 'devicectl', *command, '--device', args.phone, '--timeout', '30'],
                          capture_output=True, text=True, timeout=40, check=check)


def copy_phone(args, source, destination):
    try:
        return devicectl(args, ['device', 'copy', 'from', '--domain-type', 'appDataContainer',
                               '--domain-identifier', args.bundle_id, '--source', 'Documents/Benchmarks/' + source,
                               '--destination', str(destination)], check=False).returncode == 0
    except subprocess.SubprocessError:
        return False


def launch_phone(args, url, restart=False):
    command = ['device', 'process', 'launch', '--payload-url', url]
    if restart:
        command.append('--terminate-existing')
    # 启动参数在 bundle ID 后面；devicectl 自身选项必须在它前面。
    return subprocess.run(['xcrun', 'devicectl', *command, '--device', args.phone, '--timeout', '30',
                           args.bundle_id, '--allow-benchmark'], capture_output=True, text=True, timeout=40, check=True)


def phone_idle(args, folder):
    request_id = str(uuid.uuid4()).upper()
    launch_phone(args, 'droidondarwin://benchmark-status?requestID=' + request_id)
    for _ in range(15):
        time.sleep(2)
        file = folder / 'app-status.json'
        if copy_phone(args, 'app-status.json', file):
            status = json.loads(file.read_text())
            if status.get('requestID') == request_id:
                if status.get('active'):
                    raise RuntimeError('iPhone 上有活动会话，请先正常停止；本脚本未结束该会话')
                return status
    raise RuntimeError('无法读取新状态；请确认 App 已启用自动化测试、手机已解锁且连接可用')


def measure(result):
    events = result.get('events', [])
    def first(name):
        return next((r['elapsed_ms'] for r in events if r['event'] == name), None)
    pairs = {'start_to_ready_observed_ms': ('accepted', 'ready_observed'),
             'core_to_ready_observed_ms': ('booting', 'ready_observed'),
             'actions_ms': ('ready_observed', 'actions_completed'),
             'stop_ms': ('actions_completed', 'completed')}
    metrics = {name: first(end) - first(start) for name, (start, end) in pairs.items()
               if first(start) is not None and first(end) is not None}
    memory = result.get('memory', {})
    if memory.get('enabled'):
        buckets = {'overall': memory['overall'], **{'phase_' + k: v for k, v in memory['phases'].items()},
                   **{'boot_step_' + k: v for k, v in memory['boot_steps'].items()}}
        for stage, bucket in buckets.items():
            for name, values in bucket['metrics'].items():
                for stat in ('mean_bytes', 'sampled_peak_bytes', 'latest_bytes'):
                    metrics[f'memory_{stage}_{name}_{stat}'] = values[stat]
        for name, value in memory['process_lifetime_peak_bytes'].items():
            metrics[f'memory_process_lifetime_{name}_peak_bytes'] = value
    return metrics


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--platform', choices=['macos', 'ios'], default='macos')
    parser.add_argument('--app', type=Path, default=ROOT / 'work/native-linux-nce/macos/DroidOnDarwin.app')
    parser.add_argument('--data-directory', type=Path, default=OUT / 'device')
    parser.add_argument('--device-id', help='iPhone 上已有虚拟设备的 UUID；Mac 可省略')
    parser.add_argument('--phone', help='devicectl 中的设备 ID 或名称')
    parser.add_argument('--bundle-id', default='app.droidondarwin.ios')
    parser.add_argument('--matrix', type=Path, help='包含 configurations 和 actions 的 JSON 文件')
    parser.add_argument('--repeat', type=int, default=3)
    parser.add_argument('--timeout', type=int, default=600)
    parser.add_argument('--output', type=Path, default=OUT)
    parser.add_argument('--no-memory-sampling', action='store_true', help='关闭宿主内存采样，用于评估采集开销')
    args = parser.parse_args()
    if args.repeat < 1 or not 30 <= args.timeout <= 900:
        parser.error('repeat 需要大于零，timeout 为 30..900 秒')
    if args.platform == 'ios' and not args.phone:
        parser.error('iOS 需要 --phone')
    matrix = json.loads(args.matrix.read_text()) if args.matrix else DEFAULT_MATRIX
    if set(matrix) != {'configurations', 'actions'} or not matrix['configurations']:
        parser.error('matrix 需要 configurations 和 actions')
    names = set()
    for variant in matrix['configurations']:
        if set(variant) != {'name', 'parameters'} or variant['name'] in names:
            parser.error('配置需要唯一 name 和 parameters')
        names.add(variant['name'])
        if any(type(v) is not bool for v in variant['parameters'].values()):
            parser.error('性能参数必须是布尔值')
        if variant['parameters'].get('diagnostics'):
            parser.error('普通桌面性能实验需要关闭 diagnostics')
    folder = args.output.resolve() / (time.strftime('%Y%m%d-%H%M%S') + '-' + uuid.uuid4().hex[:6])
    folder.mkdir(parents=True)
    report = {'platform': args.platform, 'driver_os': platform.platform(), 'matrix': matrix, 'runs': [],
              'data_reset_between_runs': False, 'host_file_cache_reset': False,
              'measurement': 'App starts through observed desktop ready, scripted input submission and clean stop. Ready has up to one polling interval of observation delay. No FPS or application render latency assertion.'}
    save(folder / 'summary.json', report)
    if args.platform == 'ios':
        status = phone_idle(args, folder)
        defaults = status.get('configuration_defaults')
        if copy_phone(args, 'devices.json', folder / 'devices.json'):
            devices = json.loads((folder / 'devices.json').read_text())
            if not args.device_id and len(devices) == 1:
                args.device_id = devices[0]['id']
        if not args.device_id:
            raise SystemExit('用 --device-id 选择 devices.json 中的虚拟设备；列表已保存至 ' + str(folder))
    else:
        args.data_directory = args.data_directory.resolve()
        args.device_id = args.device_id or str(uuid.uuid5(uuid.NAMESPACE_URL, str(args.data_directory))).upper()
        report['data_directory'] = str(args.data_directory)
        report['data_image_existed_before_run'] = (args.data_directory / 'android-data.img').exists()
        info = plistlib.loads((args.app / 'Contents/Info.plist').read_bytes())
        if not info.get('DroidOnDarwinBenchmarkConfigurationQuery'):
            raise SystemExit('这个 App 缺少参数查询入口，请重新构建后运行实验')
        defaults = json.loads(subprocess.check_output(
            [str(args.app / 'Contents/MacOS/DroidOnDarwin'), '--benchmark-configuration'], text=True, timeout=15))
        report['bundle_version'] = info['CFBundleShortVersionString']
        report['build_id'] = info.get('DroidOnDarwinBuildID')
        report['host_cpu'] = subprocess.check_output(['sysctl', '-n', 'machdep.cpu.brand_string'], text=True).strip()
        with (args.app / 'Contents/MacOS/DroidOnDarwin').open('rb') as stream:
            report['executable_sha256'] = hashlib.file_digest(stream, 'sha256').hexdigest()
    if not isinstance(defaults, dict) or not defaults or any(type(v) is not bool for v in defaults.values()):
        raise SystemExit('App 未返回有效的默认参数；请更新 App 与实验脚本')
    for variant in matrix['configurations']:
        if unknown := set(variant['parameters']) - defaults.keys():
            raise SystemExit('App 尚未实现这些参数：' + ', '.join(sorted(unknown)))
    report['app_configuration_defaults'] = defaults
    args.device_id = str(uuid.UUID(args.device_id)).upper()
    for repeat in range(args.repeat):
        order = matrix['configurations'] if repeat % 2 == 0 else list(reversed(matrix['configurations']))
        for variant in order:
            run_id = str(uuid.uuid4()).upper()
            request = dict(version=1, runID=run_id, deviceID=args.device_id, configuration={**defaults, **variant['parameters']},
                           actions=matrix['actions'], timeoutSeconds=args.timeout, memorySampling=not args.no_memory_sampling)
            request_file = folder / (run_id + '-request.json')
            save(request_file, request)
            print(f'{len(report["runs"])} {variant["name"]}: {run_id}', flush=True)
            process = None
            result = None
            error = None
            path = folder / run_id
            console = None
            try:
                if args.platform == 'macos':
                    console = (folder / (run_id + '-host.log')).open('w')
                    process = subprocess.Popen([str(args.app.resolve() / 'Contents/MacOS/DroidOnDarwin'), '--run-device',
                                                '--data-directory', str(args.data_directory), '--device-id', args.device_id,
                                                '--benchmark-request', str(request_file)], stdout=console, stderr=subprocess.STDOUT)
                else:
                    encoded = base64.urlsafe_b64encode(json.dumps(request).encode()).decode().rstrip('=')
                    launch_phone(args, 'droidondarwin://benchmark?request=' + encoded, restart=True)
                    path.mkdir()
                deadline = time.monotonic() + args.timeout + 60
                while time.monotonic() < deadline:
                    if args.platform == 'ios':
                        copy_phone(args, run_id + '/result.json', path / 'result.json')
                        if copy_phone(args, run_id + '-rejected.json', path / 'rejected.json'):
                            raise RuntimeError((path / 'rejected.json').read_text())
                    if (path / 'result.json').exists():
                        result = json.loads((path / 'result.json').read_text())
                        if result.get('runID') != run_id or result.get('configuration') != request['configuration']:
                            raise RuntimeError('运行编号或实际配置不匹配')
                        if result['status'] in ('completed', 'failed') and not result['session_active']:
                            break
                    if process and process.poll() is not None:
                        raise RuntimeError(f'App 提前退出：{process.returncode}')
                    time.sleep(10 if args.platform == 'ios' else 1)
                else:
                    raise RuntimeError('实验未在时限内正常停止')
                if result['status'] != 'completed':
                    raise RuntimeError(result.get('error', '实验失败'))
                if process:
                    process.wait(timeout=15)
                    if process.returncode:
                        raise RuntimeError(f'App 退出状态：{process.returncode}')
            except (RuntimeError, subprocess.SubprocessError, OSError, ValueError, KeyError) as failure:
                error = str(failure)
                if process and process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=30)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
                        error += '; 正常停止超时，App 已强制结束，数据盘需要检查'
                elif args.platform == 'ios':
                    try:
                        launch_phone(args, 'droidondarwin://benchmark-stop?runID=' + run_id)
                    except subprocess.SubprocessError:
                        pass
            finally:
                if console:
                    console.close()
                if args.platform == 'ios':
                    for name in ('result.json', 'session.log', 'request.json', 'memory.jsonl'):
                        copy_phone(args, run_id + '/' + name, path / name)
                    if (path / 'result.json').exists():
                        result = json.loads((path / 'result.json').read_text())
                    if not (path / 'session.log').exists():
                        error = error or '未能收集运行日志'
                record = {'runID': run_id, 'configuration_name': variant['name'], 'repeat': repeat, 'request': request,
                          'result': result, 'metrics': measure(result or {}), 'error': error}
                log = path / 'session.log'
                if log.exists():
                    contents = log.read_text(errors='replace')
                    record['components'] = log_summary(contents)
                    configurations = []
                    for line in contents.splitlines():
                        if line.startswith('{"configuration":'):
                            configurations.append(json.loads(line)['configuration'])
                    if configurations != [request['configuration']]:
                        error = error or '会话日志中的生效配置不匹配'
                    if 'bootImage' in request['configuration']:
                        record['boot_image_loaded'] = 'NCE_BOOT_IMAGE loaded=' in contents
                        if record['boot_image_loaded'] != request['configuration']['bootImage']:
                            error = error or '公共启动镜像的实际加载状态与请求不匹配'
                    if record['components']['java_fatal_exceptions'] or record['components']['anrs']:
                        error = error or '运行日志包含 Java 崩溃或 ANR'
                    record['error'] = error
                elif not error:
                    error = record['error'] = '缺少会话日志'
                if not args.no_memory_sampling and not error:
                    memory = (result or {}).get('memory', {})
                    if not memory.get('enabled') or not (path / 'memory.jsonl').exists():
                        error = record['error'] = '缺少请求的内存指标或采样序列'
                    elif not memory.get('overall', {}).get('metrics'):
                        error = record['error'] = '所有内存查询均失败；本轮不能用于参数比较'
                report['runs'].append(record)
                save(folder / 'summary.json', report)
            if error:
                raise SystemExit('实验中止：' + error + '\n结果：' + str(folder))
    report['comparison'] = {}
    for name in names:
        runs = [r for r in report['runs'] if r['configuration_name'] == name]
        report['comparison'][name] = {}
        for metric in {key for r in runs for key in r['metrics']}:
            values = [r['metrics'][metric] for r in runs if metric in r['metrics']]
            report['comparison'][name][metric] = dict(median=statistics.median(values), min=min(values), max=max(values), samples=values)
    save(folder / 'summary.json', report)
    print(folder / 'summary.json')


if __name__ == '__main__':
    main()
