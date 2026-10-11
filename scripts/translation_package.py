"""完整应用使用的翻译构建校验、资源清单和运行格式。"""
import hashlib
import json
from pathlib import Path
import subprocess

from build_translation_runtime import build as build_runtime

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / 'work/native-linux-nce'


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def prepare(report_path, out, target):
    report_path = report_path.resolve()
    report = json.loads(report_path.read_text())
    config = report['configuration']
    if config['workload'] != 'desktop' or config['execution'] != 'direct' or config['code_memory'] != 'dual':
        raise ValueError('翻译应用需要双映射 Direct 完整桌面验证')
    if config.get('online_cpus', config['cpus']) != config['cpus']:
        raise ValueError('翻译应用默认启用全部 CPU，需要对应的完整桌面验证')
    required = {'android_system_boot_completed', 'android_application_data_persistence', 'android_keystore_persistence',
                'android_system_process_groups_stopped', 'linux_data_readonly_shutdown', 'android_clock_controls_ready',
                'translation_prepared_code_execution', 'translated_context_cleanup'}
    for entry in ('cli', 'gcd'):
        runs = [run for run in report['runs'] if run['entry'] == entry]
        if not runs or any(not run['validated'] or run['returncode'] or not run.get('data_image', {}).get('clean_shutdown') for run in runs):
            raise ValueError('完整桌面检查缺失或失败：' + entry)
        for run in runs:
            rows = run['results']
            passed = {row.get('test') for row in rows if row.get('passed')}
            visible = {row.get('package') for row in rows if row.get('test') == 'android_desktop_visible_activity' and row.get('passed')}
            if required - passed or not {'com.android.launcher3', 'com.android.deskclock', 'com.google.android.documentsui'} <= visible:
                raise ValueError('完整桌面检查项不完整：' + entry)
    reused = [run for run in report['runs'] if not run['data_image']['fresh']]
    if not reused or any(not {'android_application_data_persistence', 'android_keystore_persistence'} <=
                         {row.get('test') for row in run['results'] if row.get('passed') and row.get('restored')} for run in reused):
        raise ValueError('完整应用需要同一数据盘的跨启动持久化检查')
    verified = {ROOT / name: value for name, value in report['sha256'].items()}
    verified[report_path] = digest(report_path)
    for path, expected in verified.items():
        if digest(path) != expected:
            raise ValueError('本机验证输入变化：' + str(path))
    build = build_runtime(report_path, out / 'runtime', target)
    verified.update({ROOT / name: value for name, value in build['sha256'].items()})
    verified[Path(build['library'])] = build['library_sha256']
    verified[Path(__file__)] = digest(Path(__file__))
    kernel = next(path for path in verified if path.name == 'lkl-image.bin')
    fixtures = report_path.parent
    names = ['sigreturn.elf', 'guest.elf', 'late.elf', 'dynamic.elf', 'dash.elf', 'bionic-test.elf', 'libc.so',
             'dependency.so', 'plugin.so', 'nce-policy.cil', 'property_info', 'ld.config.txt', 'NceWorkload.dex', 'enabled']
    resources = {name: fixtures / name for name in names}
    for path in resources.values():
        if path not in verified:
            raise ValueError('资源未包含在验证记录中：' + str(path))
    files = {'lkl-image.bin': kernel, 'system.img': WORK / 'processes/system.img',
             'android-data-seed.img': ROOT / report['data_seed']['path']}
    seed = digest(files['android-data-seed.img'])
    if any(run.get('data_image', {}).get('fresh') and run['data_image']['initial_sha256'] != seed for run in report['runs']):
        raise ValueError('数据盘初始镜像与桌面检查不一致')
    for path in files.values():
        current = digest(path)
        if path in verified and verified[path] != current:
            raise ValueError('打包资源在编译期间变化：' + str(path))
        verified[path] = current
    identity = {**{name: digest(path) for name, path in files.items()},
                **{'Translation/' + name: digest(path) for name, path in resources.items()}}
    metadata = {'runtime': hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest(),
                'execution': {'engine': 'direct', 'cpus': config['cpus'], 'code_memory': 'dual'},
                'build': {'kernel_sha256': identity['lkl-image.bin'], 'system_sha256': identity['system.img'],
                          'source_inputs_sha256': hashlib.sha256(json.dumps(build['sha256'], sort_keys=True).encode()).hexdigest()}}
    return {'configuration': config, 'library': Path(build['library']), 'verified': verified,
            'files': files, 'resources': resources, 'metadata': metadata}


def install_resources(prepared, destination):
    (destination / 'Translation').mkdir()
    for name, path in {**prepared['files'], **{'Translation/' + name: path for name, path in prepared['resources'].items()}}.items():
        subprocess.run(['cp', '-c', path, destination / name], check=True, timeout=120)
    (destination / 'Runtime.json').write_text(json.dumps(prepared['metadata'], indent=2) + '\n')
