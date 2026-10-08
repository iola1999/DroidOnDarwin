#!/usr/bin/env python3
"""将批量内存实验库加入指定客体的 64 位 zygote 与 SurfaceFlinger。"""
import argparse
from pathlib import Path
import subprocess
import tempfile

LIBRARY = "/system/lib64/libhusk_bulk.so"
ENTRY = f"    setenv LD_PRELOAD {LIBRARY}\n"
CONFIGS = {"/system/etc/init/hw/init.zygote64.rc": "zygote",
           "/system/etc/init/surfaceflinger.rc": "surfaceflinger"}
CONTROL = "/vendor/etc/init/husk-bulk.rc"
CONTENT = ("# Husk bulk-memory experiment.\non early-init\n"
           "    chmod 0444 /sys/firmware/acpi/tables/HUSK\n")


def edit_service(content, service, remove):
    lines = content.splitlines(keepends=True)
    found = False
    active = False
    result = []
    for line in lines:
        if line.strip() and not line[0].isspace() and not line.startswith("#"):
            active = line.startswith(f"service {service} ")
            if active:
                if found: raise ValueError(f"Duplicate service {service}")
                found = True
                result.append(line)
                if not remove: result.append(ENTRY)
                continue
        if active and line == ENTRY: continue
        if active and line.strip().startswith("setenv LD_PRELOAD "):
            raise ValueError(f"Service {service} already has a different LD_PRELOAD")
        result.append(line)
    if not found: raise ValueError(f"Missing service {service}")
    return "".join(result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--library", type=Path)
    parser.add_argument("--remove", action="store_true")
    args = parser.parse_args()
    if not args.remove and (not args.library or not args.library.is_file()):
        parser.error("--library is required when installing")
    adb = ["adb", "-s", args.serial]

    def run(*command):
        result = subprocess.run([*adb, *command], capture_output=True, text=True, timeout=120)
        if result.returncode: raise RuntimeError(result.stdout + result.stderr)
        return result.stdout

    if run("shell", "id -u").strip() != "0":
        run("root"); run("wait-for-device")
    if run("shell", "id -u").strip() != "0": raise RuntimeError("ADB root unavailable")
    existing = run("shell", f"if [ -f {CONTROL} ]; then cat {CONTROL}; fi")
    if existing and existing != CONTENT: raise RuntimeError("Unknown bulk init configuration")
    updates = {target: edit_service(run("shell", f"cat {target}"), service, args.remove)
               for target, service in CONFIGS.items()}
    if not args.remove: updates[CONTROL] = CONTENT
    # This image mounts its system partition at /. Use the actual mount entry.
    mounts = [line.split() for line in run("shell", "cat /proc/mounts").splitlines()]
    system_mount = "/system" if any(row[1] == "/system" for row in mounts) else "/"
    run("shell", f"mount -o remount,rw {system_mount} && mount -o remount,rw /vendor")
    if not args.remove:
        run("push", str(args.library.resolve()), "/data/local/tmp/libhusk_bulk.so")
        # Existing processes may still map the previous inode until cold boot.
        run("shell", f"cat /data/local/tmp/libhusk_bulk.so > {LIBRARY}.new && "
            f"chmod 0644 {LIBRARY}.new && mv -f {LIBRARY}.new {LIBRARY} && "
            f"restorecon {LIBRARY} && rm /data/local/tmp/libhusk_bulk.so")
    with tempfile.TemporaryDirectory(prefix="husk-bulk-") as directory:
        for index, (target, content) in enumerate(updates.items()):
            local = Path(directory) / f"{index}.rc"
            local.write_text(content)
            run("push", str(local), "/data/local/tmp/husk-bulk-install.rc")
            run("shell", f"cat /data/local/tmp/husk-bulk-install.rc > {target} && "
                f"chmod 0644 {target} && restorecon {target}")
            if run("shell", f"cat {target}") != content: raise RuntimeError(f"Readback failed: {target}")
    run("shell", "rm /data/local/tmp/husk-bulk-install.rc")
    if args.remove: run("shell", f"rm -f {CONTROL} {LIBRARY}")
    else: run("shell", "if [ -f /sys/firmware/acpi/tables/HUSK ]; then chmod 0444 /sys/firmware/acpi/tables/HUSK; fi")
    run("shell", "sync")
    print("Removed bulk service configuration" if args.remove else "Installed bulk service configuration; cold boot required")


if __name__ == "__main__":
    main()
