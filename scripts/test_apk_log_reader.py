#!/usr/bin/env python3
"""在宿主 JVM 检查 APK 助手读取日志行后发生追加的边界。"""
from pathlib import Path
import subprocess

from build_apk_fixture import ANDROID_JAR, JAVA

ROOT = Path(__file__).resolve().parents[1]


def main():
    output = ROOT / 'work/native-linux-nce/apk-log-reader-test'
    classes = output / 'classes'
    classes.mkdir(parents=True, exist_ok=True)
    subprocess.run([
        JAVA / 'javac', '-cp', ANDROID_JAR, '-d', classes,
        ROOT / 'experiments/native-linux/services/NceApkInstall.java',
        ROOT / 'tests/native-linux/NceApkLogTest.java',
    ], check=True, timeout=30)
    subprocess.run([
        JAVA / 'java', '-cp', str(classes) + ':' + str(ANDROID_JAR),
        'NceApkLogTest', output / 'sample.log',
    ], check=True, timeout=15)


if __name__ == '__main__':
    main()
