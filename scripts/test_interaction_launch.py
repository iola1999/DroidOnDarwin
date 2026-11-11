#!/usr/bin/env python3
"""在宿主上检查真实客体采集状态处理，文件和 Android 命令使用局部测试输入。"""
from pathlib import Path
import ctypes
import errno
import os
import struct
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'work/interaction-launch-tests'
(OUT / 'include/sys').mkdir(parents=True, exist_ok=True)
(OUT / 'include/sys/system_properties.h').write_text(
    '#pragma once\n#define PROP_VALUE_MAX 92\nextern "C" int __system_property_get(const char *, char *);\n')
subprocess.run(['xcrun', 'clang++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                '-I', str(OUT / 'include'), str(ROOT / 'tests/native-linux/interaction-launch.cpp'),
                '-o', str(OUT / 'probe')], check=True, timeout=60)
subprocess.run([str(OUT / 'probe')], check=True, timeout=15)

# Exercise the host's shipped request/export path through its existing syscall
# callback. No Android process or Linux kernel starts in this check.
subprocess.run(['xcrun', 'clang', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                '-dynamiclib', '-DNCE_DIRECT_EXECUTION',
                *[str(ROOT / 'experiments/native-linux' / name) for name in
                  ('session.c', 'session-profile.c', 'translation/interaction-profile.c')],
                '-o', str(OUT / 'host.dylib')], check=True, timeout=60)
library = ctypes.CDLL(str(OUT / 'host.dylib'))
callback_type = ctypes.CFUNCTYPE(ctypes.c_long, ctypes.c_long, ctypes.POINTER(ctypes.c_long))
library.nce_session_set_syscall.argtypes = [callback_type]
library.nce_session_profile.argtypes = [ctypes.c_int, ctypes.c_char_p]
library.nce_execution_profile_enabled.restype = ctypes.c_bool
with tempfile.TemporaryDirectory(prefix='host-', dir=OUT) as temporary:
    root = Path(temporary)
    (root / 'proc').mkdir()
    guest = root / 'data/nce'
    guest.mkdir(parents=True)
    statuses = {1: -errno.EIO}

    def guest_path(address):
        return root / ctypes.string_at(address).decode().lstrip('/')

    @callback_type
    def syscall(number, values):
        try:
            if number == 56:
                flags = os.O_WRONLY | os.O_CREAT | os.O_TRUNC if values[2] & 0x200 else os.O_RDONLY
                return os.open(guest_path(values[1]), flags, values[3])
            if number == 57:
                os.close(values[0])
            elif number == 61:
                return 0
            elif number == 63:
                data = os.read(values[0], values[2])
                ctypes.memmove(values[1], data, len(data))
                return len(data)
            elif number == 64:
                return os.write(values[0], ctypes.string_at(values[1], values[2]))
            elif number == 35:
                guest_path(values[1]).unlink()
            elif number == 38:
                source, target = guest_path(values[1]), guest_path(values[3])
                source.rename(target)
                command = struct.unpack('i', target.read_bytes())[0]
                (guest / 'interaction-response').write_bytes(struct.pack('i', statuses.get(command, 0)))
            elif number in (113, 114):
                instant = time.monotonic_ns() if number == 113 else 1
                data = struct.pack('qq', instant // 1_000_000_000, instant % 1_000_000_000)
                ctypes.memmove(values[1], data, len(data))
            else:
                return -errno.ENOSYS
            return 0
        except OSError as error:
            return -error.errno

    library.nce_session_set_syscall(syscall)
    output = root / 'output'
    output.mkdir()
    encoded = os.fsencode(output)
    (guest / 'interaction-launch.jsonl').write_text('{"phase":"prepared"}\n')
    assert library.nce_session_profile(5, encoded) == 0
    assert (output / 'launch.jsonl').read_text() == '{"phase":"prepared"}\n'
    assert library.nce_session_profile(3, encoded) == 0
    assert library.nce_execution_profile_enabled()
    artifacts = {'launch.jsonl': '{"phase":"failed"}\n', 'kernel.ftrace': 'failed trace\n',
                 'cpu0.txt': 'overrun: 0\n', 'clock.txt': '[mono]\n'}
    for name, text in artifacts.items():
        (guest / ('interaction-' + name)).write_text(text)
    assert library.nce_session_profile(1, encoded) == -errno.EIO
    assert not library.nce_execution_profile_enabled()
    for name, text in artifacts.items():
        assert (output / name).read_text() == text
    assert not (output / 'frames.txt').exists()
    assert (output / 'execution.json').is_file()
    for name in artifacts:
        (guest / ('interaction-' + name)).unlink()
    failed = root / 'prepare-failed'
    failed.mkdir()
    statuses[5] = -errno.EIO
    (guest / 'interaction-launch.jsonl').write_text('{"phase":"prepare_failed"}\n')
    assert library.nce_session_profile(5, os.fsencode(failed)) == -errno.EIO
    assert (failed / 'launch.jsonl').read_text() == '{"phase":"prepare_failed"}\n'
    assert not (failed / 'kernel.ftrace').exists()
print('host interaction checks passed: failed-open exports, original status and execution capture shutdown')
