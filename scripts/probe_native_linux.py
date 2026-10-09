#!/usr/bin/env python3
"""构建并运行 N0 原型，同时编译使用同一实现的 iOS 静态库。"""
import hashlib
import json
import platform
from pathlib import Path
import shutil
import statistics
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "experiments/native-linux"
OUT = ROOT / "work/native-linux-nce"


def run(args, **kwargs):
    return subprocess.run([str(arg) for arg in args], check=True, timeout=120, **kwargs)


def main():
    if platform.system() != "Darwin" or platform.machine() != "arm64":
        raise SystemExit("Requires an Apple Silicon Mac")
    OUT.mkdir(parents=True, exist_ok=True)
    clang = subprocess.check_output(["xcrun", "--find", "clang"], text=True).strip()
    linker = shutil.which("ld.lld")
    if not linker:
        raise SystemExit("ld.lld is required for the independent ELF fixture")
    run([clang, "--target=aarch64-none-elf", "-c", SOURCE / "guest.S", "-o", OUT / "guest.o"])
    (OUT / "guest.ld").write_text("SECTIONS { . = 0; .text : { *(.text) } /DISCARD/ : { *(.comment) } }\n")
    run([linker, "-T", OUT / "guest.ld", "--entry=guest_work", OUT / "guest.o", "-o", OUT / "guest.elf"])
    # Read only the sections required from our own fixed ELF64 fixture.
    elf = (OUT / "guest.elf").read_bytes()
    shoff = struct.unpack_from("<Q", elf, 40)[0]
    shsize, shnum, shstr = struct.unpack_from("<HHH", elf, 58)
    sections = [struct.unpack_from("<IIQQQQIIQQ", elf, shoff + i * shsize) for i in range(shnum)]
    names = elf[sections[shstr][4]:sections[shstr][4] + sections[shstr][5]]

    def name(table, offset):
        return table[offset:table.index(b"\0", offset)].decode()

    text_section = next(s for s in sections if name(names, s[0]) == ".text")
    payload = elf[text_section[4]:text_section[4] + text_section[5]]
    symtab = next(s for s in sections if s[1] == 2)
    strings = sections[symtab[6]]
    strings = elf[strings[4]:strings[4] + strings[5]]
    symbols = {}
    for pos in range(symtab[4], symtab[4] + symtab[5], symtab[9]):
        key, _, _, _, value, _ = struct.unpack_from("<IBBHQQ", elf, pos)
        symbol = name(strings, key)
        if symbol.startswith("guest_"):
            symbols[symbol.upper()] = value - text_section[3]
    header = "static const unsigned char guest_image[] = {\n" + ",".join(map(str, payload)) + "\n};\n"
    header += "".join(f"#define {key} {value}\n" for key, value in symbols.items())
    (OUT / "guest-image.h").write_text(header)
    (OUT / "guest-source.sha256").write_text(hashlib.sha256((SOURCE / "guest.S").read_bytes()).hexdigest() + "\n")
    common = ["-O2", "-g", "-Wall", "-Wextra", "-Werror", "-I", SOURCE, "-I", OUT]
    mac_sdk = subprocess.check_output(["xcrun", "--sdk", "macosx", "--show-sdk-path"], text=True).strip()
    mac_flags = [*common, "-isysroot", mac_sdk]
    run([clang, *mac_flags, "-DNATIVE", "-c", SOURCE / "guest.S", "-o", OUT / "native.o"])
    files = [SOURCE / s for s in ("main.c", "nce.c", "probe.c", "entry.S")]
    run([clang, *mac_flags, *files, OUT / "native.o", "-o", OUT / "nce-probe"])
    with (OUT / "results.jsonl").open("w") as output:
        run([OUT / "nce-probe"], stdout=output)
    records = [json.loads(line) for line in (OUT / "results.jsonl").read_text().splitlines()]
    if records[-1] != {"result": "passed"}:
        raise SystemExit("Probe did not complete")
    sdk = subprocess.check_output(["xcrun", "--sdk", "iphoneos", "--show-sdk-path"], text=True).strip()
    ios = []
    for source in ("nce.c", "probe.c", "entry.S", "guest.S"):
        obj = OUT / f"ios-{source}.o"
        run([clang, *common, "-target", "arm64-apple-ios17.0", "-isysroot", sdk,
             *(["-DNATIVE"] if source == "guest.S" else []), "-c", SOURCE / source, "-o", obj])
        ios.append(obj)
    run(["xcrun", "libtool", "-static", "-o", OUT / "libnce-probe-ios.a", *ios])
    summary = {"platform": platform.platform(), "kernel_integrated": False,
               "ios_status": "compiled; device execution pending", "benchmarks": []}
    for interval in (0, 4096, 256, 16, 1):
        rows = [r for r in records if r.get("bench") == "native_vs_nce" and r["interval"] == interval]
        native = statistics.median(r["native_ns"] for r in rows)
        nce = statistics.median(r["nce_ns"] for r in rows)
        calls = rows[0]["calls"] - 1
        summary["benchmarks"].append({"syscall_interval": interval, "native_ms": native / 1e6,
                                      "nce_ms": nce / 1e6, "nce_over_native": nce / native,
                                      "extra_ns_per_call": (nce - native) / calls if calls else None})
    inputs = [SOURCE / p for p in ("entry.S", "guest.S", "main.c", "nce.c", "nce.h", "guest-registers.h", "probe.c", "probe.h")]
    inputs.append(Path(__file__).resolve())
    summary["input_sha256"] = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                               for p in inputs}
    (OUT / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
