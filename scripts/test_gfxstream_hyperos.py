#!/usr/bin/env python3
"""复用现有 HyperOS 磁盘，验证客体驱动并计时 GLES 绘制。窗口始终可见。"""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import statistics
import time

from build_arm64_exec import ROOT, verify_qemu_build
from test_hyperos_boot import qmp


def measure_settings(adb_run, output, metrics):
    def shell(command, seconds=120):
        response = adb_run("shell", command, seconds=seconds)
        if response.returncode:
            raise RuntimeError(response.stdout + response.stderr)
        return response.stdout

    shell("input keyevent KEYCODE_WAKEUP; wm dismiss-keyguard")
    shell("am force-stop com.android.settings")
    launch_start = time.monotonic()
    launch = shell("am start -W -a android.settings.SETTINGS", seconds=240)
    launch_seconds = time.monotonic() - launch_start
    (output / "settings-launch.txt").write_text(launch)
    metrics["launch_command_seconds"] = round(launch_seconds, 3)
    for label in ("TotalTime", "WaitTime"):
        value = re.search(rf"^{label}: (\d+)$", launch, re.MULTILINE)
        if value:
            metrics[label + "_ms"] = int(value[1])
    if "Status: ok" not in launch:
        raise RuntimeError("Settings launch did not complete")
    qmp(16257, "screendump", {"filename": str(output / "settings-open.ppm")})
    time.sleep(5)
    shell("dumpsys -t 60 gfxinfo com.android.settings reset")
    start = time.monotonic()
    for index in range(6):
        bottom, top = (650, 220) if index % 2 == 0 else (220, 650)
        shell(f"input swipe 180 {bottom} 180 {top} 2000")
        time.sleep(1)
    action_seconds = time.monotonic() - start
    metrics["swipe_action_seconds"] = round(action_seconds, 3)
    time.sleep(3)
    frames = shell("dumpsys -t 60 gfxinfo com.android.settings framestats")
    # Sample the same guest clock after the dump. Renderer-provided completion
    # times outside this interval cannot describe a frame in this capture.
    guest_uptime_ns = int(float(shell("cat /proc/uptime").split()[0]) * 1e9)
    (output / "settings-frames.txt").write_text(frames)
    durations = []
    invalid_frames = 0
    columns = None
    for row in csv.reader(frames.splitlines()):
        if row and row[0] == "Flags":
            columns = row
        elif columns and row and row[0].isdigit() and len(row) == len(columns):
            frame = dict(zip(columns, row))
            if int(frame["Flags"]) != 0:
                continue
            begin, end = int(frame["IntendedVsync"]), int(frame["FrameCompleted"])
            if 0 < begin < end <= guest_uptime_ns:
                durations.append((end - begin) / 1e6)
            elif begin > 0 and end != (1 << 63) - 1:
                invalid_frames += 1
    if not durations:
        raise RuntimeError("Settings did not return valid frame timings")
    durations.sort()
    metrics.update(frame_count=len(durations), invalid_frame_count=invalid_frames,
                   frame_timing_valid=not invalid_frames, guest_uptime_ns=guest_uptime_ns,
                   median_ms=statistics.median(durations),
                   p90_ms=durations[(len(durations) * 9 - 1) // 10], max_ms=durations[-1])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gpu", choices=("software", "gfxstream"), required=True)
    parser.add_argument("--accel", choices=("tcg", "hvf"), default="tcg")
    parser.add_argument("--cpu", help="Override the QEMU CPU model for feature comparisons")
    parser.add_argument("--execution", choices=("tcg", "direct"), default="tcg",
                        help="Select the CPU execution backend; Direct requires --accel tcg")
    parser.add_argument("--profile", action="store_true", help="Collect Direct statistics and CPU phase times")
    parser.add_argument("--perfmap", action="store_true", help="Record TCG host-to-guest instruction addresses")
    parser.add_argument("--angle-path", type=Path, required=True)
    parser.add_argument("--bench", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--seconds", type=int, default=900)
    parser.add_argument("--hold-seconds", type=int, default=0)
    parser.add_argument("--bench-runs", type=int, default=3)
    parser.add_argument("--bench-frames", type=int, default=60)
    parser.add_argument("--bench-seconds", type=int, default=180)
    parser.add_argument("--interaction", action="store_true",
                        help="Measure opening Settings and six list swipes before the GLES benchmark")
    parser.add_argument("--bulk-dir", type=Path, help="Run Android libc comparisons from build_guest_bulk.py")
    args = parser.parse_args()
    if args.execution == "direct" and args.accel != "tcg":
        parser.error("Direct requires --accel tcg")
    if (not 1 <= args.bench_frames <= 1000 or args.bench_runs < 0 or
        args.bench_seconds < 1 or (args.bench_runs == 0 and not args.interaction)):
        parser.error("frames must be 1..1000; runs=0 requires --interaction; seconds must be positive")
    verify_qemu_build()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    command = json.loads((ROOT / "work/hyperos3-phone-profile/command.json").read_text())
    changes = {"-name": f"HyperOS {args.gpu} {args.accel} {args.execution}", "-accel": args.accel,
        "-smp": "1", "-m": "4096", "-cpu": args.cpu or (
            "host" if args.accel == "hvf" else "max,pauth-impdef=on,sve=off,sme=off"),
        "-serial": f"file:{output}/serial.log", "-pidfile": str(output / "qemu.pid"),
        "-qmp": "tcp:127.0.0.1:16257,server=on,wait=off"}
    if args.accel == "tcg": changes["-accel"] = "tcg,thread=single,tb-size=256"
    for option, value in changes.items(): command[command.index(option) + 1] = value
    for i, item in enumerate(command):
        if "unit=0," in item and item.startswith("if=pflash"):
            command[i] = "if=pflash,unit=0,format=raw,readonly=on,file=" + str(
                ROOT / "work/performance/build-inputs/upstream/Payload/Husk.app/edk2-aarch64-code.fd")
        if item.startswith("virtio-gpu"):
            device = "virtio-gpu-rutabaga-pci,x-gfxstream-gles=on" if args.gpu == "gfxstream" else "virtio-gpu-pci"
            command[i] = device + ",xres=360,yres=800"
    if args.perfmap:
        command.append("-perfmap")
    env = dict(os.environ, HUSK_ARM64_EXEC="1" if args.execution == "direct" else "0",
               HUSK_ARM64_BACKEND="direct",
               ANGLE_DEFAULT_PLATFORM="metal",
               DYLD_LIBRARY_PATH=str(args.angle_path.resolve()))
    for name in ("HUSK_ARM64_STATS", "HUSK_ARM64_PROFILE"):
        if args.profile: env[name] = "1"
        else: env.pop(name, None)
    result = {"gpu": args.gpu, "accel": args.accel, "execution": args.execution, "passed": False,
              "cpu": changes["-cpu"],
              "backend_env": {k: v for k, v in env.items() if k.startswith("HUSK_ARM64_")},
              "bench_frames": args.bench_frames, "bench_limit_seconds": args.bench_seconds,
              "qemu_sha256": hashlib.sha256(Path(command[0]).read_bytes()).hexdigest(),
              "bench_sha256": hashlib.sha256(args.bench.read_bytes()).hexdigest()}
    graphics = [args.angle_path / name for name in ("libEGL.dylib", "libGLESv2.dylib")]
    graphics += [ROOT / "work/gfxstream-build-macos/prefix/lib" / name
                 for name in ("libgfxstream_backend.dylib", "librutabaga_gfx_ffi.dylib")]
    result["graphics_sha256"] = {str(p.resolve()): hashlib.sha256(p.read_bytes()).hexdigest()
                                  for p in graphics}
    (output / "command.json").write_text(json.dumps(command, indent=2) + "\n")
    adb = ["adb", "-s", "127.0.0.1:16255"]

    def adb_run(*options, seconds=15):
        return subprocess.run([*adb, *options], capture_output=True, text=True,
                              errors="replace", timeout=seconds)

    start = time.monotonic()
    subprocess.run(["adb", "disconnect", "127.0.0.1:16255"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5)
    with (output / "host.log").open("w") as log:
        process = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            while process.poll() is None and time.monotonic() - start < args.seconds:
                serial_path = output / "serial.log"
                serial_text = serial_path.read_text(errors="replace") if serial_path.exists() else ""
                if "Kernel panic - not syncing:" in serial_text:
                    raise RuntimeError("Guest kernel panic; inspect serial.log")
                if "Boot command: boot-recovery" in serial_text or "reboot: Restarting system with command 'recovery'" in serial_text:
                    raise RuntimeError("Guest entered Recovery; inspect serial.log for the reboot request")
                try:
                    # The guest command service is independent of adbd. Repeated
                    # ADB disconnects can interrupt its slow startup handshake.
                    with socket.create_connection(("127.0.0.1", 16256), timeout=3) as guest:
                        guest.settimeout(min(15, max(1, args.seconds - (time.monotonic() - start))))
                        guest.sendall(b"getprop sys.boot_completed; exit\n")
                        if guest.recv(128).strip() == b"1": break
                except OSError: pass
                time.sleep(3)
            else: raise RuntimeError("Could not confirm sys.boot_completed through the guest command service")
            result["boot_seconds"] = round(time.monotonic() - start, 3)
            result["boot_query_transport"] = "guest command service"
            print("boot_completed", result["boot_seconds"], flush=True)
            connect = subprocess.run(["adb", "connect", "127.0.0.1:16255"],
                                     capture_output=True, text=True, timeout=30)
            (output / "adb-connect.txt").write_text(connect.stdout + connect.stderr)
            ready = adb_run("wait-for-device", seconds=60)
            if ready.returncode: raise RuntimeError("ADB did not become ready: " + ready.stderr)
            for attempt in range(3):
                dump = adb_run("shell", "dumpsys -t 60 SurfaceFlinger", seconds=90)
                if not dump.returncode or not any(message in dump.stderr for message in ("closed", "offline")):
                    break
                adb_run("wait-for-device", seconds=60)
                time.sleep(1)
            (output / "surfaceflinger.txt").write_text(dump.stdout + dump.stderr)
            result["renderer"] = next((line.strip() for line in dump.stdout.splitlines() if "GLES:" in line), "")
            if dump.returncode or not result["renderer"]:
                raise RuntimeError(f"SurfaceFlinger query did not return a renderer (exit {dump.returncode})")
            if args.gpu == "gfxstream" and "ANGLE Metal Renderer" not in result["renderer"]:
                raise RuntimeError("SurfaceFlinger did not use the Metal renderer")
            if args.bulk_dir:
                result["bulk"] = []
                for name in ("libhusk_bulk.so", "husk-bulk-bench"):
                    local = args.bulk_dir / name
                    result.setdefault("bulk_sha256", {})[name] = hashlib.sha256(local.read_bytes()).hexdigest()
                    transfer = adb_run("push", str(local.resolve()), "/data/local/tmp/" + name)
                    if transfer.returncode: raise RuntimeError(transfer.stderr)
                for sample in range(3):
                    for mode in ("stock", "disabled", "enabled"):
                        prefix = "" if mode == "stock" else (
                            "LD_PRELOAD=/data/local/tmp/libhusk_bulk.so HUSK_BULK_STATS=1 "
                            f"HUSK_BULK_DISABLE={1 if mode == 'disabled' else 0} ")
                        run = adb_run("shell", "chmod 755 /data/local/tmp/husk-bulk-bench; " +
                                      prefix + "/data/local/tmp/husk-bulk-bench", seconds=300)
                        (output / f"bulk-{sample}-{mode}.log").write_text(run.stdout + run.stderr)
                        row = {"sample": sample, "mode": mode, "exit_code": run.returncode,
                               "passed": run.returncode == 0 and "PASS Android libc bulk" in run.stdout,
                               "enabled": "bulk_enabled=1" in run.stdout,
                               "mops": "mops=1" in run.stdout}
                        expected = (mode == "enabled" and args.accel == "tcg" and
                                    env.get("HUSK_ARM64_BULK") == "1" and not row["mops"])
                        row["passed"] &= row["enabled"] == expected
                        counts = re.search(r"completed=(\d+) refused=(\d+) transferred=(\d+)", run.stdout)
                        if counts:
                            row.update(completed=int(counts[1]), refused=int(counts[2]), transferred=int(counts[3]))
                        if expected: row["passed"] &= bool(counts and int(counts[1]) > 0)
                        row["measurements"] = [dict(size=int(m[1]), bytes=int(m[2]), copy_ns=int(m[3]), set_ns=int(m[4]))
                            for m in re.finditer(r"BENCH size=(\d+) bytes=(\d+) copy_ns=(\d+) set_ns=(\d+)", run.stdout)]
                        result["bulk"].append(row)
                        print("bulk", json.dumps(row), flush=True)
                        if not row["passed"]: raise RuntimeError("Android libc bulk validation failed")
            if args.interaction:
                # Keep completed phases when a later UI query times out.
                result["settings"] = {}
                measure_settings(adb_run, output, result["settings"])
                print("settings", json.dumps(result["settings"]), flush=True)
            push = adb_run("push", str(args.bench.resolve()), "/data/local/tmp/husk-gles-bench")
            if push.returncode: raise RuntimeError(push.stderr)
            result["passed"] = True
            result["bench_exit_codes"] = []
            for run in range(args.bench_runs):
                bench_log = output / f"bench-{run + 1}.log"
                try:
                    bench = adb_run("shell", "chmod 755 /data/local/tmp/husk-gles-bench; "
                                    f"/data/local/tmp/husk-gles-bench {args.bench_frames}",
                                    seconds=args.bench_seconds)
                except subprocess.TimeoutExpired as error:
                    # TimeoutExpired retains bytes even when subprocess text mode is enabled.
                    bench_log.write_bytes((error.stdout or b"") + (error.stderr or b""))
                    raise
                bench_log.write_text(bench.stdout + bench.stderr)
                result["bench_exit_codes"].append(bench.returncode)
                print(bench.stdout, flush=True)
                result["passed"] &= bench.returncode == 0 and "PASS frames=" in bench.stdout
                if args.gpu == "gfxstream":
                    result["passed"] &= "ANGLE Metal Renderer" in result["renderer"] and "ANGLE Metal Renderer" in bench.stdout
                if not result["passed"]: break
            if args.hold_seconds:
                print(f"holding visible VM for {args.hold_seconds} seconds", flush=True)
                until = time.monotonic() + args.hold_seconds
                while process.poll() is None and time.monotonic() < until: time.sleep(1)
            if process.poll() is not None:
                raise RuntimeError(f"QEMU exited before validation ended: {process.returncode}")
            if args.interaction and not result["settings"]["frame_timing_valid"]:
                raise RuntimeError("Settings returned invalid frame completion timestamps")
        except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
            result["error"] = str(error)
            result["passed"] = False
            if isinstance(error, subprocess.TimeoutExpired):
                (output / "failed-command.txt").write_bytes(
                    (error.stdout or b"") + (error.stderr or b""))
        finally:
            if process.poll() is not None:
                result["qemu_unexpected_exit"] = process.returncode
                result["passed"] = False
            if "boot_seconds" in result and process.poll() is None:
                try:
                    logcat = adb_run("logcat", "-d", "-t", "1500", seconds=15)
                    (output / "logcat.txt").write_text(logcat.stdout + logcat.stderr)
                except (OSError, subprocess.TimeoutExpired) as error:
                    result["logcat_error"] = str(error)
            if process.poll() is None:
                try:
                    qmp(16257, "screendump", {"filename": str(output / "screen.ppm")})
                    qmp(16257, "quit")
                    process.wait(timeout=10)
                except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired):
                    process.terminate()
                    try: process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
            result["elapsed_seconds"] = round(time.monotonic() - start, 3)
            perfmap = Path(f"/tmp/perf-{process.pid}.map")
            if args.perfmap and perfmap.exists():
                perfmap.rename(output / "tcg-perf.map")
            (output / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result), flush=True)
    if not result["passed"]: raise SystemExit(1)


if __name__ == "__main__":
    main()
