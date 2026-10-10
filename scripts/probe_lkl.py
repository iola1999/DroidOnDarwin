#!/usr/bin/env python3
"""在 Darwin 原生执行 LKL，并从受控 ARM64 ELF 调用真实 Linux 文件与定时器接口。"""
import argparse
import art_aot
import hashlib
from lkl_nce_patch import apply
from build_nce_userspace import build as build_userspace
import json
import os
from pathlib import Path
import platform
import plistlib
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
SMP = False


def run(args, **kwargs):
    return subprocess.run([str(x) for x in args], check=True, timeout=kwargs.pop('timeout', 300), **kwargs)


def check_ext4_shutdown(path):
    # e2fsck -n may return zero while skipping an unreplayed journal.
    with path.open('rb') as disk:
        disk.seek(1024)
        superblock = disk.read(1024)
    magic, state = struct.unpack_from('<HH', superblock, 0x38)
    incompat, ro_compat = struct.unpack_from('<II', superblock, 0x60)
    if magic != 0xef53 or state != 1 or incompat & 4 or ro_compat & 0x10000:
        raise SystemExit(f'Unclean ext4 shutdown: {path}: state={state:#x}, '
                         f'incompat={incompat:#x}, ro_compat={ro_compat:#x}')
    print('Ext4 shutdown state is clean: ' + str(path), flush=True)


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
        required |= {"nce_run_syscall", "nce_page_fault", "nce_set_layout", "nce_user_return", "nce_user_fault", "nce_user_preempt", "nce_prepare_user"}
    if SMP:
        required |= {"nce_begin_user", "nce_window_leave", "nce_copy_user", "nce_guest_fault"}
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
    global OUT, BUILD, PROCESS, SOURCE, SMP
    started_ns = time.time_ns()
    parser = argparse.ArgumentParser()
    parser.add_argument("--processes", action="store_true")
    parser.add_argument("--smp", action="store_true", help="Build and validate the independent two-CPU userspace variant")
    parser.add_argument("--core-scheduling", action="store_true", help="Coordinate SMP execution by address space")
    parser.add_argument("--art-oat", type=Path, help="Validate an offline-compiled OAT directory with the runtime build")
    parser.add_argument("--art-desktop", action="store_true", help="Use the desktop classpath for boot images or SystemUI/Launcher AOT")
    parser.add_argument("--art-compiler", action="store_true", help="Validate the matching dex2oat tool in an isolated NCE image")
    parser.add_argument("--art-boot-image", action="store_true", help="Generate a matching experimental ART boot image with --art-compiler")
    parser.add_argument("--art-boot", type=Path, help="Validate a single boot image at two runtime heap addresses")
    parser.add_argument("--graphics-angle", type=Path,
                        help="Run Linux GLES over the in-process renderer using these macOS ANGLE libraries")
    parser.add_argument("--graphics-window", action="store_true", help="Validate presentation in a visible Metal window")
    parser.add_argument("--storage-image", type=Path, help="Use this data image; create from the blank seed if absent")
    arguments = parser.parse_args()
    PROCESS = arguments.processes
    SMP = arguments.smp
    if SMP and not PROCESS:
        parser.error("--smp requires --processes")
    if arguments.core_scheduling and not SMP:
        parser.error("--core-scheduling requires --smp")
    angle = arguments.graphics_angle.resolve() if arguments.graphics_angle else None
    if arguments.graphics_window and not angle:
        parser.error("--graphics-window requires --graphics-angle")
    if angle and not PROCESS:
        parser.error("--graphics-angle requires --processes")
    if arguments.storage_image and not PROCESS:
        parser.error("--storage-image requires --processes")
    if (arguments.art_compiler or arguments.art_oat or arguments.art_boot) and not PROCESS:
        parser.error("ART compiler checks require --processes")
    if arguments.art_desktop and not (arguments.art_compiler or arguments.art_oat or arguments.art_boot):
        parser.error("--art-desktop requires --art-compiler, --art-oat or --art-boot")
    if arguments.art_compiler and arguments.art_oat:
        parser.error("Choose artifact generation or runtime loading")
    if arguments.art_boot and (arguments.art_compiler or arguments.art_oat):
        parser.error("Choose boot image runtime loading or other ART artifact modes")
    if arguments.art_boot_image and not arguments.art_compiler:
        parser.error("--art-boot-image requires --art-compiler")
    if arguments.art_compiler or ((arguments.art_boot or arguments.art_oat) and not arguments.art_desktop):
        if angle or os.environ.get('NCE_DESKTOP') or os.environ.get('NCE_SYSTEM_SERVER_PROBE'):
            parser.error("Isolated ART checks cannot run desktop or graphics checks")
    if (arguments.art_oat or arguments.art_boot) and arguments.art_desktop:
        if not angle or not os.environ.get('NCE_SYSTEM_SERVER_PROBE') or os.environ.get('NCE_DESKTOP'):
            parser.error("Desktop ART validation requires graphics and NCE_SYSTEM_SERVER_PROBE=1")
    if arguments.art_oat:
        arguments.art_oat = arguments.art_oat.resolve()
    if arguments.art_boot:
        arguments.art_boot = arguments.art_boot.resolve()
    if angle:
        for name in ("libANGLE-shared.dylib",):
            if not (angle / name).is_file():
                parser.error("Missing ANGLE library: " + str(angle / name))
    base = OUT
    smp_variant = "smp-core" if arguments.core_scheduling else "smp"
    if SMP:
        source = base / f"lkl-{smp_variant}-process-source"
        if not source.exists():
            run(["/bin/cp", "-cR", SOURCE, source], timeout=120)
        SOURCE = source
    if PROCESS:
        BUILD = base / "lkl-process-build"
        if arguments.art_boot_image:
            directory = "art-boot-compiler-desktop" if arguments.art_desktop else "art-boot-compiler"
        elif arguments.art_boot and not arguments.art_desktop:
            directory = "art-boot-runtime"
        elif arguments.art_compiler:
            directory = "art-compiler-desktop" if arguments.art_desktop else "art-compiler"
        elif arguments.art_oat and not arguments.art_desktop:
            directory = "art-oat"
        else:
            directory = "processes"
        if SMP:
            directory += "-" + smp_variant
            BUILD = base / f"lkl-{smp_variant}-process-build"
        OUT = base / directory
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
        (SOURCE / ".droidondarwin-source-revision").write_text(REVISION + "\n")
        archive.unlink()
    if (SOURCE / ".droidondarwin-source-revision").read_text().strip() != REVISION:
        raise SystemExit("Unexpected LKL source revision")
    if PROCESS:
        if SMP:
            from lkl_smp_patch import apply as apply_smp
            from lkl_smp_mmu_patch import apply as apply_mmu
            base_inputs = [Path(__file__).with_name("lkl_nce_patch.py"),
                           *sorted(EXPERIMENT.glob("lkl-nce-*.c")), *sorted(EXPERIMENT.glob("lkl-*.h")),
                           EXPERIMENT / "guest-registers.h", EXPERIMENT / "layout.h"]
            base_hashes = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in base_inputs}
            marker = SOURCE / ".droidondarwin-smp-process-base.json"
            if marker.exists():
                if json.loads(marker.read_text()) != base_hashes:
                    raise SystemExit("NCE base changed; regenerate the independent SMP process source tree")
            else:
                apply(SOURCE, EXPERIMENT)
                marker.write_text(json.dumps(base_hashes, indent=2) + "\n")
            apply_smp(SOURCE, EXPERIMENT, tests=False)
            apply_mmu(SOURCE, EXPERIMENT, tests=False)
            if arguments.core_scheduling:
                from lkl_smp_core_patch import apply as apply_core
                apply_core(SOURCE, EXPERIMENT)
        else:
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
    if SMP:
        target += ["-mno-outline-atomics"]
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
            options += ["BLOCK", "EXT4_FS", "EXT4_FS_SECURITY", "EXT4_FS_POSIX_ACL", "TMPFS_POSIX_ACL", "FUSE_FS", "CGROUPS", "MEMCG", "CGROUP_CPUACCT", "CGROUP_SCHED", "FAIR_GROUP_SCHED", "SECCOMP", "SECCOMP_FILTER", "PAGE_REPORTING", "INOTIFY_USER"]
            options += ["INPUT", "INPUT_EVDEV", "INPUT_MISC", "INPUT_UINPUT"]
            options += ["PSI", "BPF_SYSCALL", "CGROUP_BPF", "NETFILTER", "NETFILTER_ADVANCED",
                        "NETFILTER_NETLINK_LOG", "IP_ADVANCED_ROUTER", "IP_MULTIPLE_TABLES",
                        "IPV6_MULTIPLE_TABLES", "NET_SCHED", "NET_CLS_ACT", "NET_CLS_CGROUP", "NET_CLS_BPF", "NET_ACT_BPF",
                        "NET_SCH_INGRESS", "NETFILTER_XTABLES", "IP_NF_IPTABLES", "IP_NF_FILTER",
                        "IP_NF_MANGLE", "IP6_NF_IPTABLES", "IP6_NF_FILTER", "IP6_NF_MANGLE"]
            options += ["NF_CONNTRACK", "NF_NAT", "IP_NF_NAT", "IP_NF_RAW", "IP6_NF_RAW",
                        "IP_NF_TARGET_REJECT", "IP6_NF_TARGET_REJECT", "XFRM_USER",
                        "NETFILTER_XT_MATCH_BPF", "NETFILTER_XT_MATCH_MARK", "NETFILTER_XT_TARGET_MARK",
                        "NETFILTER_XT_MATCH_POLICY", "NETFILTER_XT_MATCH_OWNER",
                        "NETFILTER_XT_MATCH_CONNMARK", "NETFILTER_XT_TARGET_CONNMARK",
                        "NETFILTER_XT_MATCH_CONNTRACK", "NETFILTER_XT_TARGET_NFLOG",
                        "NETFILTER_XT_TARGET_TCPMSS", "NETFILTER_XT_TARGET_CT",
                        "NETFILTER_XT_TARGET_MASQUERADE"]
            options += ["NETFILTER_XT_MATCH_U32", "NETFILTER_XT_MATCH_STATE", "NETFILTER_XT_MATCH_TCPMSS",
                        "NET_KEY", "IP_MROUTE", "IPV6_MROUTE", "IKCONFIG", "IKCONFIG_PROC"]
        switches = [arg for name in options for arg in ("-e", name)]
        if SMP:
            switches += ["-e", "SMP", "-e", "DEBUG_KERNEL"]
        if arguments.core_scheduling:
            switches += ["-e", "SCHED_SMT", "-e", "SCHED_CORE"]
        if PROCESS:
            switches += ["--set-str", "LSM", "selinux"]
        switches += [arg for name in ("KALLSYMS", "DEBUG_INFO", "SLUB_DEBUG", *([] if PROCESS else ["BLOCK"])) for arg in ("-d", name)]
        run([SOURCE / "scripts/config", "--file", BUILD / ".config", *switches], env=env)
        run([*make, "olddefconfig"], env=env, stdout=log, stderr=subprocess.STDOUT)
        if SMP:
            config = (BUILD / ".config").read_text()
            assert all(name + "=y\n" in config for name in ("CONFIG_SMP", "CONFIG_MMU", "CONFIG_MMU_GATHER_RCU_TABLE_FREE"))
            assert "CONFIG_NR_CPUS=2\n" in config
            assert ("CONFIG_SCHED_CORE=y\n" in config) == arguments.core_scheduling
        run([*make, "-j8", "all"], env=env, stdout=log, stderr=subprocess.STDOUT)
    shutil.copyfile(SOURCE / "arch/lkl/include/uapi/asm/host_ops.h", OUT / "lkl-host-ops.h")
    run([*target, "-fPIC", "-fno-stack-protector", "-O2", "-I", OUT, "-I", EXPERIMENT,
         "-c", EXPERIMENT / "lkl-bridge.c", "-o", OUT / "lkl-bridge.o"])
    exports = ["--globalize-symbol=nce_run_syscall", "--globalize-symbol=nce_page_fault", "--globalize-symbol=nce_set_layout", "--globalize-symbol=nce_user_return", "--globalize-symbol=nce_user_fault", "--globalize-symbol=nce_user_preempt", "--globalize-symbol=nce_prepare_user"] if PROCESS else []
    if SMP:
        exports += ["--globalize-symbol=" + name for name in ("nce_begin_user", "nce_window_leave", "nce_copy_user", "nce_guest_fault")]
        exports += ["--set-section-alignment=.data..percpu=16384"]
    run([llvm / "llvm-objcopy", "--globalize-symbol=vsnprintf", "--globalize-symbol=lkl_ops", "--globalize-symbol=sysctl_sched_base_slice", *exports,
         BUILD / "lkl.o", OUT / "lkl-exports.o"])
    run(["ld.lld", "-shared", "-Bsymbolic", "--no-undefined", "-z", "max-page-size=16384",
         "-z", "common-page-size=16384", OUT / "lkl-exports.o", OUT / "lkl-bridge.o", "-o", OUT / "lkl.so"])
    package_elf(OUT / "lkl.so")
    if PROCESS:
        build_userspace(OUT, llvm, art_compiler=arguments.art_compiler, art_oat=arguments.art_oat, art_desktop=arguments.art_desktop, art_boot=arguments.art_boot, art_boot_image=arguments.art_boot_image)
    inputs = [EXPERIMENT / name for name in ("lkl-main.c", "lkl-host.c", "nce.c", "nce-thread.c", "entry.S")]
    flags = ["-O2", "-g", "-Wall", "-Wextra", "-Werror", "-I", OUT, "-I", EXPERIMENT]
    if SMP:
        flags += ["-DNCE_SMP"]
    if arguments.core_scheduling:
        flags += ["-DNCE_CORE_SCHEDULING"]
    if PROCESS:
        flags += ["-DNCE_PROCESS"]
        inputs += [EXPERIMENT / "lkl-process-host.c", EXPERIMENT / "nce-cpu.c", EXPERIMENT / "programs.c", EXPERIMENT / "memory.c", EXPERIMENT / "execute-check.c", EXPERIMENT / "compute.c", EXPERIMENT / "input.c", EXPERIMENT / "session.c"]
        run([clang, *flags, "-isysroot", sdk, "-DNATIVE", "-c", EXPERIMENT / "guest.S", "-o", OUT / "native.o"])
    payload = [OUT / "payload.S"] if PROCESS else []
    graphics_link = []
    graphics_files = []
    graphics_env = {}
    if angle:
        gfx = ROOT / "work/gfxstream-src"
        prefix = ROOT / "work/gfxstream-build-macos-ios-api/prefix"
        build = json.loads((prefix.parent / "manifest.json").read_text())
        for name, expected in build["inputs"].items():
            if hashlib.sha256((ROOT / name).read_bytes()).hexdigest() != expected:
                raise SystemExit("Rebuild gfxstream; input changed: " + name)
        for name, expected in build["libraries"].items():
            if hashlib.sha256(Path(name).read_bytes()).hexdigest() != expected:
                raise SystemExit("Rebuild gfxstream; library changed: " + name)
        flags.append("-DNCE_GRAPHICS")
        run([clang, "-std=c++17", "-O2", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-return-type-c-linkage",
             "-isysroot", sdk,
             "-isystem", gfx / "host/include", "-isystem", gfx / "host/features/include", "-isystem", gfx / "common/base/include",
             "-c", EXPERIMENT / "graphics/host-graphics.cpp", "-o", OUT / "host-graphics.o"])
        graphics_link = [OUT / "host-graphics.o", "-L", prefix / "lib", "-lgfxstream_backend", "-lc++",
                         "-Wl,-rpath," + str(prefix / "lib"), "-Wl,-rpath," + str(angle)]
        graphics_env = {"DYLD_LIBRARY_PATH": str(angle), "ANGLE_DEFAULT_PLATFORM": "metal"}
        graphics_files = [prefix / "lib/libgfxstream_backend.dylib", angle / "libANGLE-shared.dylib"]
        run([clang, "-std=c++17", "-O2", "-Wno-return-type-c-linkage", "-isysroot", sdk,
             "-I", gfx / "host/include", "-I", gfx / "host/features/include", "-I", gfx / "common/base/include",
             "-I", EXPERIMENT / "graphics/protocol", "-I", angle.parent / "include",
             EXPERIMENT / "graphics/channel-test.cpp", *graphics_link[1:], "-o", OUT / "channel-test"])
        with (OUT / "channel-test.log").open("w") as log:
            run([OUT / "channel-test"], stdout=log, stderr=subprocess.STDOUT,
                env={**os.environ, **graphics_env})
    run([clang, *flags, "-isysroot", sdk, *inputs, *payload, *([OUT / "native.o"] if PROCESS else []),
         *graphics_link, "-o", OUT / "lkl-probe"])
    data_image = arguments.storage_image.resolve() if arguments.storage_image else OUT / "android-data.img"
    storage_started_from_seed = PROCESS and not data_image.exists()
    if PROCESS:
        seed = OUT / "android-data-seed.img"
        if not seed.exists():
            with seed.open("wb") as image:
                image.truncate(512 * 1024 * 1024)
            run(["/opt/homebrew/opt/e2fsprogs/sbin/mke2fs", "-F", "-q", "-t", "ext4", "-b", "4096",
                 "-E", "lazy_itable_init=0,lazy_journal_init=0", seed])
        if storage_started_from_seed:
            data_image.parent.mkdir(parents=True, exist_ok=True)
            run(["cp", "-c", seed, data_image])
    with (OUT / "lkl-results.jsonl").open("w") as output, (OUT / "lkl-console.log").open("w") as log:
        probe_env = {**os.environ, "NCE_GPU_OUTPUT": str(OUT / "gpu.bmp"), "NCE_GRAPHICS_OUTPUT": str(OUT / "framework.png"), "NCE_STORAGE_IMAGE": str(data_image), "NCE_SYSTEM_IMAGE": str(OUT / "system.img")} if PROCESS else os.environ
        if arguments.art_compiler:
            probe_env.update(NCE_ART_COMPILER_PROBE='1', NCE_ART_NTERP='1',
                             NCE_ART_EXPANDED_JIT='1', NCE_ART_FAST_BASELINE='0')
            if arguments.art_desktop:
                key = 'NCE_ART_COMPILER_BOOT_DESKTOP' if arguments.art_boot_image else 'NCE_ART_COMPILER_DESKTOP'
                probe_env[key] = '1'
            if arguments.art_boot_image:
                probe_env['NCE_ART_COMPILER_BOOT'] = '1'
            artifacts = OUT / 'artifacts'
            artifacts.mkdir(exist_ok=True)
            probe_env['NCE_ART_COMPILER_OUTPUT'] = str(artifacts)
        if arguments.art_oat:
            probe_env['NCE_TRACE_SYSTEMUI' if arguments.art_desktop else 'NCE_ART_OAT_PROBE'] = '1'
        if arguments.art_boot:
            if arguments.art_desktop:
                probe_env['NCE_ART_BOOT_IMAGE'] = os.environ.get('NCE_ART_BOOT_IMAGE', '1')
            else:
                probe_env.update(NCE_ART_BOOT_PROBE='1', NCE_ART_NTERP='1',
                                 NCE_ART_EXPANDED_JIT='1', NCE_ART_FAST_BASELINE='0')
        probe_timeout = 3600 if arguments.art_compiler and arguments.art_desktop else 480
        run([OUT / "lkl-probe", OUT / "lkl-image.bin"], timeout=probe_timeout, stdout=output, stderr=log, env={**probe_env, **graphics_env})
    results = [json.loads(line) for line in (OUT / "lkl-results.jsonl").read_text().splitlines()]
    if results[-1].get("result") != "passed":
        raise SystemExit("LKL probe did not complete")
    if PROCESS:
        check_ext4_shutdown(data_image)
    ios_sdk = subprocess.check_output(["xcrun", "--sdk", "iphoneos", "--show-sdk-path"], text=True).strip()
    run([clang, *flags, "-target", "arm64-apple-ios17.0", "-isysroot", ios_sdk,
         "-c", EXPERIMENT / "lkl-host.c", "-o", OUT / "ios-lkl-host.o"])
    if PROCESS:
        run([clang, "-target", "arm64-apple-ios17.0", "-isysroot", ios_sdk,
             "-c", OUT / "payload.S", "-o", OUT / "ios-payload.o"])
        run([clang, *flags, "-target", "arm64-apple-ios17.0", "-isysroot", ios_sdk,
             "-c", EXPERIMENT / "lkl-process-host.c", "-o", OUT / "ios-lkl-process-host.o"])
    dispatch_results = None
    if PROCESS:
        run([clang, "-O2", "-Wall", "-Wextra", "-Werror", "-isysroot", sdk, "-I", OUT,
             EXPERIMENT / "host-context-test.c", EXPERIMENT / "lkl-host.c",
             "-o", OUT / "host-context-test"])
        run([OUT / "host-context-test"])
        run([clang, *flags, "-DNCE_IOS_APP", "-isysroot", sdk, *inputs, *payload, OUT / "native.o",
             EXPERIMENT / "dispatch-test.c", *graphics_link, "-o", OUT / "dispatch-probe"])
        with (OUT / "dispatch-results.jsonl").open("w") as output, (OUT / "dispatch-console.log").open("w") as log:
            subprocess.run([OUT / "dispatch-probe", OUT / "lkl-image.bin"], check=True,
                           timeout=probe_timeout, stdout=output, stderr=log,
                           env={**probe_env, **graphics_env, "NCE_GPU_OUTPUT": str(OUT / "dispatch-gpu.bmp"), "NCE_GRAPHICS_OUTPUT": str(OUT / "dispatch-framework.png")})
        dispatch_results = [json.loads(line) for line in (OUT / "dispatch-results.jsonl").read_text().splitlines()]
        if dispatch_results[-1].get("result") != "passed":
            raise SystemExit("GCD probe did not complete")
        check_ext4_shutdown(data_image)
        if arguments.art_oat and not arguments.art_desktop:
            compiler_bases = set(json.loads((arguments.art_oat / 'manifest.json').read_text())['compiler_heap_bases'])
            for records in (results, dispatch_results):
                runtime_bases = {row['reference_base'] for row in records if row.get('phase') == 'art_aot_heap'}
                if len(runtime_bases) != 2 or not runtime_bases - compiler_bases:
                    raise SystemExit('AOT checks did not exercise different compiler/runtime heap bases')
        if not any(r.get("inherited_blocked_threads", 0) > 0 for r in dispatch_results):
            raise SystemExit("GCD regression did not exercise an inherited signal mask")
        generations = [next((r.get("generation", 0) for r in records
                             if r.get("test") == "linux_ext4_persistence" and r.get("passed")), 0)
                       for records in (results, dispatch_results)]
        if generations[0] < 1 or generations[1] != generations[0] + 1:
            raise SystemExit("Persistent storage did not survive kernel restart")
        if os.environ.get("NCE_SYSTEM_SERVER_PROBE"):
            for name in ("android_keystore_persistence", "android_application_data_persistence"):
                checks = [next((r for r in records if r.get("test") == name and r.get("passed")), None)
                          for records in (results, dispatch_results)]
                if not all(checks) or not checks[1].get("restored"):
                    raise SystemExit(name + " did not verify data from the preceding Android run")
                if name == "android_application_data_persistence" and checks[0]["row_id"] != checks[1]["row_id"]:
                    raise SystemExit("Clock alarm identity changed across Android restart")
        run(["/opt/homebrew/opt/e2fsprogs/sbin/e2fsck", "-fn", data_image])
        if not arguments.art_compiler and (not arguments.art_boot or arguments.art_desktop) and (not arguments.art_oat or arguments.art_desktop) and (OUT / "framework.png").read_bytes() != (OUT / "dispatch-framework.png").read_bytes():
            raise SystemExit("CLI and GCD framework images differ")
        if angle and (OUT / "gpu.bmp").read_bytes() != (OUT / "dispatch-gpu.bmp").read_bytes():
            raise SystemExit("CLI and GCD GPU images differ")
    window_results = None
    if arguments.graphics_window:
        app = OUT / "DroidOnDarwinGPU.app/Contents"
        (app / "MacOS").mkdir(parents=True, exist_ok=True)
        (app / "Info.plist").write_bytes(plistlib.dumps({
            "CFBundleIdentifier": "app.droidondarwin.graphics-test",
            "CFBundleExecutable": "graphics-window", "CFBundleName": "DroidOnDarwin GPU",
            "CFBundlePackageType": "APPL", "NSHighResolutionCapable": True}))
        window_binary = app / "MacOS/graphics-window"
        run([clang, "-std=c++17", "-O2", "-fobjc-arc", "-isysroot", sdk,
             "-c", EXPERIMENT / "graphics/window-test.mm", "-o", OUT / "graphics-window.o"])
        run([clang, *flags, "-DNCE_IOS_APP", "-isysroot", sdk, *inputs, *payload, OUT / "native.o",
             OUT / "graphics-window.o", *graphics_link, "-framework", "AppKit", "-framework", "Metal",
             "-framework", "QuartzCore", "-o", window_binary])
        with (OUT / "window-results.jsonl").open("w") as output, (OUT / "window-console.log").open("w") as log:
            run([window_binary, OUT / "lkl-image.bin"], timeout=480, stdout=output, stderr=log,
                env={**probe_env, **graphics_env, "NCE_GPU_OUTPUT": str(OUT / "window-gpu.bmp")})
        window_results = [json.loads(line) for line in (OUT / "window-results.jsonl").read_text().splitlines()]
        if window_results[-1].get("result") != "passed" or not any(
                r.get("test") == "native_metal_display_created" and r.get("passed") for r in window_results):
            raise SystemExit("Metal window validation did not complete")
        check_ext4_shutdown(data_image)
        if (OUT / "gpu.bmp").read_bytes() != (OUT / "window-gpu.bmp").read_bytes():
            raise SystemExit("Headless and visible GPU output differ")
    manifest = {"source_revision": REVISION, "platform": platform.platform(), "results": results,
                "ios_status": "this build compiled for iOS; device execution pending",
                "config_sha256": hashlib.sha256((BUILD / ".config").read_bytes()).hexdigest(),
                "kernel_sha256": hashlib.sha256((OUT / "lkl-image.bin").read_bytes()).hexdigest()}
    if PROCESS:
        manifest["storage_started_from_seed"] = storage_started_from_seed
    if angle:
        manifest["window_results"] = window_results
        manifest["graphics_libraries_sha256"] = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in graphics_files}
        manifest["ios_status"] = "core compiled for iOS; graphics integration and device execution pending"
    sources = [*inputs, EXPERIMENT / "lkl-bridge.c", EXPERIMENT / "nce.h", EXPERIMENT / "nce-thread.h", EXPERIMENT / "guest-registers.h", EXPERIMENT / "guest.S", Path(__file__).resolve()]
    if SMP:
        manifest["cpus"] = 2
        manifest["core_scheduling"] = arguments.core_scheduling
        sources += [*sorted((EXPERIMENT / "smp").glob("*")), Path(__file__).with_name("lkl_smp_patch.py"),
                    Path(__file__).with_name("lkl_smp_mmu_patch.py")]
        if arguments.core_scheduling:
            sources.append(Path(__file__).with_name("lkl_smp_core_patch.py"))
    if PROCESS:
        manifest["dispatch_results"] = dispatch_results
        sources += [EXPERIMENT / "nce-cpu.h", EXPERIMENT / "memory.h", EXPERIMENT / "layout.h", EXPERIMENT / "execute-check.h", EXPERIMENT / "input.h", EXPERIMENT / "session.h", Path(__file__).with_name("lkl_nce_patch.py"),
                    *sorted(EXPERIMENT.glob("lkl-*.h")),
                    *sorted(EXPERIMENT.glob("lkl-nce-*.c")), EXPERIMENT / "exec-guest.S", EXPERIMENT / "programs.h",
                    Path(__file__).with_name("build_nce_userspace.py"), EXPERIMENT / "dispatch-test.c", EXPERIMENT / "host-context-test.c",
                    Path(__file__).with_name("prepare_art_test_dex.py"), Path(__file__).with_name("art_aot.py"),
                    Path(__file__).with_name("prepare_android_runtime.py"), Path(__file__).with_name("prepare_android_modules.py"),
                    Path(__file__).with_name("prepare_android_desktop.py"), Path(__file__).with_name("build_android_linker.py"),
                    Path(__file__).with_name("build_android_libc.py"), Path(__file__).with_name("build_android_art.py"),
                    Path(__file__).with_name("build_android_crypto.py"), Path(__file__).with_name("build_android_gralloc.py"), Path(__file__).with_name("build_android_network.py"),
                    ROOT / "patches/nce-gralloc-memfd.patch",
                    EXPERIMENT / "android-sources.json", *sorted((EXPERIMENT / "art").glob("*")),
                    *sorted((EXPERIMENT / "userspace").glob("*")), *sorted(p for p in (EXPERIMENT / "services").rglob("*") if p.is_file()),
                    *sorted(p for p in (EXPERIMENT / "graphics").rglob("*") if p.is_file())]
        run(["python3", ROOT / "scripts/test_nce_userspace.py", "--fixtures", OUT])
        sources.append(ROOT / "scripts/test_nce_userspace.py")
        manifest["stage"] = "Android Bionic, Binder, ART Java and ARM64 JIT workloads"
        if arguments.art_compiler:
            manifest["stage"] = "NCE ART offline compiler validation"
        if arguments.art_boot:
            manifest['stage'] = 'NCE ART boot image desktop validation' if arguments.art_desktop else 'NCE ART boot image loading at two heap addresses'
            artifact = json.loads((arguments.art_boot / 'boot-manifest.json').read_text())
            sources += [arguments.art_boot / name for name in ['boot-manifest.json', *artifact['files']]]
            manifest['art_boot'] = artifact
            for records in (() if arguments.art_desktop else (results, dispatch_results)):
                bases = {r['reference_base'] for r in records if r.get('phase') == 'art_boot_runtime_heap'}
                if len(bases) != 2 or not bases - set(artifact['compiler_heap_bases']):
                    raise SystemExit('Boot image checks did not exercise different heap bases')
                images = [r for r in records if r.get('phase') == 'art_boot_runtime_image']
                generated_offset = artifact['plan']['compiler_settings']['image_offset']
                if len(images) != 2 or any(r['image_begin'] - r['reference_base'] == generated_offset for r in images):
                    raise SystemExit('Boot image checks did not relocate compressed object offsets')
            if arguments.art_desktop:
                log_names = ['lkl-console.log', 'dispatch-console.log']
                if window_results is not None:
                    log_names.append('window-console.log')
                for log_name in log_names:
                    loaded = 'NCE_BOOT_IMAGE loaded=' in (OUT / log_name).read_text()
                    if loaded != (probe_env['NCE_ART_BOOT_IMAGE'] == '1'):
                        raise SystemExit('Unexpected boot image loading state in ' + log_name)
        if arguments.art_oat:
            manifest["stage"] = "NCE ART offline code execution validation"
            artifact = json.loads((arguments.art_oat / 'manifest.json').read_text())
            sources += [arguments.art_oat / name for name in ['manifest.json', *artifact['files']]]
            manifest['art_aot'] = artifact
            if arguments.art_desktop:
                for log_name in ('lkl-console.log', 'dispatch-console.log'):
                    log = (OUT / log_name).read_text()
                    for target in art_aot.DESKTOP:
                        path = str(Path(target['dex']).parent / 'oat/arm64' / (target['name'] + '.odex'))
                        executable = 'false' if os.environ.get('NCE_ART_AOT') == '0' else 'true'
                        if path + ': speed executable=' + executable not in log:
                            raise SystemExit('Missing expected AOT executable=' + executable + ' in ' + log_name + ': ' + path)
        manifest["userspace_libraries"] = {"musl": "1.2.5", "compiler-rt": "20.1.8", "dash": "0.5.12"}
        manifest["generated_sha256"] = {name: hashlib.sha256((OUT / name).read_bytes()).hexdigest()
                                        for name in ["system.img", "art-aot-targets.h", "android-data-seed.img", "storage-test.elf", "program-images.h", "user-scripts.h", "payload.S", "payload.bin", "lkl-image.h", "lkl-host-ops.h", "guest-image.h",
                                                     "exec-guest.elf", "sigreturn.elf", "init.elf", "worker.elf", "mm-bench.elf", "dash.elf",
                                                     "libc.so", "dependency.so", "plugin.so", "dynamic.elf", "binder-test.elf", "gpu-test.elf", "egl-test.elf", "gralloc-test.elf", "libnce-qemupipe.so", "bionic-test.elf", "cfi-test.elf", "libnce-cfi-a.so", "libnce-cfi-b.so", "nce-native-libraries.apk", "art-test.elf", "art-heap-test.elf", "android-services-test.elf"]}
        android = ROOT / "work/native-linux-nce/android"
        manifest["android_builds"] = {name: json.loads((android / name / "manifest.json").read_text())
                                      for name in ("linker-build", "libc-build", "art-build", "crypto-build")}
        manifest["android_runtime_manifest_sha256"] = hashlib.sha256(
            (android / "android36-runtime.json").read_bytes()).hexdigest()
        manifest["android_modules_manifest_sha256"] = hashlib.sha256(
            (android / "android36-modules/manifest.json").read_bytes()).hexdigest()
        manifest["android_desktop_manifest_sha256"] = hashlib.sha256(
            (android / "android36-desktop/manifest.json").read_bytes()).hexdigest()
        manifest["aosp_launcher_sha256"] = hashlib.sha256(
            (android / "android36-aosp-components/Launcher3QuickStep/Launcher3QuickStep.apk").read_bytes()).hexdigest()
        manifest["android_gralloc_build"] = json.loads((ROOT / "work/native-linux-nce/gralloc-build/manifest.json").read_text())
    changed = [str(p.relative_to(ROOT)) for p in sources if p.stat().st_mtime_ns > started_ns]
    if changed:
        raise SystemExit("Build inputs changed during validation; rerun: " + ", ".join(changed))
    manifest["input_sha256"] = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources}
    (OUT / "lkl-summary.json").write_text(json.dumps(manifest, indent=2) + "\n")
    if arguments.art_compiler:
        artifacts = OUT / 'artifacts'
        compiler_heap_bases = sorted({row['reference_base'] for row in results + dispatch_results
                                      if row.get('phase') == 'art_compiler_heap'})
        if not compiler_heap_bases:
            raise SystemExit('Compiler heap address was not recorded')
        art_aot.write_manifest(artifacts, manifest['android_builds']['art-build']['inputs'],
                               json.loads((OUT / 'art-aot-plan.json').read_text()), compiler_heap_bases)
        if arguments.art_boot_image:
            boot_bases = {r['reference_base'] for r in results + dispatch_results
                          if r.get('phase') == 'art_boot_image_header'}
            art_aot.write_manifest(artifacts, manifest['android_builds']['art-build']['inputs'],
                                   json.loads((OUT / 'art-boot-plan.json').read_text()), boot_bases)
            manifest['boot_image_generation'] = {
                'files': {name: hashlib.sha256((artifacts / name).read_bytes()).hexdigest()
                          for name in ('boot.art', 'boot.oat', 'boot.vdex')},
                'runtime_loading_verified': False,
            }
            (OUT / 'lkl-summary.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
