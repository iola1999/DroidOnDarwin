"""使用 Android SDK 自带的 dexlib2 生成接口调用回归。"""
import subprocess
from pathlib import Path


def prepare_interface_object_calls(path):
    root = Path(__file__).resolve().parents[1]
    java = Path('/opt/homebrew/opt/openjdk@17/bin')
    libraries = Path('/opt/homebrew/share/android-commandlinetools/cmdline-tools/latest/lib/external')
    jars = [libraries / name for name in (
        'com/android/tools/smali/smali-dexlib2/3.0.9/smali-dexlib2-3.0.9.jar',
        'com/google/guava/guava/33.3.1-jre/guava-33.3.1-jre.jar',
        'com/google/code/findbugs/jsr305/3.0.2/jsr305-3.0.2.jar')]
    if any(not jar.is_file() for jar in jars):
        raise RuntimeError('Missing Android SDK dexlib2 regression dependencies')
    output = path.parent.parent / 'test-dex-tool'
    output.mkdir(exist_ok=True)
    classpath = ':'.join(map(str, jars))
    subprocess.run([java / 'javac', '-cp', classpath, '-d', output,
                    root / 'experiments/native-linux/art/NceTestDex.java'], check=True, timeout=60)
    subprocess.run([java / 'java', '-cp', str(output) + ':' + classpath, 'NceTestDex', path], check=True, timeout=60)
