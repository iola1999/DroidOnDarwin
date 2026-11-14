#!/usr/bin/env python3
"""将固定 Android ashmem C 驱动复制到 LKL，并记录实际编译输入。"""
import hashlib
import json
from pathlib import Path
import shlex
import subprocess

VENDOR = Path(__file__).resolve().parents[1] / 'third_party/android-ashmem'
SOURCE_FILES = ('ashmem.c', 'ashmem.h', 'uapi/ashmem.h')
GENERATED_FILES = (*('drivers/android/' + name for name in SOURCE_FILES),
                   'drivers/android/Kconfig', 'drivers/android/Makefile')


def input_files():
    metadata = json.loads((VENDOR / 'sources.json').read_text())
    return [VENDOR / 'sources.json', *[VENDOR / name for name in metadata['files']]]


def apply(source: Path):
    metadata = json.loads((VENDOR / 'sources.json').read_text())
    contents = {}
    for name, expected in metadata['files'].items():
        data = (VENDOR / name).read_bytes()
        if len(data) != expected['bytes'] or hashlib.sha256(data).hexdigest() != expected['sha256']:
            raise RuntimeError('Android ashmem 原始文件摘要不一致：' + name)
        contents[name] = data
    text = contents['ashmem.c'].decode()
    # LKL 固定使用实际页大小；Android 的动态页仿真依赖 X86_64。
    for before, after in (('#include <linux/page_size_compat.h>\n', ''), ('__PAGE_ALIGN(', 'PAGE_ALIGN(')):
        if text.count(before) != 1:
            raise RuntimeError('Android ashmem 适配位置缺失或重复：' + before)
        text = text.replace(before, after)
    contents['ashmem.c'] = text.encode()
    if hashlib.sha256(contents['ashmem.c']).hexdigest() != metadata['adapted_c_sha256']:
        raise RuntimeError('Android ashmem 生成源码摘要不一致')
    kconfig = source / 'drivers/android/Kconfig'
    config = '''config ASHMEM
	bool "Enable the Anonymous Shared Memory Subsystem"
	depends on SHMEM
	help
	  The ashmem subsystem is a new shared memory allocator, similar to
	  POSIX SHM but with different behavior and sporting a simpler
	  file-based API.

	  It is, in theory, a good memory allocator for low-memory devices,
	  because it can discard shared memory units when under memory pressure.

'''
    text = kconfig.read_text()
    if text.count(config) > 1:
        raise RuntimeError('LKL Android Kconfig 的 ashmem 配置重复')
    if config not in text:
        if 'config ASHMEM\n' in text or text.count('endmenu\n') != 1:
            raise RuntimeError('LKL Android Kconfig 的 ashmem 插入位置不一致')
        kconfig.write_text(text.replace('endmenu\n', config + 'endmenu\n'))
    makefile = source / 'drivers/android/Makefile'
    text = makefile.read_text()
    rule = 'obj-$(CONFIG_ASHMEM) += ashmem.o\n'
    if text.count(rule) > 1:
        raise RuntimeError('LKL Android Makefile 的 ashmem 规则重复')
    if rule not in text:
        if 'ashmem.o' in text:
            raise RuntimeError('LKL Android Makefile 已有不同的 ashmem 规则')
        makefile.write_text(text + '\n' + rule)
    for name in SOURCE_FILES:
        destination = source / 'drivers/android' / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        if not destination.exists() or destination.read_bytes() != contents[name]:
            destination.write_bytes(contents[name])


def compiled_inputs(build: Path):
    command_file = build / 'drivers/android/.ashmem.o.cmd'
    command = shlex.split(command_file.read_text().splitlines()[0].split(' := ', 1)[1])
    if command.count('-c') != 1 or command.count('-o') != 1:
        raise RuntimeError('ashmem 实际编译命令缺少唯一的对象输出')
    output_index = command.index('-o')
    del command[output_index:output_index + 2]
    command.remove('-c')
    command = [value for value in command if not value.startswith('-Wp,-MMD,')]
    dependency_file = build / 'drivers/android/ashmem-inputs.d'
    command += ['-M', '-MF', str(dependency_file), '-MT', 'drivers/android/ashmem.o']
    subprocess.run(command, check=True, cwd=build, timeout=120)
    names = shlex.split(dependency_file.read_text().replace('\\\n', ' ').split(':', 1)[1])
    files = [(build / name).resolve(strict=True) for name in names]
    return command, [command_file, build / 'drivers/android/ashmem.o', dependency_file, *files]
