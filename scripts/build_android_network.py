#!/usr/bin/env python3
"""构建使用 LKL BPF 解释器的 Android 网络程序加载器。"""
import base64
import hashlib
import json
from pathlib import Path
import subprocess
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
ANDROID = ROOT / 'work/native-linux-nce/android'
SOURCE = ANDROID / 'network-source'


def build():
    manifest = ROOT / 'experiments/native-linux/services/network-sources.json'
    for item in json.loads(manifest.read_text())['files']:
        path = SOURCE / item['path']
        path.parent.mkdir(parents=True, exist_ok=True)
        if not path.exists():
            path.write_bytes(base64.b64decode(urllib.request.urlopen(item['url'], timeout=30).read()))
        if hashlib.sha256(path.read_bytes()).hexdigest() != item['sha256']:
            raise RuntimeError('Network source checksum mismatch: ' + item['path'])
    out = ANDROID / 'network-build'
    out.mkdir(exist_ok=True)
    public = out / 'include/cutils/android_filesystem_config.h'
    public.parent.mkdir(parents=True, exist_ok=True)
    public.unlink(missing_ok=True)
    public.symlink_to(ANDROID / 'core-include/private/android_filesystem_config.h')
    source = (SOURCE / 'bpf/loader/NetBpfLoad.cpp').read_text()
    original = 'if (isAtLeastU) {\n        // Note: writing these proc files requires CAP_NET_ADMIN'
    if source.count(original) != 1:
        raise RuntimeError('BPF JIT configuration block changed')
    # LKL executes BPF with the kernel interpreter. Program verification,
    # map creation, attachment and load failures retain upstream behavior.
    source = source.replace(original,
        'if (isAtLeastU && GetProperty("ro.nce.prototype", "") != "1") {\n'
        '        // Note: writing these proc files requires CAP_NET_ADMIN')
    original = 'if (!jitLen && bpfloader_ver >= BPFLOADER_MAINLINE_25Q2_VERSION) {'
    if source.count(original) != 1:
        raise RuntimeError('BPF JIT result check changed')
    source = source.replace(original,
        'if (!jitLen && bpfloader_ver >= BPFLOADER_MAINLINE_25Q2_VERSION && '
        'GetProperty("ro.nce.prototype", "") != "1") {')
    patched = out / 'NetBpfLoad.cpp'
    patched.write_text(source)
    tool = Path('/opt/homebrew/share/android-ndk/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android35-clang++')
    args = [tool, '-std=c++20', '-O2', '-g', '-ffixed-x18', '-mno-outline-atomics',
            '-nostdlib++', '-include', ANDROID / 'art-build/platform-libcxx-config.h',
            '-Wl,-z,max-page-size=16384,-z,common-page-size=16384']
    for path in (SOURCE / 'libbpf', SOURCE / 'bpf/headers/include',
                 SOURCE / 'bpf/syscall_wrappers/include', ANDROID / 'libbase/include',
                 ANDROID / 'core-include', ANDROID / 'liblog/liblog/include',
                 out / 'include'):
        args += ['-I', path]
    args += [patched, ANDROID / 'android36/lib64/libbase.so',
             ANDROID / 'android36/lib64/libc++.so',
             ANDROID / 'android36/com.android.tethering/lib64/libbpf.so',
             ANDROID / 'android36/lib64/liblog.so', '-o', out / 'netbpfload']
    subprocess.run([str(a) for a in args], check=True, timeout=120)
    return out / 'netbpfload'


if __name__ == '__main__':
    print(build())
