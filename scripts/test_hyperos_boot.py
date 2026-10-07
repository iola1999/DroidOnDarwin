#!/usr/bin/env python3
"""复用 Mac 实验磁盘，以可见窗口验证 HyperOS 启动阶段并记录宿主耗时。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import time
from build_arm64_exec import verify_qemu_build

ROOT = Path(__file__).resolve().parents[1]


def qmp(port, command, arguments=None):
    with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
        stream = sock.makefile("rwb")
        json.loads(stream.readline())
        for ident, name in enumerate(("qmp_capabilities", command)):
            request = {"execute": name, "id": ident}
            if ident and arguments:
                request["arguments"] = arguments
            stream.write((json.dumps(request) + "\n").encode())
            stream.flush()
            while True:
                response = json.loads(stream.readline())
                if response.get("id") == ident:
                    if "error" in response:
                        raise RuntimeError(response["error"])
                    break


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=("tcg", "direct"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--seconds", type=int, default=180)
    parser.add_argument("--stage", choices=("policy", "boot"), default="policy")
    parser.add_argument("--stats", action="store_true")
    parser.add_argument("--cpu", help="Override the guest CPU for an explicit feature comparison")
    args = parser.parse_args()
    verify_qemu_build()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    command = json.loads((ROOT / "work/hyperos3-phone-profile/command.json").read_text())
    replacements = {
        "-name": "HyperOS " + args.mode,
        "-accel": "tcg,thread=single,tb-size=256", "-smp": "1",
        "-serial": f"file:{output}/serial.log", "-pidfile": str(output / "qemu.pid"),
        "-qmp": "tcp:127.0.0.1:16257,server=on,wait=off",
    }
    for option, value in replacements.items():
        command[command.index(option) + 1] = value
    if args.cpu:
        command[command.index("-cpu") + 1] = args.cpu
    for i, item in enumerate(command):
        if "unit=0," in item and item.startswith("if=pflash"):
            command[i] = "if=pflash,unit=0,format=raw,readonly=on,file=" + str(
                ROOT / "work/performance/build-inputs/upstream/Payload/Husk.app/edk2-aarch64-code.fd")
    env = dict(os.environ, HUSK_ARM64_EXEC="1" if args.mode == "direct" else "0", HUSK_ARM64_BACKEND="direct")
    if args.stats:
        env["HUSK_ARM64_STATS"] = "1"
    (output / "command.json").write_text(json.dumps(command, indent=2) + "\n")
    result = {"mode": args.mode, "stage": args.stage, "passed": False,
              "backend_env": {k: v for k, v in env.items() if k.startswith("HUSK_ARM64_")},
              "qemu_sha256": hashlib.sha256(Path(command[0]).read_bytes()).hexdigest(), "events": {}}
    patterns = ("Compiling SELinux policy", "Loading SELinux policy", "starting service 'bootanim'",
                "sys.boot_completed=1", "HUSK-SETUP")
    start = time.monotonic()
    next_guest_query = start
    with (output / "host.log").open("w") as log:
        process = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            while process.poll() is None and time.monotonic() - start < args.seconds:
                serial = output / "serial.log"
                text = serial.read_text(errors="replace") if serial.exists() else ""
                for pattern in patterns:
                    if pattern in text and pattern not in result["events"]:
                        result["events"][pattern] = round(time.monotonic() - start, 3)
                        print(pattern, result["events"][pattern], flush=True)
                if args.stage == "policy" and "Loading SELinux policy" in text:
                    result["passed"] = True
                    break
                if args.stage == "boot" and time.monotonic() >= next_guest_query:
                    next_guest_query = time.monotonic() + 5
                    try:
                        with socket.create_connection(("127.0.0.1", 16256), timeout=2) as guest:
                            guest.sendall(b"getprop sys.boot_completed; exit\n")
                            if b"1" == guest.recv(128).strip():
                                result["passed"] = True
                                result["events"]["boot_completed_query"] = round(time.monotonic() - start, 3)
                                break
                    except OSError:
                        pass
                time.sleep(0.25)
        finally:
            result["elapsed_seconds"] = round(time.monotonic() - start, 3)
            if process.poll() is None:
                if args.stage == "boot" and not result["passed"]:
                    with (output / "sample.txt").open("w") as sample:
                        try:
                            subprocess.run(["sample", str(process.pid), "3", "10"],
                                           stdout=sample, stderr=subprocess.STDOUT, timeout=15)
                        except (OSError, subprocess.TimeoutExpired) as error:
                            result["sample_error"] = str(error)
                try:
                    if args.stage == "boot":
                        qmp(16257, "screendump", {"filename": str(output / "screen.ppm")})
                    qmp(16257, "quit")
                    process.wait(timeout=5)
                except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired):
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
            result["exit_code"] = process.returncode
            (output / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result), flush=True)
    if not result["passed"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
