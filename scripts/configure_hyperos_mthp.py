#!/usr/bin/env python3
"""为指定 HyperOS 测试客体设置 16 KiB mTHP，并写入早期启动配置。"""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--policy", choices=("always", "never"), required=True)
    args = parser.parse_args()
    adb = ["adb", "-s", args.serial]

    def run(*command):
        result = subprocess.run([*adb, *command], capture_output=True, text=True, timeout=120)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        return result.stdout

    if run("shell", "id -u").strip() != "0":
        run("root")
        run("wait-for-device")
    if run("shell", "id -u").strip() != "0":
        raise RuntimeError("ADB root unavailable")
    control = "/sys/kernel/mm/transparent_hugepage/hugepages-16kB/enabled"
    before = run("shell", f"cat {control}").strip()
    if "always" not in before or "never" not in before:
        raise RuntimeError("Guest kernel lacks the expected 16 KiB mTHP control")
    target = "/vendor/etc/init/husk-memory.rc"
    content = ("# Husk experimental memory allocation policy.\n"
               "on early-init\n"
               f"    write {control} {args.policy}\n")
    existing = run("shell", f"if [ -f {target} ]; then cat {target}; fi")
    if existing and not existing.startswith("# Husk experimental memory allocation policy.\n"):
        raise RuntimeError("Existing vendor init file has an unknown owner")
    with tempfile.TemporaryDirectory(prefix="husk-mthp-") as temporary:
        path = Path(temporary) / "husk-memory.rc"
        path.write_text(content)
        run("push", str(path), "/data/local/tmp/husk-memory.rc")
        run("shell", f"mount -o remount,rw /vendor && "
            f"cat /data/local/tmp/husk-memory.rc > {target} && "
            f"chmod 0644 {target} && restorecon {target} && "
            f"echo {args.policy} > {control} && sync")
        run("shell", "rm /data/local/tmp/husk-memory.rc")
    if run("shell", f"cat {target}") != content:
        raise RuntimeError("Vendor init readback differs from the requested policy")
    after = run("shell", f"cat {control}").strip()
    if f"[{args.policy}]" not in after:
        raise RuntimeError("Guest kernel did not accept the mTHP policy")
    print(f"16 KiB mTHP: {before} -> {after}")
    print(f"Installed {target}; cold boot is needed to check early allocation behavior.")


if __name__ == "__main__":
    main()
