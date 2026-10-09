#!/usr/bin/env python3
"""在 Darwin 原生执行 LKL，并从受控 ARM64 ELF 调用真实 Linux 文件与定时器接口。"""
import argparse
import hashlib
from lkl_nce_patch import apply
from build_nce_userspace import build as build_userspace
import json
import os
from pathlib import Path
import platform
import shlex
import shutil
import struct
import subprocess
import tarfile
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "work/native-linux-nce"
SOURCE = OUT / "lkl-source"
BUILD = OUT / "lkl-build"
EXPERIMENT = ROOT / "experiments/native-linux"
REVISION = "d0f76a77e8c2ca68bd7d96190b8da09040eab072"


def run(args, **kwargs):
    return subprocess.run([str(x) for x in args], check=True, timeout=300, **kwargs)


def package_elf(path):
    data = path.read_bytes()
    phoff, shoff = struct.unpack_from("<QQ", data, 32)
    phsize, phnum, shsize, shnum = struct.unpack_from("<HHHH", data, 54)
    segments = []
    for i in range(phnum):
        kind, flags, offset, va, _, filesz, memsz, _ = struct.unpack_from("<IIQQQQQQ", data, phoff + phsize * i)
        if kind == 1:
            segments.append((flags, offset, va, filesz, memsz))
    total = (max(va + memsz for _, _, va, _, memsz in segments) + 16383) & ~16383
    image = bytearray(total)
    protection = []
    for flags, offset, va, filesz, memsz in segments:
        image[va:va + filesz] = data[offset:offset + filesz]
        begin, end = va & ~16383, (va + memsz + 16383) & ~16383
        protection.append((begin, end - begin, ((flags & 4) >> 2) | (flags & 2) | ((flags & 1) << 2)))
    sections = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shsize) for i in range(shnum)]
    relocations = []
    for section in sections:
        if section[1] != 4:
            continue
        for pos in range(section[4], section[4] + section[5], section[9]):
            offset, info, addend = struct.unpack_from("<QQq", data, pos)
            if info != 1027 or not 0 <= offset <= total - 8 or not 0 <= addend < total:
                raise RuntimeError(f"Unsupported ELF relocation: {offset:x}, {info:x}, {addend:x}")
            relocations.append((offset, addend))
    symbols = {}
    symtab = next(s for s in sections if s[1] == 11)
    strings = sections[symtab[6]]
    strings = data[strings[4]:strings[4] + strings[5]]
    required = {"lkl_init", "lkl_syscall", "lkl_sys_halt", "lkl_cleanup", "nce_lkl_start"}
    if PROCESS:
        required |= {"nce_run_syscall", "nce_page_fault", "nce_set_layout", "nce_user_return", "nce_user_fault"}
    for pos in range(symtab[4], symtab[4] + symtab[5], symtab[9]):
        index, _, _, _, value, _ = struct.unpack_from("<IBBHQQ", data, pos)
        name = strings[index:strings.index(b"\0", index)].decode()
        if name in required:
            symbols[name] = value
    if symbols.keys() != required:
        raise RuntimeError(f"Missing exports: {required - symbols.keys()}")
    header = f"#define LKL_IMAGE_SIZE {total}\n"
    header += "".join(f"#define {name.upper()} {value}\n" for name, value in symbols.items())
    header += "static const unsigned long lkl_segments[][3] = {\n"
    header += "".join(f"{{{a},{b},{c}}},\n" for a, b, c in protection) + "};\n"
    header += "static const unsigned long lkl_relocations[][2] = {\n"
    header += "".join(f"{{{a},{b}}},\n" for a, b in relocations) + "};\n"
    (OUT / "lkl-image.h").write_text(header)
    (OUT / "lkl-image.bin").write_bytes(image)


def main():
    global OUT, BUILD, PROCESS
    started_ns = time.time_ns()
    parser = argparse.ArgumentParser()
    parser.add_argument("--processes", action="store_true")
    PROCESS = parser.parse_args().processes
    base = OUT
    if PROCESS:
        BUILD = base / "lkl-process-build"
        OUT = base / "processes"
        OUT.mkdir(exist_ok=True)
        for name in ("guest-image.h", "guest-source.sha256"):
            shutil.copyfile(base / name, OUT / name)
    if platform.system() != "Darwin" or platform.machine() != "arm64":
        raise SystemExit("Requires an Apple Silicon Mac")
    if not (OUT / "guest-image.h").exists():
        raise SystemExit("Run scripts/probe_native_linux.py first")
    if (OUT / "guest-source.sha256").read_text().strip() != hashlib.sha256((EXPERIMENT / "guest.S").read_bytes()).hexdigest():
        raise SystemExit("ELF fixture changed; run scripts/probe_native_linux.py first")
    for tool in ("gmake", "gsed", "ld.lld", "brew"):
        if not shutil.which(tool):
            raise SystemExit(f"Missing build tool: {tool}")
    if not SOURCE.exists():
        archive = OUT / "lkl.tar.gz"
        print("Downloading pinned Linux source", flush=True)
        urllib.request.urlretrieve(f"https://codeload.github.com/lkl/linux/tar.gz/{REVISION}", archive)
        with tarfile.open(archive) as tar:
            tar.extractall(OUT, filter="data")
        (OUT / f"linux-{REVISION}").rename(SOURCE)
        (SOURCE / ".husk-source-revision").write_text(REVISION + "\n")
        archive.unlink()
    if (SOURCE / ".husk-source-revision").read_text().strip() != REVISION:
        raise SystemExit("Unexpected LKL source revision")
    if PROCESS:
        apply(SOURCE, EXPERIMENT)
    # This host-tool type conflicts with Darwin's uuid_t from sys/types.h.
    mod = SOURCE / "scripts/mod/file2alias.c"
    text = mod.read_text()
    if "#define uuid_t lkl_mod_uuid_t" not in text:
        mod.write_text(text.replace("#include <stdbool.h>", "#include <stdbool.h>\n#ifdef __APPLE__\n#define uuid_t lkl_mod_uuid_t\n#endif"))
    include = OUT / "host-include"
    include.mkdir(exist_ok=True)
    if not (include / "elf.h").exists():
        urllib.request.urlretrieve("https://raw.githubusercontent.com/ifduyue/musl/v1.2.5/include/elf.h", include / "elf.h")
    (include / "byteswap.h").write_text("#define bswap_16 __builtin_bswap16\n#define bswap_32 __builtin_bswap32\n#define bswap_64 __builtin_bswap64\n")
    llvm = Path(subprocess.check_output(["brew", "--prefix", "llvm"], text=True).strip()) / "bin"
    sdk = subprocess.check_output(["xcrun", "--sdk", "macosx", "--show-sdk-path"], text=True).strip()
    clang = subprocess.check_output(["xcrun", "--find", "clang"], text=True).strip()
    env = os.environ.copy()
    sed_prefix = subprocess.check_output(["brew", "--prefix", "gnu-sed"], text=True).strip()
    env["PATH"] = f"{sed_prefix}/libexec/gnubin:{llvm}:" + env["PATH"]
    target = [llvm / "clang", "--target=aarch64-linux-gnu", "-ffixed-x18"]
    make = ["gmake", "-C", SOURCE, f"O={BUILD}", "ARCH=lkl", "CLANG_TARGET_FLAGS_lkl=aarch64-linux-gnu",
            f"CC={shlex.join(map(str, target))}", f"HOSTCC={shlex.join([clang, '-isysroot', sdk, '-I', str(include), *(['-idirafter', str(OUT / 'linux-uapi/include')] if PROCESS else [])])}",
            f"LD={shutil.which('ld.lld')}", "AR=llvm-ar", "NM=llvm-nm", "OBJCOPY=llvm-objcopy",
            "OBJDUMP=llvm-objdump", "STRIP=llvm-strip"]
    BUILD.mkdir(exist_ok=True)
    with (OUT / "lkl-build.log").open("w") as log:
        if PROCESS:
            run(["gmake", "-C", SOURCE, "ARCH=arm64", "O=" + str(OUT / "linux-uapi-build"),
                 "HOSTCC=" + shlex.join([clang, "-isysroot", sdk]),
                 "headers_install", "INSTALL_HDR_PATH=" + str(OUT / "linux-uapi")],
                env=env, stdout=log, stderr=subprocess.STDOUT)
        if not (BUILD / ".config").exists():
            run([*make, "allnoconfig"], env=env, stdout=log, stderr=subprocess.STDOUT)
        options = ["64BIT", "EXPERT", "PROC_FS", "SYSFS", "FUTEX", "EPOLL", "SIGNALFD", "TIMERFD",
                   "LKL_HOST_MEMCPY", "LKL_HOST_MEMSET", "LKL_HOST_MEMMOVE"]
        if PROCESS:
            options += ["MMU", "PRINTK", "NAMESPACES", "PID_NS", "POSIX_TIMERS", "ANDROID_BINDER_IPC", "ANDROID_BINDERFS", "MEMFD_CREATE", "TMPFS", "TMPFS_XATTR",
                        "NET", "UNIX", "INET", "AUDIT", "SECURITY", "SECURITY_NETWORK",
                        "SECURITY_SELINUX", "SECURITY_SELINUX_DEVELOP"]
        switches = [arg for name in options for arg in ("-e", name)]
        if PROCESS:
            switches += ["--set-str", "LSM", "selinux"]
        switches += [arg for name in ("KALLSYMS", "BLOCK", "DEBUG_INFO", "SLUB_DEBUG") for arg in ("-d", name)]
        run([SOURCE / "scripts/config", "--file", BUILD / ".config", *switches], env=env)
        run([*make, "olddefconfig"], env=env, stdout=log, stderr=subprocess.STDOUT)
        run([*make, "-j8", "all"], env=env, stdout=log, stderr=subprocess.STDOUT)
    shutil.copyfile(SOURCE / "arch/lkl/include/uapi/asm/host_ops.h", OUT / "lkl-host-ops.h")
    run([*target, "-fPIC", "-fno-stack-protector", "-O2", "-I", OUT, "-I", EXPERIMENT,
         "-c", EXPERIMENT / "lkl-bridge.c", "-o", OUT / "lkl-bridge.o"])
    exports = ["--globalize-symbol=nce_run_syscall", "--globalize-symbol=nce_page_fault", "--globalize-symbol=nce_set_layout", "--globalize-symbol=nce_user_return", "--globalize-symbol=nce_user_fault"] if PROCESS else []
    run([llvm / "llvm-objcopy", "--globalize-symbol=vsnprintf", "--globalize-symbol=lkl_ops", *exports,
         BUILD / "lkl.o", OUT / "lkl-exports.o"])
    run(["ld.lld", "-shared", "-Bsymbolic", "--no-undefined", "-z", "max-page-size=16384",
         "-z", "common-page-size=16384", OUT / "lkl-exports.o", OUT / "lkl-bridge.o", "-o", OUT / "lkl.so"])
    package_elf(OUT / "lkl.so")
    if PROCESS:
        build_userspace(OUT, llvm)
    inputs = [EXPERIMENT / name for name in ("lkl-main.c", "lkl-host.c", "nce.c", "entry.S")]
    flags = ["-O2", "-g", "-Wall", "-Wextra", "-Werror", "-I", OUT, "-I", EXPERIMENT]
    if PROCESS:
        flags += ["-DNCE_PROCESS"]
        inputs += [EXPERIMENT / "lkl-process-host.c", EXPERIMENT / "programs.c", EXPERIMENT / "memory.c", EXPERIMENT / "execute-check.c", EXPERIMENT / "compute.c"]
        run([clang, *flags, "-isysroot", sdk, "-DNATIVE", "-c", EXPERIMENT / "guest.S", "-o", OUT / "native.o"])
    run([clang, *flags, "-isysroot", sdk, *inputs, *([OUT / "native.o"] if PROCESS else []), "-o", OUT / "lkl-probe"])
    with (OUT / "lkl-results.jsonl").open("w") as output, (OUT / "lkl-console.log").open("w") as log:
        run([OUT / "lkl-probe", OUT / "lkl-image.bin"], stdout=output, stderr=log)
    results = [json.loads(line) for line in (OUT / "lkl-results.jsonl").read_text().splitlines()]
    if results[-1].get("result") != "passed":
        raise SystemExit("LKL probe did not complete")
    ios_sdk = subprocess.check_output(["xcrun", "--sdk", "iphoneos", "--show-sdk-path"], text=True).strip()
    run([clang, *flags, "-target", "arm64-apple-ios17.0", "-isysroot", ios_sdk,
         "-c", EXPERIMENT / "lkl-host.c", "-o", OUT / "ios-lkl-host.o"])
    if PROCESS:
        run([clang, *flags, "-target", "arm64-apple-ios17.0", "-isysroot", ios_sdk,
             "-c", EXPERIMENT / "lkl-process-host.c", "-o", OUT / "ios-lkl-process-host.o"])
    dispatch_results = None
    if PROCESS:
        run([clang, "-O2", "-Wall", "-Wextra", "-Werror", "-isysroot", sdk, "-I", OUT,
             EXPERIMENT / "host-context-test.c", EXPERIMENT / "lkl-host.c",
             "-o", OUT / "host-context-test"])
        run([OUT / "host-context-test"])
        run([clang, *flags, "-DNCE_IOS_APP", "-isysroot", sdk, *inputs, OUT / "native.o",
             EXPERIMENT / "dispatch-test.c", "-o", OUT / "dispatch-probe"])
        with (OUT / "dispatch-results.jsonl").open("w") as output, (OUT / "dispatch-console.log").open("w") as log:
            subprocess.run([OUT / "dispatch-probe", OUT / "lkl-image.bin"], check=True,
                           timeout=30, stdout=output, stderr=log)
        dispatch_results = [json.loads(line) for line in (OUT / "dispatch-results.jsonl").read_text().splitlines()]
        if dispatch_results[-1].get("result") != "passed":
            raise SystemExit("GCD probe did not complete")
        if not any(r.get("inherited_blocked_threads", 0) > 0 for r in dispatch_results):
            raise SystemExit("GCD regression did not exercise an inherited signal mask")
    manifest = {"source_revision": REVISION, "platform": platform.platform(), "results": results,
                "ios_status": "this build compiled for iOS; device execution pending",
                "config_sha256": hashlib.sha256((BUILD / ".config").read_bytes()).hexdigest(),
                "kernel_sha256": hashlib.sha256((OUT / "lkl-image.bin").read_bytes()).hexdigest()}
    sources = [*inputs, EXPERIMENT / "lkl-bridge.c", EXPERIMENT / "nce.h", EXPERIMENT / "guest-registers.h", EXPERIMENT / "guest.S", Path(__file__).resolve()]
    if PROCESS:
        manifest["dispatch_results"] = dispatch_results
        sources += [EXPERIMENT / "memory.h", EXPERIMENT / "layout.h", EXPERIMENT / "execute-check.h", Path(__file__).with_name("lkl_nce_patch.py"),
                    *sorted(EXPERIMENT.glob("lkl-*.h")),
                    *sorted(EXPERIMENT.glob("lkl-nce-*.c")), EXPERIMENT / "exec-guest.S", EXPERIMENT / "programs.h",
                    Path(__file__).with_name("build_nce_userspace.py"), EXPERIMENT / "dispatch-test.c", EXPERIMENT / "host-context-test.c",
                    Path(__file__).with_name("prepare_android_runtime.py"), Path(__file__).with_name("build_android_linker.py"),
                    Path(__file__).with_name("build_android_libc.py"), Path(__file__).with_name("build_android_art.py"),
                    EXPERIMENT / "android-sources.json", *sorted((EXPERIMENT / "art").glob("*")),
                    *sorted((EXPERIMENT / "userspace").glob("*")), *sorted((EXPERIMENT / "services").glob("*"))]
        run(["python3", ROOT / "scripts/test_nce_userspace.py"])
        sources.append(ROOT / "scripts/test_nce_userspace.py")
        manifest["stage"] = "Android Bionic, Binder, ART Java and ARM64 JIT workloads"
        manifest["userspace_libraries"] = {"musl": "1.2.5", "compiler-rt": "20.1.8", "dash": "0.5.12"}
        manifest["generated_sha256"] = {name: hashlib.sha256((OUT / name).read_bytes()).hexdigest()
                                        for name in ["program-images.h", "user-scripts.h", "lkl-image.h", "lkl-host-ops.h", "guest-image.h",
                                                     "exec-guest.elf", "sigreturn.elf", "init.elf", "worker.elf", "mm-bench.elf", "dash.elf",
                                                     "libc.so", "dependency.so", "plugin.so", "dynamic.elf", "binder-test.elf", "bionic-test.elf", "art-test.elf", "art-heap-test.elf", "android-services-test.elf"]}
        android = ROOT / "work/native-linux-nce/android"
        manifest["android_builds"] = {name: json.loads((android / name / "manifest.json").read_text())
                                      for name in ("linker-build", "libc-build", "art-build")}
        manifest["android_runtime_manifest_sha256"] = hashlib.sha256(
            (android / "android36-runtime.json").read_bytes()).hexdigest()
    changed = [str(p.relative_to(ROOT)) for p in sources if p.stat().st_mtime_ns > started_ns]
    if changed:
        raise SystemExit("Build inputs changed during validation; rerun: " + ", ".join(changed))
    manifest["input_sha256"] = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources}
    (OUT / "lkl-summary.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
