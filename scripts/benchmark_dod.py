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
import re
import shutil
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


def missing_displayed_packages(components, required):
    displayed = set()
    for row in components['displayed_activities']:
        match = re.search(r'ActivityTaskManager: Displayed ([^ /]+)/[^ ]+ for user ', row['message'])
        if match:
            displayed.add(match[1])
    return sorted(set(required) - displayed)


def save(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n')


def prepare_seeded_data(seed, destination):
    # Each run owns a new directory; existing device data is never replaced.
    if not seed.is_file():
        raise ValueError('实验数据种子不存在：' + str(seed))
    destination.mkdir()
    shutil.copyfile(seed, destination / 'android-data.img')
    with (destination / 'android-data.img').open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


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


def parameter_values_valid(parameters):
    return isinstance(parameters, dict) and all(
        (key == 'translation' and isinstance(value, dict) and value and all(type(item) in (bool, int) for item in value.values()))
        or (key != 'translation' and type(value) in (bool, int)) for key, value in parameters.items())


def merge_parameters(defaults, requested):
    unknown = set(requested) - defaults.keys()
    if unknown:
        raise ValueError('App 尚未实现这些参数：' + ', '.join(sorted(unknown)))
    result = dict(defaults)
    for key, value in requested.items():
        if type(value) is not type(defaults[key]):
            raise ValueError('参数类型与 App 不一致：' + key)
        if key == 'translation':
            result[key] = merge_parameters(defaults[key], value)
        else:
            result[key] = value
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--platform', choices=['macos', 'ios'], default='macos')
    parser.add_argument('--app', type=Path, default=ROOT / 'work/native-linux-nce/macos/DroidOnDarwin.app')
    parser.add_argument('--data-directory', type=Path, default=OUT / 'device')
    parser.add_argument('--data-seed', type=Path, help='Mac 每轮从指定实验种子新建数据目录，替代 data-directory')
    parser.add_argument('--device-id', help='iPhone 上已有虚拟设备的 UUID；Mac 可省略')
    parser.add_argument('--phone', help='devicectl 中的设备 ID 或名称')
    parser.add_argument('--bundle-id', default='app.droidondarwin.ios')
    parser.add_argument('--matrix', type=Path, help='包含 configurations 和 actions 的 JSON 文件')
    parser.add_argument('--require-displayed-package', action='append', default=[],
                        help='要求日志包含该包的 Activity 首帧，可重复指定；适用于本次会话首次打开的 Activity')
    parser.add_argument('--repeat', type=int, default=3)
    parser.add_argument('--timeout', type=int, default=600)
    parser.add_argument('--output', type=Path, default=OUT)
    parser.add_argument('--no-memory-sampling', action='store_true', help='关闭宿主内存采样，用于评估采集开销')
    args = parser.parse_args()
    if args.repeat < 1 or not 30 <= args.timeout <= 900:
        parser.error('repeat 需要大于零，timeout 为 30..900 秒')
    if args.platform == 'ios' and not args.phone:
        parser.error('iOS 需要 --phone')
    if args.data_seed and args.platform != 'macos':
        parser.error('data-seed 当前只适用于 Mac')
    matrix = json.loads(args.matrix.read_text()) if args.matrix else DEFAULT_MATRIX
    if set(matrix) != {'configurations', 'actions'} or not matrix['configurations']:
        parser.error('matrix 需要 configurations 和 actions')
    names = set()
    for variant in matrix['configurations']:
        if set(variant) != {'name', 'parameters'} or variant['name'] in names:
            parser.error('配置需要唯一 name 和 parameters')
        names.add(variant['name'])
        if not parameter_values_valid(variant['parameters']):
            parser.error('性能参数必须是布尔值或整数')
        if variant['parameters'].get('diagnostics'):
            parser.error('普通桌面性能实验需要关闭 diagnostics')
    folder = args.output.resolve() / (time.strftime('%Y%m%d-%H%M%S') + '-' + uuid.uuid4().hex[:6])
    folder.mkdir(parents=True)
    report = {'platform': args.platform, 'driver_os': platform.platform(), 'matrix': matrix, 'runs': [],
              'required_displayed_packages': args.require_displayed_package,
              'data_reset_between_runs': False, 'host_file_cache_reset': False,
              'measurement': 'App starts through observed desktop ready, scripted input submission and clean stop. Ready has up to one polling interval of observation delay. No FPS or application render latency assertion.'}
    save(folder / 'summary.json', report)
    if args.data_seed:
        args.data_seed = args.data_seed.resolve()
        with args.data_seed.open('rb') as stream:
            report['data_seed'] = {'path': str(args.data_seed), 'sha256': hashlib.file_digest(stream, 'sha256').hexdigest()}
        report['data_reset_between_runs'] = True
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
        report['data_directory'] = None if args.data_seed else str(args.data_directory)
        report['data_image_existed_before_run'] = False if args.data_seed else (args.data_directory / 'android-data.img').exists()
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
    if not defaults or not parameter_values_valid(defaults):
        raise SystemExit('App 未返回有效的默认参数；请更新 App 与实验脚本')
    for variant in matrix['configurations']:
        try:
            merge_parameters(defaults, variant['parameters'])
        except ValueError as error:
            raise SystemExit(str(error)) from error
    report['app_configuration_defaults'] = defaults
    args.device_id = str(uuid.UUID(args.device_id)).upper()
    for repeat in range(args.repeat):
        order = matrix['configurations'] if repeat % 2 == 0 else list(reversed(matrix['configurations']))
        for variant in order:
            run_id = str(uuid.uuid4()).upper()
            request = dict(version=1, runID=run_id, deviceID=args.device_id, configuration=merge_parameters(defaults, variant['parameters']),
                           actions=matrix['actions'], timeoutSeconds=args.timeout, memorySampling=not args.no_memory_sampling)
            request_file = folder / (run_id + '-request.json')
            save(request_file, request)
            print(f'{len(report["runs"])} {variant["name"]}: {run_id}', flush=True)
            process = None
            result = None
            error = None
            path = folder / run_id
            console = None
            initial_data = None
            run_data = args.data_directory
            try:
                if args.platform == 'macos':
                    if args.data_seed:
                        run_data = folder / (run_id + '-device')
                        initial_data = prepare_seeded_data(args.data_seed, run_data)
                        if initial_data != report['data_seed']['sha256']:
                            raise RuntimeError('实验种子或复制后的数据哈希发生变化')
                    data_image = run_data / 'android-data.img'
                    if not args.data_seed and data_image.exists():
                        with data_image.open('rb') as disk:
                            initial_data = hashlib.file_digest(disk, 'sha256').hexdigest()
                    console = (folder / (run_id + '-host.log')).open('w')
                    process = subprocess.Popen([str(args.app.resolve() / 'Contents/MacOS/DroidOnDarwin'), '--run-device',
                                                '--data-directory', str(run_data), '--device-id', args.device_id,
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
                    if any(action.get('kind') == 'profile' for action in request['actions']):
                        if not copy_phone(args, run_id + '/interaction', path / 'interaction'):
                            error = error or '未能收集交互采集文件'
                    if (path / 'result.json').exists():
                        result = json.loads((path / 'result.json').read_text())
                    if not (path / 'session.log').exists():
                        error = error or '未能收集运行日志'
                record = {'runID': run_id, 'configuration_name': variant['name'], 'repeat': repeat, 'request': request,
                          'result': result, 'metrics': measure(result or {}), 'error': error,
                          'initial_data_sha256': initial_data}
                if args.platform == 'macos':
                    record['data_directory'] = str(run_data)
                log = path / 'session.log'
                if log.exists():
                    contents = log.read_text(errors='replace')
                    record['components'] = log_summary(contents)
                    missing = missing_displayed_packages(record['components'], args.require_displayed_package)
                    if missing:
                        error = error or '缺少应用首帧：' + ', '.join(missing)
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
