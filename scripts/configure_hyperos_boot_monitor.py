#!/usr/bin/env python3
"""配置已核对版本的 HyperOS BootMonitor，防止慢速模拟触发恢复模式。"""
import argparse
import subprocess


EXPECTED_SHA256 = "9ada5c44226cdfe0100ac52b33b7564e42c7a66c033d0427eaa5a8307d8ccb4d"
CONTROL = "/cache/boot_flag"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    parser.add_argument("mode", choices=("disable", "enable"))
    args = parser.parse_args()

    def shell(command):
        result = subprocess.run(["adb", "-s", args.serial, "shell", command],
                                capture_output=True, text=True, timeout=30)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        return result.stdout.strip()

    if shell("id -u") != "0":
        raise RuntimeError("ADB root is required")
    digest = shell("sha256sum /system_ext/bin/hypsys_system").split()[0]
    if digest != EXPECTED_SHA256:
        raise RuntimeError("BootMonitor binary differs from the reviewed version")
    if shell("getprop ro.build.ab_update") != "false":
        raise RuntimeError("This configuration requires the cache-partition BootMonitor path")
    current = shell(f"if [ -f {CONTROL} ]; then cat {CONTROL}; fi")
    if current and current != "disable" and not current.isdecimal():
        raise RuntimeError(f"Unrecognized {CONTROL} content")
    value = "disable" if args.mode == "disable" else "0"
    shell(f"printf '%s\\n' '{value}' > {CONTROL} && chmod 0644 {CONTROL} && sync")
    if shell(f"cat {CONTROL}") != value:
        raise RuntimeError("BootMonitor control readback failed")
    print(f"BootMonitor {args.mode}: {CONTROL}={value}; applies from the next start")


if __name__ == "__main__":
    main()
