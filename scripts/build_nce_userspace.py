#!/usr/bin/env python3
"""编译内置 Linux 程序，按 ELF 段与 AArch64 映射符号生成执行布局。"""
import base64
import art_aot
import hashlib
import inspect as python_inspect
import json
import lzma
from pathlib import Path
import re
import shlex
import shutil
import struct
import subprocess
import tarfile
import tempfile
import urllib.request
import zipfile
import xml.etree.ElementTree as ET
from prepare_android_runtime import main as prepare_android_runtime
from build_android_linker import build as build_android_linker
from build_android_libc import build as build_android_libc
from build_android_art import build as build_android_art
from build_android_crypto import build as build_android_crypto
from prepare_android_modules import prepare as prepare_android_modules, OUT as MODULES
from prepare_android_desktop import prepare as prepare_android_desktop, prepare_vendor_module, prepare_aosp_components, OUT as DESKTOP
from build_android_gralloc import build as build_android_gralloc
from build_android_network import build as build_android_network
from prepare_art_test_dex import prepare_interface_object_calls

ROOT = Path(__file__).resolve().parents[1]
EXP = ROOT / 'experiments/native-linux'
WORK = ROOT / 'work/native-linux-nce'
PAGE = 16384
PROGRAM_LIMIT = 512 * 1024 * 1024
MAX_ALIGNMENT = 2 * 1024 * 1024
INTERPRETER = '/lib/ld-musl-aarch64.so.1'
ANDROID_INTERPRETER = '/system/bin/linker64'
ANDROID_BOOTSTRAP_INTERPRETER = '/system/bin/bootstrap/linker64'
MUSL_SHA = 'a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4'
RT_SHA = '15277402f6fd63397c0917a5c7171cda82d16d226094b828c1ed0f58f73b9c69'
DASH_SHA = '6a474ac46e8b0b32916c4c60df694c82058d3297d8b385b74508030ca4a8f28a'


def run(args, **kw):
    return subprocess.run([str(a) for a in args], check=True, timeout=300, **kw)


def musl(llvm):
    source = WORK / 'musl-1.2.5'
    archive = WORK / 'musl-1.2.5.tar.gz'
    if not archive.exists():
        urllib.request.urlretrieve('https://musl.libc.org/releases/musl-1.2.5.tar.gz', archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != MUSL_SHA:
        raise RuntimeError('musl source archive hash mismatch')
    if not source.exists():
        with tarfile.open(archive) as tar:
            tar.extractall(WORK, filter='data')
    build = WORK / 'musl-build'
    build.mkdir(exist_ok=True)
    target = [llvm / 'clang', '--target=aarch64-linux-musl', '-ffixed-x18', '-mno-outline-atomics']
    with (build / 'build.log').open('w') as log:
        if not (build / 'install/lib/libc.so').exists():
            run([source / 'configure', '--target=aarch64-linux-musl', '--enable-shared',
                 '--prefix=' + str(build / 'install'), '--syslibdir=' + str(build / 'install/lib'),
                 'CC=' + shlex.join(map(str, target)), 'AR=' + str(llvm / 'llvm-ar'),
                 'RANLIB=' + str(llvm / 'llvm-ranlib'), 'CFLAGS=-O2 -fPIC',
                 'LDFLAGS=-fuse-ld=lld -Wl,-z,max-page-size=16384,-z,common-page-size=16384', 'LIBCC='],
                cwd=build, stdout=log, stderr=subprocess.STDOUT)
        run(['gmake', '-j8', 'install'], cwd=build, stdout=log, stderr=subprocess.STDOUT)
    return build / 'install'


def x18_instruction(word):
    rd, rn, rm = word & 31, (word >> 5) & 31, (word >> 16) & 31
    return (word & 0x7fe0fc00 == 0x2a000000 and 18 in (rd, rn, rm) or
            word in (0x92403643, 0x9272c652) or
            word & 0xff800000 == 0xa9000000 and 18 in (rd, (word >> 10) & 31))

def android_dependencies(llvm, replacements, embedded=()):
    root = WORK / 'android/android36'
    files = {}
    for directory in ('lib64', 'com.android.art/lib64', 'com.android.runtime/lib64/bionic',
                      'com.android.i18n/lib64', 'com.android.os.statsd/lib64'):
        for path in (root / directory).glob('*.so'):
            if path.is_file(): files[path.name] = path
    for module in ('com.android.tethering', 'com.android.adbd', 'com.android.conscrypt', 'com.android.resolv', 'com.android.appsearch'):
        for path in (root / module / 'lib64').glob('*.so'):
            if path.is_file(): files.setdefault(path.name, path)
    files.update(replacements)
    pending = ['libart.so', 'libjavacore.so', 'libopenjdk.so', 'libicu_jni.so',
               'libstats_jni.so', 'libjavacrypto.so', 'libartpalette-system.so', 'libbinder_ndk.so', 'libandroid.so',
               'libprocessgroup_setup.so', 'libframework-connectivity-tiramisu-jni.so', 'libandroid_servers.so',
               'libalarm_jni.so', 'libbpf.so', 'libservice-connectivity.so', 'libmedia_jni.so', 'libsoundpool.so', 'libvintf_jni.so',
               'libframework-connectivity-jni.so', 'libicing.so', 'libdrmframework_jni.so',
               'libjnigraphics.so', 'libcompiler_rt.so', 'libaudioeffect_jni.so', 'librtp_jni.so',
               'libcom.android.tethering.dns_helper.so', 'librs_jni.so', 'libmainlinecronet.133.0.6876.3.so',
               'libandroid_net_connectivity_com_android_net_module_util_jni.so']
    for name in ('servicemanager', 'app_process64', 'aconfigd-system', 'installd', 'idmap2', 'idmap2d',
                 'vold', 'vold_prepare_subdirs', 'storaged', 'cmd', 'dumpsys', 'netd', 'lmkd', 'bpfloader',
                 'iptables', 'audioserver', 'mediaserver', 'mediametrics', 'gatekeeperd', 'keystore2', 'hw/android.system.suspend-service'):
        pending += re.findall(r'Shared library: \[(.*?)\]',
                              subprocess.check_output([llvm / 'llvm-readelf', '-d', root / 'bin' / name], text=True))
    pending += re.findall(r'Shared library: \[(.*?)\]', subprocess.check_output(
        [llvm / 'llvm-readelf', '-d', root / 'com.android.art/bin/artd'], text=True))
    pending += re.findall(r'Shared library: \[(.*?)\]', subprocess.check_output(
        [llvm / 'llvm-readelf', '-d', root / 'com.android.os.statsd/bin/statsd'], text=True))
    pending += re.findall(r'Shared library: \[(.*?)\]', subprocess.check_output(
        [llvm / 'llvm-readelf', '-d', root / 'com.android.tethering/bin/for-system/clatd'], text=True))
    pending += re.findall(r'Shared library: \[(.*?)\]', subprocess.check_output(
        [llvm / 'llvm-readelf', '-d', root / 'com.android.sdkext/bin/derive_classpath'], text=True))
    for guest, binary in embedded:
        siblings = {Path(name).name for name, _ in embedded if name.split('!/', 1)[0] == guest.split('!/', 1)[0]}
        needed = re.findall(r'Shared library: \[(.*?)\]',
                            subprocess.check_output([llvm / 'llvm-readelf', '-d', binary], text=True))
        pending += [name for name in needed if name not in siblings]
    result = {}
    while pending:
        name = pending.pop()
        if name in result or name in ('libc.so', 'libdl.so', 'ld-android.so'):
            continue
        if name not in files:
            raise RuntimeError(f'Missing Android dependency: {name}')
        path = files[name]
        result[name] = path
        dynamic = subprocess.check_output([llvm / 'llvm-readelf', '-d', path], text=True)
        pending += re.findall(r'Shared library: \[(.*?)\]', dynamic)
    return [('/system/lib64/' + name, path) for name, path in sorted(result.items())]

def android_graphics_dependencies(llvm, programs):
    power = prepare_vendor_module('com.android.hardware.power')
    gatekeeper = prepare_vendor_module('com.android.hardware.gatekeeper.nonsecure')
    system = WORK / 'android/android36/lib64'
    vendor = DESKTOP / 'vendor/lib64'
    provided = {Path(guest).name for guest, _ in programs if guest.endswith('.so')}
    pending = [('libEGL.so', '/system/lib64/libEGL.so'), ('libGLESv2.so', '/system/lib64/libGLESv2.so')]
    pending += [(name, '/vendor/lib64/egl/' + name) for name in (
        'libEGL_emulation.so', 'libGLESv1_CM_emulation.so', 'libGLESv2_emulation.so')]
    allocator, mapper = build_android_gralloc()
    replacements = {allocator.name: allocator, mapper.name: mapper}
    composer = DESKTOP / 'vendor/bin/hw/android.hardware.graphics.composer3-service.ranchu'
    surfaceflinger = WORK / 'android/android36/bin/surfaceflinger'
    replacements.update({composer.name: composer, surfaceflinger.name: surfaceflinger})
    pending += [(allocator.name, '/vendor/bin/hw/' + allocator.name),
                (mapper.name, '/vendor/lib64/hw/' + mapper.name),
                (composer.name, '/vendor/bin/hw/' + composer.name),
                (surfaceflinger.name, '/system/bin/' + surfaceflinger.name)]
    health = DESKTOP / 'vendor/bin/hw/android.hardware.health-service.example'
    replacements[health.name] = health
    pending.append((health.name, '/vendor/bin/hw/' + health.name))
    for name in ('android.hardware.power-service.example', 'android.hardware.power.stats-service.example'):
        replacements[name] = power / 'bin/hw' / name
        pending.append((name, '/vendor/bin/hw/' + name))
    for name in ('hwservicemanager', 'hw/android.hidl.allocator@1.0-service'):
        binary = DESKTOP / 'system_ext/bin' / name
        replacements[binary.name] = binary
        pending.append((binary.name, '/system_ext/bin/' + name))
    gatekeeper_binary = gatekeeper / 'bin/hw/android.hardware.gatekeeper-service.nonsecure'
    replacements[gatekeeper_binary.name] = gatekeeper_binary
    pending.append((gatekeeper_binary.name, '/vendor/bin/hw/' + gatekeeper_binary.name))
    keymint = DESKTOP / 'vendor/bin/hw/android.hardware.security.keymint-service'
    replacements[keymint.name] = keymint
    pending.append((keymint.name, '/vendor/bin/hw/' + keymint.name))
    audio = DESKTOP / 'vendor/bin/hw/android.hardware.audio.service'
    replacements[audio.name] = audio
    pending.append((audio.name, '/vendor/bin/hw/' + audio.name))
    for name in ('android.hardware.audio@7.1-impl.ranchu.so',
                 'android.hardware.audio.effect@7.0-impl.so',
                 'android.hardware.audio.legacy@7.1-impl.ranchu.so', 'audio.primary.default.so',
                 'audio.r_submix.default.so'):
        replacements[name] = vendor / 'hw' / name
        pending.append((name, '/vendor/lib64/hw/' + name))
    pending.append(('libaudiohal@7.1.so', '/system/lib64/libaudiohal@7.1.so'))
    result = []
    while pending:
        name, guest = pending.pop()
        if name in provided:
            continue
        path = replacements.get(name) or next((base / name for base in (system, vendor, vendor / 'egl', DESKTOP / 'system_ext/lib64', power / 'lib64', gatekeeper / 'lib64') if (base / name).is_file()), None)
        if path is None:
            raise RuntimeError('Missing graphics dependency: ' + name)
        provided.add(name)
        result.append((guest, path))
        for dependency in re.findall(r'Shared library: \[(.*?)\]',
                subprocess.check_output([llvm / 'llvm-readelf', '-d', path], text=True)):
            pending.append((dependency, '/system/lib64/' + dependency))
    return result


def android_service_interfaces(out):
    modules = prepare_android_modules()['modules']
    lines = ['static const struct { const char *name, *version_name; long long version, archive_size; } apex_modules[] = {']
    for module in modules:
        lines.append('{%s,%s,%d,%d},' % (json.dumps(module['name']), json.dumps(module['version_name']),
                     module['version'], (MODULES / module['archive']).stat().st_size))
    (out / 'apex-modules.h').write_text('\n'.join(lines + ['};']) + '\n')
    generated = out / 'android-aidl'
    if generated.exists():
        shutil.rmtree(generated)
    generated.mkdir()
    aidl = Path('/opt/homebrew/share/android-commandlinetools/build-tools/36.0.0/aidl')
    for kind in ('apex', 'suspend', 'installd'):
        directory = EXP / 'services' / kind
        origin = json.loads((directory / 'sources.json').read_text())
        for name, expected in origin['files'].items():
            if hashlib.sha256((directory / name).read_bytes()).hexdigest() != expected:
                raise RuntimeError('Android interface source checksum mismatch: ' + name)
        for source in sorted(directory.rglob('*.aidl')):
            run([aidl, '--lang=ndk', '--structured', '--min_sdk_version=35',
                 *(['--stability=vintf'] if kind == 'suspend' else []), '-I' + str(directory),
                 '--out=' + str(generated), '--header_out=' + str(generated), source])
    return generated


def android_service_headers():
    directory = WORK / 'android/binder-include/android'
    directory.mkdir(parents=True, exist_ok=True)
    sources = json.loads((EXP / 'services/sources.json').read_text())
    for item in sources['headers']:
        path = directory / item['name']
        if not path.exists():
            path.write_bytes(base64.b64decode(urllib.request.urlopen(item['url'], timeout=30).read()))
        if hashlib.sha256(path.read_bytes()).hexdigest() != item['sha256']:
            raise RuntimeError('Binder header checksum mismatch: ' + item['name'])
    return directory.parent

def android_property_info(out):
    item = json.loads((EXP / 'services/sources.json').read_text())['property_service']
    archive = WORK / 'android/property-service.tar.gz'
    if not archive.exists():
        archive.write_bytes(urllib.request.urlopen(item['url'], timeout=30).read())
    if hashlib.sha256(archive.read_bytes()).hexdigest() != item['sha256']:
        raise RuntimeError('Property serializer source checksum mismatch')
    source = WORK / 'android/property-service'
    with tarfile.open(archive) as package:
        for member in package:
            if not member.issym():
                package.extract(member, source, filter='data')
    serializer = source / 'libpropertyinfoserializer'
    base = WORK / 'android/libbase'
    run(['xcrun', 'clang++', '-std=c++20', '-O2',
         '-I', serializer / 'include', '-I', source / 'libpropertyinfoparser/include',
         '-I', base / 'include', EXP / 'services/property-info.cpp',
         *[serializer / name for name in ('property_info_serializer.cpp', 'trie_builder.cpp', 'trie_serializer.cpp')],
         source / 'libpropertyinfoparser/property_info_parser.cpp',
         base / 'strings.cpp', base / 'stringprintf.cpp', base / 'posix_strerror_r.cpp',
         '-o', out / 'property-info-builder'])
    run([out / 'property-info-builder', out / 'property_info'])
    return out / 'property_info'

def android_service_policy(out):
    platform_policy = (WORK / 'android/android36/etc/selinux/plat_sepolicy.cil').read_text()
    declarations = [line for line in platform_policy.splitlines()
                    if re.match(r'^\((?:class|classcommon|classorder|common|sid|sidorder) ', line)]
    classes = re.findall(r'^\(class (\w+) ', platform_policy, re.M)
    sids = re.findall(r'^\(sid (\w+)\)', platform_policy, re.M)
    if 'service_manager' not in classes or not {'kernel', 'security', 'unlabeled', 'init'} <= set(sids):
        raise RuntimeError('Android SELinux declarations changed')
    rules = [(EXP / 'services/nce-policy.cil').read_text()]
    bpf_labels = [line for line in platform_policy.splitlines() if line.startswith('(genfscon bpf ')]
    bpf_types = sorted({re.search(r'\(u object_r (\w+) ', line)[1] for line in bpf_labels})
    rules += [f'(type {name})' for name in bpf_types]
    rules += ['(typeattributeset nce_all (' + ' '.join(bpf_types) + '))',
              '(allow nce_all fs_bpf (filesystem (associate)))', *bpf_labels]
    rules += [f'(sidcontext {name} (u r nce_t ((s0) (s0))))' for name in sids]
    rules += [f'(allow nce_process nce_all ({name} (all)))' for name in classes if name not in ('service_manager', 'hwservice_manager')]
    path = out / 'nce-policy.cil'
    path.write_text('\n'.join(declarations + rules) + '\n')
    return path

def android_data(out):
    root = WORK / 'android/android36'
    java = Path('/opt/homebrew/opt/openjdk@17/bin')
    sdk = Path('/opt/homebrew/share/android-commandlinetools')
    classes = out / 'java-classes'
    classes.mkdir(exist_ok=True)
    run([java / 'javac', '--release', '11', '-d', classes, EXP / 'art/NceWorkload.java', EXP / 'art/NceIntrinsics.java', EXP / 'art/NceInlining.java', EXP / 'art/NceProxy.java', EXP / 'art/NceVolatile.java', EXP / 'art/NceInterpreter.java', EXP / 'art/NceCompilerPaths.java', EXP / 'art/NceFastBaseline.java'])
    run([java / 'java', '-cp', sdk / 'build-tools/36.0.0/lib/d8.jar', 'com.android.tools.r8.D8',
         '--min-api', '35', '--lib', sdk / 'platforms/android-35/android.jar', '--output', classes,
         *sorted(classes.glob('*.class'))])
    prepare_interface_object_calls(classes / 'classes.dex')
    files = [('/data/nce/nce-policy.cil', android_service_policy(out)),
             ('/data/nce/property_info', android_property_info(out)),
             ('/data/nce/NceWorkload.dex', classes / 'classes.dex'),
             ('/linkerconfig/ld.config.txt', EXP / 'art/ld.config.txt'),
             ('/system/etc/public.libraries.txt', EXP / 'art/public.libraries.txt'),
             ('/system/etc/llndk.libraries.txt', root / 'etc/llndk.libraries.txt'),
             ('/system/etc/preloaded-classes', root / 'etc/preloaded-classes'),
             ('/vendor/etc/permissions/nce-features.xml', EXP / 'services/nce-features.xml')]
    fragments = sorted((root / 'etc/classpaths').glob('*classpath.pb'))
    fragments += sorted(root.glob('com.android.*/etc/classpaths/*classpath.pb'))
    jars = set()
    for fragment in fragments:
        relative = fragment.relative_to(root)
        guest = ('/system/' if relative.parts[0] == 'etc' else '/apex/') + str(relative)
        files.append((guest, fragment))
        # Copy referenced files; the SDK's derive_classpath binary interprets
        # protobuf types, SDK conditions and ordering at runtime.
        jars.update(path.decode() for path in re.findall(rb'/(?:system|apex)/[a-zA-Z0-9_./@-]+\.jar', fragment.read_bytes()))
    for guest in sorted(jars):
        if not guest.startswith(('/system/', '/apex/')) or '..' in Path(guest).parts:
            raise RuntimeError('Unexpected classpath location: ' + guest)
        source = root / guest.split('/', 2)[2]
        if not source.is_file():
            raise RuntimeError('Missing classpath resource: ' + guest)
        files.append((guest, source))
    # Reuse the pinned platform names with the prototype's service policy.
    # Unknown names keep the denied label; the SDK wildcard is not imported.
    service_names = set()
    for source in (root / 'etc/selinux/plat_service_contexts',
                   DESKTOP / 'vendor/etc/selinux/vendor_service_contexts'):
        for line in source.read_text().splitlines():
            fields = line.partition('#')[0].split()
            if fields and fields[0] != '*':
                if len(fields) != 2:
                    raise RuntimeError(f'Invalid service context: {source}: {line}')
                service_names.add(fields[0])
    contexts = {name: 'u:object_r:nce_service:s0' for name in service_names}
    for line in (EXP / 'services/service_contexts').read_text().splitlines():
        name, context = line.split()
        contexts[name] = context
    context_file = out / 'plat_service_contexts'
    context_file.write_text(''.join(f'{name} {context}\n' for name, context in sorted(contexts.items())))
    files += [('/system/etc/selinux/plat_service_contexts', context_file)]
    fragment = 'vendor/etc/vintf/manifest/android.hardware.graphics.gralloc.ranchu.xml'
    files.append(('/' + fragment, DESKTOP / fragment))
    fragment = 'vendor/etc/vintf/manifest/android.hardware.health-service.example.xml'
    files.append(('/' + fragment, DESKTOP / fragment))
    files.append(('/vendor/etc/vintf/manifest.xml', EXP / 'services/vendor-manifest.xml'))
    power = prepare_vendor_module('com.android.hardware.power')
    files += [('/vendor/etc/vintf/manifest/' + path.name, path)
              for path in sorted((power / 'etc/vintf').glob('*.xml'))]
    gatekeeper = prepare_vendor_module('com.android.hardware.gatekeeper.nonsecure')
    files += [('/vendor/etc/vintf/manifest/' + path.name, path)
              for path in sorted((gatekeeper / 'etc/vintf').glob('*.xml'))]
    for service in ('keymint', 'secureclock', 'sharedsecret'):
        fragment = f'vendor/etc/vintf/manifest/android.hardware.security.{service}-service.xml'
        files.append(('/' + fragment, DESKTOP / fragment))
    key_contexts = out / 'plat_keystore2_key_contexts'
    namespaces = re.findall(r'^\s*(\d+)\s+u:', (root / 'etc/selinux/plat_keystore2_key_contexts').read_text(), re.M)
    key_contexts.write_text(''.join(f'{value} u:object_r:nce_t:s0\n' for value in namespaces))
    files.append(('/system/etc/selinux/plat_keystore2_key_contexts', key_contexts))
    hardware_names = set()
    for source in (root / 'etc/selinux/plat_hwservice_contexts',
                   DESKTOP / 'vendor/etc/selinux/vendor_hwservice_contexts'):
        for line in source.read_text().splitlines():
            fields = line.partition('#')[0].split()
            if fields and fields[0] != '*':
                hardware_names.add(fields[0])
    hardware_contexts = out / 'plat_hwservice_contexts'
    hardware_contexts.write_text(''.join(f'{name} u:object_r:nce_service:s0\n'
                                        for name in sorted(hardware_names)) + '* u:object_r:nce_denied:s0\n')
    files += [('/system/etc/selinux/plat_hwservice_contexts', hardware_contexts)]
    audio_manifest = 'vendor/etc/vintf/manifest/android.hardware.audio@7.1-impl.ranchu.xml'
    files += [('/' + audio_manifest, DESKTOP / audio_manifest)]
    for source in sorted((DESKTOP / 'vendor/etc').glob('*audio*.xml')):
        files.append(('/vendor/etc/' + source.name, source))
    for name in ('default_volume_tables.xml',):
        files.append(('/vendor/etc/' + name, DESKTOP / 'vendor/etc' / name))
    files += [('/system/etc/selinux/plat_seapp_contexts', EXP / 'services/seapp_contexts')]
    files += [('/system/etc/selinux/plat_mac_permissions.xml', root / 'etc/selinux/plat_mac_permissions.xml')]
    files += [('/system/etc/selinux/plat_file_contexts', EXP / 'services/file_contexts')]
    files += [('/system/usr/keylayout/NCE_Touch.kl', EXP / 'services/NCE_Touch.kl'),
              ('/system/usr/idc/NCE_Touch.idc', EXP / 'services/NCE_Touch.idc')]
    files += [('/system/' + str(path.relative_to(root)), path)
              for path in sorted((root / 'usr').rglob('*')) if path.is_file()]
    files += [('/vendor/etc/fstab.nce', EXP / 'services/fstab.nce')]
    files += [('/system/etc/init/netbpfload.rc', root / 'etc/init/netbpfload.rc')]
    files += [('/system/etc/xtables.lock', root / 'etc/xtables.lock')]
    files += [('/apex/' + str(path.relative_to(root)), path)
              for path in sorted((root / 'com.android.tethering/etc/bpf').rglob('*')) if path.is_file()]
    files += [(f'/system/etc/{name}.json', EXP / 'services' / f'{name}.json')
              for name in ('cgroups', 'task_profiles')]
    files += [('/apex/com.android.i18n/etc/icu/icudt76l.dat', root / 'com.android.i18n/etc/icu/icudt76l.dat')]
    timezone = root / 'com.android.tzdata/etc/tz'
    files += [(f'/apex/com.android.tzdata/etc/tz/{path.relative_to(timezone)}', path)
              for path in sorted(timezone.rglob('*')) if path.is_file()]
    framework_classes = out / 'framework-classes'
    shutil.rmtree(framework_classes, ignore_errors=True)
    framework_classes.mkdir()
    run([java / 'javac', '-source', '8', '-target', '8',
         '-bootclasspath', sdk / 'platforms/android-35/android.jar',
         '-d', framework_classes, EXP / 'services/NceFramework.java', EXP / 'services/NceZygoteChild.java',
         EXP / 'services/NceSurface.java', EXP / 'services/NceShutdown.java', EXP / 'services/NceKeystore.java',
         EXP / 'services/NceAppData.java', EXP / 'services/NceStorage.java', EXP / 'services/NceClockUi.java'])
    run([java / 'java', '-cp', sdk / 'build-tools/36.0.0/lib/d8.jar', 'com.android.tools.r8.D8',
         '--min-api', '35', '--lib', sdk / 'platforms/android-35/android.jar', '--output', framework_classes,
         *sorted(framework_classes.glob('*.class'))])
    framework_jar = out / 'nce-framework.jar'
    with zipfile.ZipFile(framework_jar, 'w') as jar:
        jar.writestr(zipfile.ZipInfo('classes.dex'), (framework_classes / 'classes.dex').read_bytes())
    files += [('/system/framework/nce-framework.jar', framework_jar)]
    files += [('/system/framework/framework-res.apk', root / 'framework/framework-res.apk')]
    files += [('/system/etc/event-log-tags', root / 'etc/event-log-tags')]
    desktop_apps = (
        'system/priv-app/GooglePackageInstaller/GooglePackageInstaller.apk',
        'system/priv-app/SettingsProvider/SettingsProvider.apk',
        'system/priv-app/IntentResolver/IntentResolver.apk',
        'system/priv-app/InputDevices/InputDevices.apk',
        'system/priv-app/FusedLocation/FusedLocation.apk',
        'system/priv-app/Shell/Shell.apk',
        'system/priv-app/NetworkStackGoogle/NetworkStackGoogle.apk',
        'system/priv-app/DocumentsUIGoogle/DocumentsUIGoogle.apk',
        'system/priv-app/DownloadProvider/DownloadProvider.apk',
        'system/priv-app/DownloadProviderUi/DownloadProviderUi.apk',
        'system/priv-app/ExternalStorageProvider/ExternalStorageProvider.apk',
        'system/app/KeyChain/KeyChain.apk',
        'system/app/GoogleExtShared/GoogleExtShared.apk',
        'system_ext/priv-app/SystemUIGoogle/SystemUIGoogle.apk',
        'system_ext/priv-app/SettingsGoogle/SettingsGoogle.apk',
        'product/app/PrebuiltDeskClockGoogle/PrebuiltDeskClockGoogle.apk',
    )
    for name in desktop_apps:
        source = DESKTOP / name
        if not source.is_file():
            raise RuntimeError('Missing desktop APK: ' + name)
        files.append(('/' + name, source))
    files += prepare_aosp_components()
    for module in ('com.android.permission', 'com.android.configinfrastructure',
                   'com.android.tethering', 'com.android.extservices', 'com.android.devicelock',
                   'com.android.healthfitness', 'com.android.appsearch', 'com.android.mediaprovider'):
        for directory in ('app', 'priv-app'):
            for source in sorted((root / module / directory).rglob('*.apk')):
                files.append(('/apex/' + str(source.relative_to(root)), source))
    for source in sorted((root / 'com.android.adservices/app').rglob('SdkSandboxGoogle.apk')):
        files.append(('/apex/' + str(source.relative_to(root)), source))
    for name in ('GoogleConfigOverlay.apk', 'RanchuCommonOverlay.apk',
                 'SystemUIGoogle__sdk_gphone16k_arm64__auto_generated_rro_product.apk',
                 'framework-res__sdk_gphone16k_arm64__auto_generated_rro_product.apk',
                 'framework-res__emulator__auto_generated_characteristics_rro.apk'):
        source = DESKTOP / 'product/overlay' / name
        files.append(('/product/overlay/' + name, source))
    for partition in ('system_ext', 'product'):
        for source in sorted((DESKTOP / partition / 'etc/permissions').glob('privapp-permissions*.xml')):
            files.append(('/' + str(source.relative_to(DESKTOP)), source))
    for name in ('com.android.systemui.xml', 'com.android.launcher3.xml', 'androidx.window.extensions.xml', 'androidx.window.sidecar.xml'):
        source = DESKTOP / 'system_ext/etc/permissions' / name
        files.append(('/' + str(source.relative_to(DESKTOP)), source))

    files += [('/system/etc/vintf/' + name, root / 'etc/vintf' / name)
              for name in ('manifest.xml', 'manifest/android.system.suspend-service.xml',
                           'manifest/android.system.net.netd-service.xml', 'manifest/android.system.keystore2-service.xml')]
    files += [('/system/etc/vintf/' + path.name, path)
              for path in sorted((root / 'etc/vintf').glob('compatibility_matrix*.xml'))]
    files.append(('/vendor/etc/vintf/compatibility_matrix.xml',
                  DESKTOP / 'vendor/etc/vintf/compatibility_matrix.xml'))
    files += [('/' + str(path.relative_to(DESKTOP)), path)
              for path in sorted((DESKTOP / 'system_ext/etc/vintf').rglob('*.xml'))]
    for directory in ('permissions', 'sysconfig', 'compatconfig'):
        for path in sorted((root / 'etc' / directory).rglob('*')):
            if path.is_file():
                files.append(('/system/' + str(path.relative_to(root)), path))
        for module in sorted(root.glob('com.android.*')):
            for path in sorted((module / 'etc' / directory).rglob('*')):
                if path.is_file():
                    files.append(('/apex/' + str(path.relative_to(root)), path))

    # App uses-library declarations are independent of the boot/server classpaths.
    registered = {guest for guest, _ in files}
    for guest, source in tuple(files):
        if '/etc/permissions/' not in guest or not guest.endswith('.xml'):
            continue
        for library in ET.parse(source).getroot().iter('library'):
            path = library.get('file', '')
            if not path.endswith('.jar') or path in registered:
                continue
            if not path.startswith(('/system/', '/apex/', '/system_ext/', '/product/')) or '..' in Path(path).parts:
                raise RuntimeError('Unexpected shared Java library: ' + path)
            origin = root / path.split('/', 2)[2] if path.startswith(('/system/', '/apex/')) else DESKTOP / path[1:]
            if not origin.is_file():
                raise RuntimeError('Missing shared Java library: ' + path)
            files.append((path, origin))
            registered.add(path)

    # VDEX stores verifier dependencies; OAT machine code requires separate NCE preparation.
    vdex_files = []
    for guest, source in files:
        if source.suffix not in ('.apk', '.jar'):
            continue
        relative = Path('oat/arm64') / (source.stem + '.vdex')
        cache = source.parent / relative
        destination = str(Path(guest).parent / relative)
        if cache.is_file() and destination not in registered:
            vdex_files.append((destination, cache))
            registered.add(destination)
    files += vdex_files
    print(f'Included {len(vdex_files)} SDK VDEX caches, {sum(path.stat().st_size for _, path in vdex_files)} bytes', flush=True)

    config_names = ('package.map', 'flag.map', 'flag.val', 'flag.info')
    files += [(f'/system/etc/aconfig/{name}', root / 'etc/aconfig' / name) for name in config_names]
    for module in sorted(root.glob('com.android.*')):
        if all((module / 'etc' / name).is_file() for name in config_names):
            files += [(f'/apex/{module.name}/etc/{name}', module / 'etc' / name) for name in config_names]
    module_manifest = json.loads((MODULES / 'manifest.json').read_text())
    for module in module_manifest['modules']:
        name = module['name']
        files += [(f'/system/apex/{name}.apex', MODULES / module['archive']),
                  (f'/apex/{name}/apex_manifest.pb', root / name / 'apex_manifest.pb')]
    for path in sorted((MODULES / 'config').rglob('*')):
        if path.is_file():
            partition, name = path.relative_to(MODULES / 'config').parts
            files.append((f'/{partition}/etc/aconfig/{name}', path))
    files += [(f'/system/fonts/{path.name}', path) for path in sorted((root / 'fonts').glob('*')) if path.is_file()]
    files += [(f'/system/etc/{name}', root / 'etc' / name) for name in ('fonts.xml', 'font_fallback.xml')]
    return files




def apk_native_libraries(out, assets):
    """登记未压缩、按客体页对齐的 APK 原生库，保留原 APK 字节。"""
    programs, locations = [], {}
    for guest, source in assets:
        if not guest.endswith('.apk'):
            continue
        with zipfile.ZipFile(source) as archive, source.open('rb') as raw:
            for item in archive.infolist():
                if not item.filename.startswith('lib/arm64-v8a/') or not item.filename.endswith('.so'):
                    continue
                if item.compress_type != zipfile.ZIP_STORED or item.flag_bits & 1:
                    raise RuntimeError(f'{source}: native library must be stored without compression or encryption')
                raw.seek(item.header_offset)
                header = raw.read(30)
                if len(header) != 30 or header[:4] != b'PK\x03\x04':
                    raise RuntimeError(f'{source}: invalid ZIP local header')
                name_size, extra_size = struct.unpack_from('<HH', header, 26)
                offset = item.header_offset + 30 + name_size + extra_size
                if offset % PAGE:
                    raise RuntimeError(f'{source}: native library is not aligned to {PAGE} bytes')
                data = archive.read(item)
                raw.seek(offset)
                if raw.read(item.file_size) != data:
                    raise RuntimeError(f'{source}: ZIP native library bytes differ')
                path = out / 'apk-libraries' / hashlib.sha256(guest.encode()).hexdigest()[:16] / Path(item.filename).name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
                name = guest + '!/' + item.filename
                if name in locations:
                    raise RuntimeError(f'{source}: duplicate native library entry {item.filename}')
                programs.append((name, path))
                locations[name] = (guest, offset)
    return programs, locations


def native_library_zip(out):
    path = out / 'nce-native-libraries.apk'
    with zipfile.ZipFile(path, 'w') as archive:
        for name in ('libnce-cfi-a.so', 'libnce-cfi-b.so'):
            item = zipfile.ZipInfo('lib/arm64-v8a/' + name)
            padding = -(archive.fp.tell() + 30 + len(item.filename)) % PAGE
            if 0 < padding < 4:
                padding += PAGE
            if padding:
                item.extra = struct.pack('<HH', 0xffff, padding - 4) + bytes(padding - 4)
            archive.writestr(item, (out / name).read_bytes())
    return '/system/etc/nce-native-libraries.apk', path


def direct_branch(word, address):
    """返回直接分支目标及是否存在顺序执行路径。"""
    if word & 0x7c000000 == 0x14000000:
        bits, immediate = 26, word & 0x3ffffff
        fallthrough = bool(word & 0x80000000)
    elif word & 0xff000000 == 0x54000000 or word & 0x7e000000 == 0x34000000:
        bits, immediate, fallthrough = 19, (word >> 5) & 0x7ffff, True
    elif word & 0x7e000000 == 0x36000000:
        bits, immediate, fallthrough = 14, (word >> 5) & 0x3fff, True
    else:
        return None, True
    displacement = immediate - ((immediate >> (bits - 1)) << bits)
    return address + displacement * 4, fallthrough


def extend_function_ranges(path, data, section, ranges, objects, sections=(), functions=None):
    # Stripped libunwind assembly can lack STT_FUNC. Follow only direct branches
    # from classified instructions; unreachable bytes retain the existing checks.
    base, length = section[3], section[5]
    classified = bytearray(length // 4)
    for lo, hi in ranges:
        if lo % 4 or hi % 4:
            raise RuntimeError(f'{path}: unaligned instruction span')
        classified[(lo - base) // 4:(hi - base) // 4] = b'\1' * ((hi - lo) // 4)
    pending = []
    for source_index, source_ranges in (functions or {}).items():
        if not 0 <= source_index < len(sections):
            continue
        source = sections[source_index]
        if source == section or not source[2] & 4:
            continue
        for lo, hi in set(source_ranges):
            if not source[3] <= lo < hi <= source[3] + source[5] or lo % 4 or hi % 4:
                continue
            for address in range(lo, hi, 4):
                word, = struct.unpack_from('<I', data, source[4] + address - source[3])
                target, _ = direct_branch(word, address)
                if target is not None and base <= target < base + length:
                    pending.append(target)
    for index, known in enumerate(classified):
        if known:
            word, = struct.unpack_from('<I', data, section[4] + index * 4)
            target, _ = direct_branch(word, base + index * 4)
            if target is not None and base <= target < base + length:
                pending.append(target)
    while pending:
        address = pending.pop()
        while base <= address < base + length and not classified[(address - base) // 4]:
            if any(lo <= address < hi for lo, hi in objects):
                raise RuntimeError(f'{path}: direct branch enters an object symbol at {address:x}')
            word, = struct.unpack_from('<I', data, section[4] + address - base)
            if word == 0:
                raise RuntimeError(f'{path}: direct branch reaches undefined instruction at {address:x}')
            classified[(address - base) // 4] = 1
            target, fallthrough = direct_branch(word, address)
            if target is not None and base <= target < base + length:
                pending.append(target)
            # BR/RET terminate this path. BLR returns to the next instruction.
            if (not fallthrough or
                    word & 0xfe000000 == 0xd6000000 and word & 0xfffffc1f != 0xd63f0000 or
                    word & 0xffe0001f in (0xd4200000, 0xd4400000)):
                break
            address += 4
    merged, start = [], None
    for index, known in enumerate([*classified, 0]):
        if known and start is None:
            start = base + index * 4
        elif not known and start is not None:
            merged.append((start, base + index * 4))
            start = None
    return merged


def inspect(path, shared=False, function_symbols=False, nce_oat=False):
    data = path.read_bytes()
    if len(data) < 64 or data[:7] != b'\x7fELF\x02\x01\x01' or struct.unpack_from('<HH', data, 16) != (3, 183):
        raise RuntimeError(f'{path}: requires AArch64 ET_DYN')
    entry, phoff, shoff = struct.unpack_from('<QQQ', data, 24)
    phentsize, phnum, shentsize, shnum = struct.unpack_from('<HHHH', data, 54)
    if phentsize != 56 or shentsize != 64:
        raise RuntimeError('Unexpected ELF table format')
    if phoff + phnum * phentsize > len(data) or shoff + shnum * shentsize > len(data):
        raise RuntimeError(f'{path}: truncated ELF tables')
    phdrs = [struct.unpack_from('<IIQQQQQQ', data, phoff + i * phentsize) for i in range(phnum)]
    loads = [p for p in phdrs if p[0] == 1]
    if loads != sorted(loads, key=lambda p: p[3]):
        raise RuntimeError(f'{path}: load segments are out of address order')
    interpreters = [p for p in phdrs if p[0] == 3]
    if len(interpreters) > 1:
        raise RuntimeError(f'{path}: multiple ELF interpreters')
    for p in interpreters:
        if p[2] + p[5] > len(data) or data[p[2]:p[2] + p[5]] not in [i.encode() + b'\0' for i in (INTERPRETER, ANDROID_INTERPRETER, ANDROID_BOOTSTRAP_INTERPRETER)]:
            raise RuntimeError(f'{path}: unsupported ELF interpreter')
    segments = []
    for _, flags, offset, va, _, filesz, memsz, align in loads:
        if (align > MAX_ALIGNMENT or align and align & (align - 1) or
                (va - offset) % max(PAGE, align) or filesz > memsz or offset + filesz > len(data)):
            raise RuntimeError(f'{path}: unsupported segment layout')
        if va + memsz > PROGRAM_LIMIT:
            raise RuntimeError(f'{path}: exceeds the current per-program range')
        if flags & 1 and memsz:
            if flags & 2:
                raise RuntimeError(f'{path}: writable executable segment')
            lo, hi = va & -PAGE, (va + memsz + PAGE - 1) & -PAGE
            if any(p[1] & 2 and (p[3] & -PAGE) < hi and ((p[3] + p[6] + PAGE - 1) & -PAGE) > lo for p in loads):
                raise RuntimeError(f'{path}: executable and writable segments share a host page')
            segments.append((lo, hi - lo))
    if any(a + size > b for (a, size), (b, _) in zip(segments, segments[1:])):
        raise RuntimeError(f'{path}: executable segments share a host page')
    if not segments or not (shared and entry == 0) and not any(p[1] & 1 and p[3] <= entry < p[3] + p[5] for p in loads):
        raise RuntimeError(f'{path}: missing executable entry')
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * shentsize) for i in range(shnum)]
    if nce_oat:
        rodata = [section for section in sections if section[1] == 1 and section[2] == 2
                  and data[section[4]:section[4] + 8] == b'oat\nn01\0']
        if len(rodata) != 1:
            raise RuntimeError('Expected NCE n01 OAT header for literal pool parsing')
    mappings = {}
    functions = {}
    objects = {}
    symbol_sources = [(data, sections, False)]
    if function_symbols:
        names = sections[struct.unpack_from('<H', data, 62)[0]]
        names = data[names[4]:names[4] + names[5]]
        for section in sections:
            if names[section[0]:names.find(b'\0', section[0])] != b'.gnu_debugdata':
                continue
            debug = lzma.decompress(data[section[4]:section[4] + section[5]], memlimit=64 * 1024 * 1024)
            if debug[:7] != b'\x7fELF\x02\x01\x01' or len(debug) < 64:
                raise RuntimeError(f'{path}: invalid mini debug ELF')
            off, = struct.unpack_from('<Q', debug, 40)
            entsize, count = struct.unpack_from('<HH', debug, 58)
            if entsize != 64 or off + count * entsize > len(debug):
                raise RuntimeError(f'{path}: invalid mini debug sections')
            symbol_sources.append((debug, [struct.unpack_from('<IIQQQQIIQQ', debug, off + i * 64)
                                            for i in range(count)], True))
    for symbol_bytes, symbol_sections, mini in symbol_sources:
        for symtab in symbol_sections:
            if symtab[1] != 2 and not (function_symbols and symtab[1] == 11):
                continue
            if symtab[9] != 24 or symtab[5] % 24 or symtab[6] >= len(symbol_sections) or symtab[4] + symtab[5] > len(symbol_bytes):
                raise RuntimeError(f'{path}: invalid symbol table')
            strings = symbol_sections[symtab[6]]
            if strings[4] + strings[5] > len(symbol_bytes):
                raise RuntimeError(f'{path}: truncated symbol strings')
            strings = symbol_bytes[strings[4]:strings[4] + strings[5]]
            for off in range(symtab[4], symtab[4] + symtab[5], symtab[9]):
                name, info, _, sec, addr, size = struct.unpack_from('<IBBHQQ', symbol_bytes, off)
                end = strings.find(b'\0', name)
                if end < 0:
                    raise RuntimeError(f'{path}: invalid symbol name')
                name = strings[name:end].decode()
                if mini:
                    sec = next((i for i, s in enumerate(sections) if s[2] & 4 and s[3] <= addr < s[3] + s[5]), -1)
                if not mini and re.fullmatch(r'\$[xd](?:\..*)?', name):
                    mappings.setdefault(sec, set()).add((addr, name[1]))
                if info & 15 == 2 and size:
                    functions.setdefault(sec, []).append((addr, addr + size))
                if info & 15 == 1 and size:
                    objects.setdefault(sec, []).append((addr, addr + size))
    patches = []
    instruction_ranges = []
    for index, sec in enumerate(sections):
        if not sec[2] & 4 or not sec[5]:
            continue
        if not any(p[1] & 1 and p[3] <= sec[3] and sec[3] + sec[5] <= p[3] + p[5] and
                   sec[4] - p[2] == sec[3] - p[3] for p in loads):
            raise RuntimeError(f'{path}: executable section is outside its file segment')
        markers = sorted(mappings.get(index, []))
        inferred = False
        # LLD's AArch64 PLT carries SHF_AARCH64_PURECODE instead of $x symbols.
        if not markers and sec[2] & 0x20000000:
            markers = [(sec[3], 'x')]
        if not markers and function_symbols:
            # Pinned Android platform binaries retain function boundaries but
            # can omit mapping symbols. Scan only those function ranges.
            ranges = sorted(set(functions.get(index, [])))
            inferred = True
            merged = []
            for lo, hi in ranges:
                # Android CFI exports can point at a 4-byte branch table entry
                # while retaining the target function's size in .dynsym.
                if sec[3] <= lo <= sec[3] + sec[5] - 4 and hi > sec[3] + sec[5]:
                    word, = struct.unpack_from('<I', data, sec[4] + lo - sec[3])
                    displacement = (word & 0x3ffffff) - ((word & 0x2000000) << 1)
                    target = lo + displacement * 4
                    if word & 0xfc000000 == 0x14000000 and any(
                            sec[3] <= first <= target < last <= sec[3] + sec[5]
                            for first, last in ranges):
                        hi = lo + 4
                if lo < sec[3] or hi > sec[3] + sec[5]:
                    raise RuntimeError(f'{path}: function symbol outside executable section')
                if merged and lo <= merged[-1][1]:
                    merged[-1] = (merged[-1][0], max(hi, merged[-1][1]))
                else:
                    merged.append((lo, hi))
            # Resolve assembly gaps only when they contain instructions requiring rewriting.
            gaps = [(sec[3], merged[0][0])] if merged else [(sec[3], sec[3] + sec[5])]
            gaps += [(hi, merged[i + 1][0] if i + 1 < len(merged) else sec[3] + sec[5])
                     for i, (_, hi) in enumerate(merged)]
            if not nce_oat and any(x18_instruction(word) or word & 0xffe0001f == 0xd4000001 or
                   word & 0xffffffe0 in (0xd53bd040, 0xd51bd040, 0xd53b0020)
                   for lo, hi in gaps for (word,) in struct.iter_unpack('<I', data[sec[4] + lo - sec[3]:sec[4] + hi - sec[3]])):
                merged = extend_function_ranges(path, data, sec, merged, objects.get(index, []), sections, functions)
            if nce_oat:
                merged = art_aot.code_ranges(data, sec[4], sec[3], merged)
            markers = [(sec[3], 'd')] if not merged or merged[0][0] > sec[3] else []
            for lo, hi in merged:
                markers.append((lo, 'x'))
                if hi < sec[3] + sec[5]: markers.append((hi, 'd'))
        if not markers or markers[0][0] != sec[3]:
            raise RuntimeError(f'{path}: executable section lacks mapping symbols')
        for i, (begin, kind) in enumerate(markers):
            end = markers[i + 1][0] if i + 1 < len(markers) else sec[3] + sec[5]
            if begin >= end or end > sec[3] + sec[5]:
                raise RuntimeError(f'{path}: invalid instruction mapping span')
            if kind != 'x':
                if inferred and not nce_oat:
                    for address in range(begin, end - 3, 4):
                        word, = struct.unpack_from('<I', data, sec[4] + address - sec[3])
                        if word & 0xffe0001f == 0xd4000001 or word & 0xffffffe0 in (0xd53bd040, 0xd51bd040, 0xd53b0020) or x18_instruction(word):
                            raise RuntimeError(f'{path}: patch candidate outside function symbols at {address:x}')
                continue
            if begin % 4 or end % 4:
                raise RuntimeError(f'{path}: unaligned instruction span')
            instruction_ranges.append((begin, end))
            for address in range(begin, end, 4):
                word, = struct.unpack_from('<I', data, sec[4] + address - sec[3])
                if word & 0xffe0001f == 0xd4000001 or word & 0xffffffe0 in (0xd53bd040, 0xd51bd040, 0xd53b0020) or x18_instruction(word):
                    patches.append((address, word))
    return dict(data=data, entry=entry, phoff=phoff, phsize=phnum*phentsize, segments=segments, patches=sorted(patches),
                span=(max(p[3] + p[6] for p in loads) + PAGE - 1) & -PAGE,
                alignment=max(PAGE, max(p[7] for p in loads)), instruction_ranges=instruction_ranges)


def elf_disassembly(llvm, path, elf):
    data = elf['data']
    shoff, = struct.unpack_from('<Q', data, 40)
    count, = struct.unpack_from('<H', data, 60)
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * 64) for i in range(count)]
    amended = None
    for section in sections:
        if section[1] not in (2, 11):
            continue
        strings = sections[section[6]]
        strings = data[strings[4]:strings[4] + strings[5]]
        for off in range(section[4], section[4] + section[5], 24):
            name, info, _, index, value, _ = struct.unpack_from('<IBBHQQ', data, off)
            if count <= index < 0xff00:
                symbol = strings[name:strings.index(b'\0', name)]
                if not symbol.startswith(b'rust_metadata_') or info & 15 != 1 or value:
                    raise RuntimeError(f'{path}: invalid section index on symbol {symbol!r}')
                # SDK stripping can remove .rustc while retaining its metadata symbol.
                # Adjust only the disassembler copy; loaded ELF bytes stay unchanged.
                if amended is None:
                    amended = bytearray(data)
                struct.pack_into('<H', amended, off + 6, 0xfff1)  # SHN_ABS
    if amended is None:
        return subprocess.check_output([llvm / 'llvm-objdump', '-d', path], text=True)
    with tempfile.NamedTemporaryFile(suffix='.so', dir=WORK) as temporary:
        temporary.write(amended)
        temporary.flush()
        return subprocess.check_output([llvm / 'llvm-objdump', '-d', temporary.name], text=True)


def check_x18_disassembly(path, elf, disassembly):
    patched = {address for address, _ in elf['patches']}
    for line in disassembly.splitlines():
        if not re.search(r'\b[wx]18\b', line):
            continue
        match = re.match(r'\s*([0-9a-f]+):', line)
        if not match:
            raise RuntimeError(f'{path}: unrecognized x18 disassembly: {line.strip()}')
        address = int(match[1], 16)
        if any(begin <= address < end for begin, end in elf['instruction_ranges']) and address not in patched:
            raise RuntimeError(f'{path}: unsupported use of x18: {line.strip()}')


def compiler_runtime(llvm, libc):
    source = WORK / 'compiler-rt-20.1.8.src'
    archive = WORK / (source.name + '.tar.xz')
    if not archive.exists():
        urllib.request.urlretrieve('https://github.com/llvm/llvm-project/releases/download/llvmorg-20.1.8/' + archive.name, archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != RT_SHA:
        raise RuntimeError('compiler-rt source archive hash mismatch')
    if not source.exists():
        with tarfile.open(archive) as tar:
            tar.extractall(WORK, filter='data')
    build = WORK / 'compiler-rt-build'
    build.mkdir(exist_ok=True)
    library = build / 'libbuiltins.a'
    if not library.exists() or not (build / 'muldc3.o').exists():
        builtins = source / 'lib/builtins'
        cmake = (builtins / 'CMakeLists.txt').read_text()
        files = re.search(r'set\(GENERIC_TF_SOURCES\s+(.*?)\)', cmake, re.S)[1].split()
        files += ['clzti2.c', 'ctzti2.c', 'udivti3.c', 'udivmodti4.c', 'divti3.c', 'modti3.c', 'umodti3.c', 'muldc3.c', 'mulsc3.c', 'aarch64/fp_mode.c']
        objects = []
        for name in files:
            obj = build / (Path(name).stem + '.o')
            run([llvm / 'clang', '--target=aarch64-linux-musl', '-ffixed-x18', '-mno-outline-atomics',
                 '-O2', '-fPIC', '-fno-builtin', '-fvisibility=hidden', '-isystem', libc / 'include',
                 '-c', builtins / name, '-o', obj])
            objects.append(obj)
        run([llvm / 'llvm-ar', 'rcs', library, *objects])
    return library


def dash(out, llvm, libc, runtime, resource):
    source = WORK / 'dash-0.5.12'
    archive = WORK / 'dash-0.5.12.tar.gz'
    if not archive.exists():
        urllib.request.urlretrieve('https://sources.buildroot.net/dash/dash-0.5.12.tar.gz', archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != DASH_SHA:
        raise RuntimeError('dash source archive hash mismatch')
    if not source.exists():
        with tarfile.open(archive) as tar:
            tar.extractall(WORK, filter='data')
    build = WORK / 'dash-build'
    build.mkdir(exist_ok=True)
    cc = build / 'target-cc'
    flags = [str(llvm / 'clang'), '--target=aarch64-linux-musl', '-ffixed-x18', '-mno-outline-atomics',
             '-nostdinc', '-isystem', str(libc / 'include'), '-isystem', str(resource), '-fPIE']
    link = ['-nostdlib', '-fuse-ld=lld', '-Wl,-pie,-dynamic-linker,' + INTERPRETER + ',-z,max-page-size=16384,-z,common-page-size=16384',
            str(libc / 'lib/Scrt1.o'), str(libc / 'lib/crti.o')]
    libraries = ['-L' + str(libc / 'lib'), '-lc', str(runtime), str(libc / 'lib/crtn.o')]
    cc.write_text('#!/usr/bin/env python3\nimport os,sys\n'
                  f'flags={json.dumps(flags)}\nargs=sys.argv[1:]\n'
                  f'if not any(a in args for a in ("-c","-S","-E")) and any(a.endswith((".c",".o")) for a in args):\n'
                  f' args={json.dumps(link)}+args+{json.dumps(libraries)}\n'
                  'os.execv(flags[0], flags+args)\n')
    cc.chmod(0o755)
    with (build / 'build.log').open('w') as log:
        if not (build / 'config.h').exists():
            run([source / 'configure', '--host=aarch64-linux-musl', '--build=aarch64-apple-darwin',
                 '--without-libedit', 'CC=' + str(cc), 'CC_FOR_BUILD=cc', 'CFLAGS=-O2 -g'],
                cwd=build, stdout=log, stderr=subprocess.STDOUT)
        # mksignames runs on macOS but must use the Linux signal numbers.
        generator = (source / 'src/mksignames.c').read_text()
        definitions = dict(re.findall(r'^#define\s+((?:SIG\w+|_NSIG))\s+(\w+)\s*$',
                                     (libc / 'include/bits/signal.h').read_text(), re.M))
        names = set(re.findall(r'\bSIG[A-Z0-9]+\b', generator)) | {'NSIG', '_NSIG'}
        header = '#include <signal.h>\n' + ''.join(f'#undef {name}\n' for name in sorted(names))
        header += ''.join(f'#define {name} {value}\n' for name, value in definitions.items() if name in names)
        # musl 1.2.5 reserves 32..34 for its internal thread signals.
        header += '#define NSIG _NSIG\n#define SIGRTMIN 35\n#define SIGRTMAX 64\n'
        generated = build / 'src/mksignames-target.c'
        generated.write_text(generator.replace('#include <signal.h>', header, 1))
        run(['cc', '-Wno-deprecated-non-prototype', generated, '-o', build / 'src/mksignames'], stdout=log, stderr=subprocess.STDOUT)
        # The wrapper is a build input that make's generated rules do not track.
        (build / 'src/dash').unlink(missing_ok=True)
        run(['gmake', '-j8'], cwd=build, stdout=log, stderr=subprocess.STDOUT)
    shutil.copyfile(build / 'src/dash', out / 'dash.elf')
    return out / 'dash.elf'


def build(out, llvm, art_compiler=False, art_oat=None, art_desktop=False, art_boot=None):
    libc = musl(llvm)
    runtime = compiler_runtime(llvm, libc)
    with (WORK / 'musl-build/shared-build.log').open('w') as log:
        run(['gmake', '-j8', '-W', 'obj/ldso/dynlink.lo', 'LIBCC=' + str(runtime), 'install'],
            cwd=WORK / 'musl-build', stdout=log, stderr=subprocess.STDOUT)
    resource = Path(subprocess.check_output([llvm / 'clang', '-print-resource-dir'], text=True).strip()) / 'include'
    run([llvm / 'clang', '--target=aarch64-linux-gnu', '-c', EXP / 'exec-guest.S', '-o', out / 'exec-guest.o'])
    run([shutil.which('ld.lld'), '-pie', '--no-dynamic-linker', '--entry=_start', '-Ttext=0x10000',
         '-z', 'max-page-size=16384', out / 'exec-guest.o', '-o', out / 'exec-guest.elf'])
    programs = [('/nce-exec', out / 'exec-guest.elf')]
    run([llvm / 'clang', '--target=aarch64-linux-gnu', '-c', EXP / 'userspace/sigreturn.S', '-o', out / 'sigreturn.o'])
    run([shutil.which('ld.lld'), '-pie', '--no-dynamic-linker', '--entry=_start',
         '-T', EXP / 'userspace/sigreturn.ld', '-z', 'max-page-size=16384',
         out / 'sigreturn.o', '-o', out / 'sigreturn.elf'])
    programs.append(('/lib/nce-sigreturn.so', out / 'sigreturn.elf'))
    # Force a real ELF text segment beyond the former 64 KiB limit. The table
    # also exercises static-PIE relocations and indirect calls across that text.
    large = out / 'large-text.c'
    large.write_text('\n'.join(
        f'__attribute__((noinline)) static unsigned f{i}(unsigned x) {{ return (x + {i*7919+17}u) ^ (x >> 3); }}'
        for i in range(4096)) + '\nstatic unsigned (*volatile functions[])(unsigned) = {' +
        ','.join(f'f{i}' for i in range(4096)) + '};\n'
        'unsigned exercise_large_text(unsigned x) { for (unsigned i=0;i<4096;++i) x=functions[i](x); return x; }\n')
    run([llvm / 'clang', '--target=aarch64-linux-musl', '-ffixed-x18', '-mno-outline-atomics',
         '-fPIE', '-O2', '-c', large, '-o', out / 'large-text.o'])
    for unit in ('mm-tests', 'signal-tests', 'code-tests', 'file-tests'):
        run([llvm / 'clang', '--target=aarch64-linux-musl', '-ffixed-x18', '-mno-outline-atomics',
             '-fPIE', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-nostdinc', '-isystem', libc / 'include', '-isystem', resource,
             '-c', EXP / f'userspace/{unit}.c', '-o', out / f'{unit}.o'])
    run([llvm / 'clang', '--target=aarch64-linux-musl', '-c', EXP / 'userspace/signal-registers.S',
         '-o', out / 'signal-registers.o'])
    for name in ('init', 'worker', 'mm-bench', 'binder-test', 'storage-test', 'reclaim-test', 'host-poll-test'):
        run([llvm / 'clang', '--target=aarch64-linux-musl', '-ffixed-x18', '-mno-outline-atomics',
             '-fPIE', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-nostdinc', '-isystem', libc / 'include', '-isystem', resource,
             '-isystem', out / 'linux-uapi/include',
             '-c', EXP / f'userspace/{name}.c', '-o', out / f'{name}.o'])
        run([shutil.which('ld.lld'), '-pie', '--no-dynamic-linker', '-z', 'max-page-size=16384',
             '-z', 'common-page-size=16384', libc / 'lib/rcrt1.o', libc / 'lib/crti.o',
             out / f'{name}.o', *([out / 'large-text.o'] if name == 'worker' else []),
             *([out / 'mm-tests.o', out / 'file-tests.o', out / 'signal-tests.o', out / 'signal-registers.o', out / 'code-tests.o'] if name == 'mm-bench' else []),
             libc / 'lib/libc.a', runtime, libc / 'lib/crtn.o', '-o', out / f'{name}.elf'])
        programs.append(('/init-user' if name == 'init' else '/' + name, out / f'{name}.elf'))
    programs.append(('/bin/sh', dash(out, llvm, libc, runtime, resource)))
    ndk = Path('/opt/homebrew/share/android-ndk')
    if 'Pkg.Revision = 29.0.14206865' not in (ndk / 'source.properties').read_text():
        raise RuntimeError('Bionic fixture requires Android NDK 29.0.14206865')
    run([ndk / 'toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android35-clang',
         '-ffixed-x18', '-mno-outline-atomics', '-g', '-O2', '-Wall', '-Wextra', '-Werror',
         '-Wl,-z,max-page-size=16384,-z,common-page-size=16384',
         EXP / 'userspace/bionic-test.c', EXP / 'userspace/proc-exit-tests.c', EXP / 'userspace/file-tests.c', EXP / 'userspace/seccomp-tests.c',
         EXP / 'userspace/signal-tests.c', EXP / 'userspace/signal-registers.S', '-o', out / 'bionic-test.elf'])
    programs.append(('/bionic-test', out / 'bionic-test.elf'))
    cfi_flags = [ndk / 'toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android35-clang',
                 '-ffixed-x18', '-mno-outline-atomics', '-O2', '-g', '-flto',
                 '-fsanitize=cfi-icall', '-fsanitize-cfi-cross-dso', '-fsanitize-trap=cfi-icall',
                 '-fvisibility=hidden', '-Wl,-z,max-page-size=16384,-z,common-page-size=16384']
    for name, increment in (('a', 11), ('b', 29)):
        library = out / f'libnce-cfi-{name}.so'
        run([*cfi_flags, '-shared', '-fPIC', '-DNCE_CFI_INCREMENT=' + str(increment),
             EXP / 'userspace/cfi-library.c', '-o', library])
        programs.append(('/system/lib64/' + library.name, library))
    run([*cfi_flags, EXP / 'userspace/cfi-test.c', '-ldl', '-o', out / 'cfi-test.elf'])
    programs.append(('/system/bin/cfi-test', out / 'cfi-test.elf'))
    run([ndk / 'toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android35-clang',
         '-ffixed-x18', '-mno-outline-atomics', '-g', '-O2', '-Wall', '-Wextra', '-Werror',
         '-Wl,-z,max-page-size=16384,-z,common-page-size=16384',
         EXP / 'art/reference_heap.c', EXP / 'art/reference_heap_test.c', '-o', out / 'art-heap-test.elf'])
    programs.append(('/art-heap-test', out / 'art-heap-test.elf'))
    prepare_android_runtime()
    android = WORK / 'android/android36/com.android.runtime'
    programs.append((ANDROID_INTERPRETER, build_android_linker()))
    programs.append(('/system/lib64/libc.so', build_android_libc()))
    programs.append(('/system/lib64/libdl.so', WORK / 'android/linker-build/libdl.so'))
    crypto = {**build_android_crypto(), **build_android_crypto(cronet=True)}
    art = build_android_art(include_compiler=art_compiler)
    if art_compiler:
        programs.append(('/system/bin/nce-dex2oat', art.pop('nce-dex2oat')))
    replacements = {**art, **crypto}
    prepare_android_desktop()
    assets = android_data(out) + [native_library_zip(out)]
    art_aot.generate_header(out / 'art-aot-targets.h')
    if art_compiler or art_boot:
        boot_plan = art_aot.boot_plan(assets)
        (out / 'art-boot-plan.json').write_text(json.dumps(boot_plan, indent=2) + '\n')
    if art_boot:
        current = json.loads((WORK / 'android/art-build/manifest.json').read_text())['inputs']
        art_aot.validate(art_boot, current, boot_plan)
        programs.append(('/data/nce/boot/arm64/boot.oat', art_boot / 'boot.oat'))
        assets += [('/data/nce/boot/arm64/' + name, art_boot / name)
                   for name in ('boot.art', 'boot.vdex')]
    if art_compiler or art_oat:
        aot_plan = art_aot.plan(assets, desktop=art_desktop)
        (out / 'art-aot-plan.json').write_text(json.dumps(aot_plan, indent=2) + '\n')
    if art_oat:
        current = json.loads((WORK / 'android/art-build/manifest.json').read_text())['inputs']
        art_aot.validate(art_oat, current, aot_plan)
        for target in aot_plan['targets']:
            prefix = str(Path(target['dex']).parent / 'oat/arm64' / target['name'])
            programs.append((prefix + '.odex', art_oat / (target['name'] + '.odex')))
            assets = [(guest, path) for guest, path in assets if guest != prefix + '.vdex']
            assets.append((prefix + '.vdex', art_oat / (target['name'] + '.vdex')))
    embedded, program_locations = apk_native_libraries(out, assets)
    programs += android_dependencies(llvm, replacements, embedded)
    programs += embedded
    android_service_headers()
    programs += android_graphics_dependencies(llvm, programs)
    module = 'com.android.configinfrastructure'
    module_root = WORK / 'android/android36' / module
    programs.append((f'/apex/{module}/bin/aconfigd-mainline', module_root / 'bin/aconfigd-mainline'))
    module_files = {path.name: path for path in (module_root / 'lib64').glob('*.so')}
    pending, visited = [module_root / 'bin/aconfigd-mainline'], set(replacements) | {'libc.so', 'libdl.so', 'ld-android.so'}
    while pending:
        binary = pending.pop()
        needed = re.findall(r'Shared library: \[(.*?)\]',
                            subprocess.check_output([llvm / 'llvm-readelf', '-d', binary], text=True))
        for name in needed:
            if name in visited:
                continue
            visited.add(name)
            if name not in module_files:
                if any(guest == '/system/lib64/' + name for guest, _ in programs):
                    continue
                raise RuntimeError('Missing aconfig module dependency: ' + name)
            programs.append((f'/apex/{module}/lib64/{name}', module_files[name]))
            pending.append(module_files[name])

    run([ndk / 'toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android35-clang',
         '-ffixed-x18', '-mno-outline-atomics', '-g', '-O2', '-Wall', '-Wextra', '-Werror',
         '-Wl,-z,max-page-size=16384,-z,common-page-size=16384',
         EXP / 'userspace/art-test.c', '-ldl', '-o', out / 'art-test.elf'])
    programs.append(('/system/bin/art-test', out / 'art-test.elf'))
    if art_compiler:
        run([ndk / 'toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android35-clang',
             '-ffixed-x18', '-mno-outline-atomics', '-O2', '-Wall', '-Wextra', '-Werror',
             '-Wl,-z,max-page-size=16384,-z,common-page-size=16384',
             '-I', out, EXP / 'userspace/art-compiler-test.c', '-ldl', '-o', out / 'art-compiler-test.elf'])
        programs.append(('/system/bin/art-compiler-test', out / 'art-compiler-test.elf'))
    protocol = EXP / 'graphics/protocol'
    for name, origin in json.loads((protocol / 'sources.json').read_text())['files'].items():
        if hashlib.sha256((protocol / name).read_bytes()).hexdigest() != origin['sha256']:
            raise RuntimeError('Graphics protocol checksum mismatch: ' + name)
    run([ndk / 'toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android35-clang',
         '-ffixed-x18', '-mno-outline-atomics', '-g', '-O2', '-Wall', '-Wextra', '-Werror',
         '-I', protocol, '-Wl,-z,max-page-size=16384,-z,common-page-size=16384',
         EXP / 'userspace/gpu-test.c', '-o', out / 'gpu-test.elf'])
    programs.append(('/system/bin/gpu-test', out / 'gpu-test.elf'))
    run([ndk / 'toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android35-clang',
         '-shared', '-fPIC', '-ffixed-x18', '-O2', '-Wall', '-Wextra', '-Werror',
         '-Wl,-z,max-page-size=16384,-z,common-page-size=16384',
         EXP / 'graphics/guest-pipe.c', '-o', out / 'libnce-qemupipe.so'])
    programs.append(('/system/lib64/libnce-qemupipe.so', out / 'libnce-qemupipe.so'))
    run([ndk / 'toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android35-clang',
         '-ffixed-x18', '-mno-outline-atomics', '-g', '-O2', '-Wall', '-Wextra', '-Werror',
         '-Wl,-z,max-page-size=16384,-z,common-page-size=16384',
         EXP / 'userspace/egl-test.c', '-lEGL', '-lGLESv2', '-o', out / 'egl-test.elf'])
    programs.append(('/system/bin/egl-test', out / 'egl-test.elf'))
    run([ndk / 'toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android35-clang',
         '-ffixed-x18', '-mno-outline-atomics', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
         '-Wl,-z,max-page-size=16384,-z,common-page-size=16384',
         EXP / 'userspace/gralloc-test.c', '-landroid', '-lEGL', '-lGLESv2', '-o', out / 'gralloc-test.elf'])
    programs.append(('/system/bin/gralloc-test', out / 'gralloc-test.elf'))
    programs.append(('/system/bin/secilc', WORK / 'android/android36/bin/secilc'))
    programs.append(('/system/bin/servicemanager', WORK / 'android/android36/bin/servicemanager'))
    programs.append(('/system/bin/aconfigd-system', WORK / 'android/android36/bin/aconfigd-system'))
    programs.append(('/system/bin/installd', WORK / 'android/android36/bin/installd'))
    for name in ('idmap2', 'idmap2d', 'vold', 'vold_prepare_subdirs', 'storaged', 'cmd', 'dumpsys', 'netd', 'lmkd',
                 'bpfloader', 'iptables', 'audioserver', 'mediaserver', 'mediametrics', 'gatekeeperd', 'keystore2'):
        programs.append(('/system/bin/' + name, WORK / 'android/android36/bin' / name))
    programs.append(('/apex/com.android.art/bin/artd', WORK / 'android/android36/com.android.art/bin/artd'))
    programs.append(('/apex/com.android.os.statsd/bin/statsd', WORK / 'android/android36/com.android.os.statsd/bin/statsd'))
    programs.append(('/apex/com.android.tethering/bin/netbpfload', build_android_network()))
    programs.append(('/apex/com.android.tethering/bin/for-system/clatd',
                     WORK / 'android/android36/com.android.tethering/bin/for-system/clatd'))
    programs.append(('/system/bin/hw/android.system.suspend-service', WORK / 'android/android36/bin/hw/android.system.suspend-service'))
    programs.append(('/system/bin/app_process64', WORK / 'android/android36/bin/app_process64'))
    programs.append(('/system/bin/derive_classpath', WORK / 'android/android36/com.android.sdkext/bin/derive_classpath'))
    run([ndk / 'toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android35-clang',
         '-shared', '-fPIC', '-ffixed-x18', '-O2', '-Wall', '-Wextra', '-Werror',
         '-Wl,-z,max-page-size=16384,-z,common-page-size=16384',
         EXP / 'services/framework-log.c', '-llog', '-ldl', '-o', out / 'libnce-framework-log.so'])
    programs.append(('/system/lib64/libnce-framework-log.so', out / 'libnce-framework-log.so'))
    apex_generated = android_service_interfaces(out)
    run([ndk / 'toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android35-clang++',
         '-ffixed-x18', '-mno-outline-atomics', '-g', '-O2', '-Wall', '-Wextra', '-Werror',
         '-std=c++17', '-nostdlib++', '-include', WORK / 'android/art-build/platform-libcxx-config.h',
         '-I', apex_generated, '-I', out, '-I', android_service_headers(),
         '-Wl,-z,max-page-size=16384,-z,common-page-size=16384',
         EXP / 'userspace/android-services-test.cpp', EXP / 'userspace/property-service.cpp',
         EXP / 'userspace/display-service-test.cpp',
         EXP / 'userspace/input-service.cpp',
         EXP / 'userspace/framework-test.cpp', EXP / 'userspace/cgroup-test.cpp', EXP / 'userspace/desktop-session.cpp', EXP / 'userspace/crypto-tests.cpp',
         EXP / 'userspace/apex-service.cpp', EXP / 'userspace/suspend-test.cpp', EXP / 'userspace/installd-test.cpp', EXP / 'userspace/storage-layout.cpp',
         *sorted(apex_generated.rglob('*.cpp')),
         WORK / 'android/android36/lib64/libc++.so',
         '-I', WORK / 'android/boringssl/src/include', crypto['libcrypto.so'], '-ldl',
         WORK / 'android/android36/lib64/libbinder_ndk.so',
         '-o', out / 'android-services-test.elf'])
    programs.append(('/system/bin/android-services-test', out / 'android-services-test.elf'))
    shutil.copyfile(libc / 'lib/libc.so', out / 'libc.so')
    programs.append((INTERPRETER, out / 'libc.so'))
    for name in ('dependency', 'plugin', 'dynamic'):
        run([llvm / 'clang', '--target=aarch64-linux-musl', '-ffixed-x18', '-mno-outline-atomics',
             '-fPIC', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-nostdinc', '-isystem', libc / 'include', '-isystem', resource,
             '-c', EXP / f'userspace/{name}.c', '-o', out / f'{name}.o'])
        options = ['-pie', '-dynamic-linker', INTERPRETER, libc / 'lib/Scrt1.o', libc / 'lib/crti.o'] if name == 'dynamic' else ['-shared', '-soname', name + '.so']
        file = out / (name + ('.elf' if name == 'dynamic' else '.so'))
        run([shutil.which('ld.lld'), *options, '-z', 'max-page-size=16384', '-z', 'common-page-size=16384',
             out / f'{name}.o', *([out / 'dependency.so', '-rpath', '/lib'] if name == 'dynamic' else []),
             '-L' + str(libc / 'lib'), '-lc', runtime, libc / 'lib/crtn.o', '-o', file])
        programs.append(('/dynamic' if name == 'dynamic' else '/lib/' + name + '.so', file))
    header, entries = ['extern const unsigned char nce_payload[];'], []
    payload = bytearray()
    system_files = []
    cache_path = out / 'elf-inspection-cache.json'
    inspector_source = '\n'.join(python_inspect.getsource(function) for function in
        (inspect, x18_instruction, direct_branch, extend_function_ranges, elf_disassembly, check_x18_disassembly, art_aot.code_ranges))
    inspector_source += repr((PAGE, PROGRAM_LIMIT, MAX_ALIGNMENT, INTERPRETER,
                              ANDROID_INTERPRETER, ANDROID_BOOTSTRAP_INTERPRETER))
    inspector = hashlib.sha256(inspector_source.encode() + subprocess.check_output(
        [llvm / 'llvm-objdump', '--version'])).hexdigest()
    cache = json.loads(cache_path.read_text()) if cache_path.exists() else {}
    if cache.get('inspector') != inspector:
        cache = {'inspector': inspector, 'files': {}}
    offset = 0
    for i, (name, path) in enumerate(programs):
        data = path.read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        cached = cache['files'].get(name)
        if cached and cached['sha256'] == digest:
            elf = dict(cached['layout'], data=data)
        else:
            elf = inspect(path, shared=name.endswith(('.so', '.odex', '.oat')), function_symbols=name.endswith(('.odex', '.oat')) or name.startswith(('/system/', '/vendor/', '/apex/', '/system_ext/', '/product/')) or name in ('/bionic-test', '/art-test', '/art-heap-test'), nce_oat=name.endswith(('.odex', '.oat')))
            dis = elf_disassembly(llvm, path, elf)
            check_x18_disassembly(path, elf, dis)
            cache['files'][name] = {'sha256': digest, 'layout': {k: v for k, v in elf.items() if k != 'data'}}
        # Bionic's CFI shadow permits only one DSO in each 256 KiB address block.
        # This also applies to libraries without CFI, whose unload clears shadow entries.
        alignment = max(elf['alignment'], 1 << 18)
        offset = (offset + alignment - 1) & -alignment
        stub_size = (32 + 80 * len(elf['patches']) + PAGE - 1) & -PAGE
        if elf['span'] + stub_size >= 128 * 1024 * 1024:
            raise RuntimeError('ELF and its branch stubs exceed the AArch64 branch range')
        # Exercise linking and syscalls beyond the original global branch range.
        if name == '/dynamic':
            offset = max(offset, 256 * 1024 * 1024)
        if offset + elf['span'] + stub_size > PROGRAM_LIMIT:
            raise RuntimeError('Program layouts overlap the mmap area')
        if name == '/worker' and sum(size for _, size in elf['segments']) <= 65536:
            raise RuntimeError('Large-text fixture did not exceed 64 KiB')
        position = len(payload)
        system_image = name.startswith(('/system/', '/vendor/', '/apex/', '/system_ext/', '/product/'))
        if system_image and name not in program_locations:
            system_files.append((name, path))
        payload.extend(elf['data'][:elf['phoff'] + elf['phsize']] if system_image else elf['data'])
        header.append(f'static NceSegment segments_{i}[] = {{' + ','.join(
            f'{{.offset={lo},.size={size}}}' for lo, size in elf['segments']) + '};')
        header.append(f'static const NcePatch patches_{i}[] = {{' + ','.join(
            f'{{{va},{word}}}' for va, word in elf['patches']) + '};')
        container, source_offset = program_locations.get(name, (name, 0))
        entries.append(f'{{.path="{name}",.container="{container}",.source_offset={source_offset},.file=nce_payload+{position},.size={len(elf["data"])},.system_image={int(system_image)},.sha256={{' + ','.join(str(x) for x in hashlib.sha256(elf['data']).digest()) + f'}},.offset={offset},.span={elf["span"]},'
                       f'.stub_size={stub_size},.entry={elf["entry"]},.phoff={elf["phoff"]},.phsize={elf["phsize"]},'
                       f'.segments=segments_{i},.segment_count={len(elf["segments"])},'
                       f'.patches=patches_{i},.patch_count={len(elf["patches"])}}}')
        offset += elf['span'] + stub_size
        print(f'{name}: {len(elf["data"])} bytes, {len(elf["segments"])} executable segments, {len(elf["patches"])} patched instructions', flush=True)
    cache_path.write_text(json.dumps(cache) + '\n')
    header += ['NceProgram nce_programs[] = {' + ',\n'.join(entries) + '};',
               f'const size_t nce_program_count = {len(entries)};']
    (out / 'program-images.h').write_text('\n'.join(header) + '\n')
    asset_header = ['extern const unsigned char nce_payload[];', 'static const char shell_script[] = ' +
              json.dumps((EXP / 'userspace/shell-tests.sh').read_text()) + ';\n']
    entries = []
    for name, path in assets:
        if name.startswith(('/system/', '/apex/', '/system_ext/', '/product/', '/vendor/')):
            system_files.append((name, path))
            continue
        data = path.read_bytes()
        entries.append(f'{{"{name}",nce_payload+{len(payload)},{len(data)}}}')
        payload.extend(data)
    asset_header.append('static const struct { const char *path; const void *data; size_t size; } art_assets[] = {' +
                  ',\n'.join(entries) + '};')
    (out / 'user-scripts.h').write_text('\n'.join(asset_header) + '\n')
    (out / 'payload.bin').write_bytes(payload)
    system_root = out / 'system-root'
    if system_root.exists():
        shutil.rmtree(system_root)
    system_root.mkdir()
    for name, path in system_files:
        target = system_root / name.lstrip('/')
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, target)
        target.chmod(0o755 if '/bin/' in name else 0o644)
    (system_root / 'system/bin/bootstrap').mkdir(exist_ok=True)
    (system_root / 'system/bin/bootstrap/linker64').symlink_to('/system/bin/linker64')
    for name in ('ip6tables', 'iptables-restore', 'ip6tables-restore'):
        (system_root / 'system/bin' / name).symlink_to('iptables')
    (system_root / 'apex/com.android.art/lib64').mkdir(parents=True, exist_ok=True)
    log_mount = system_root / 'vendor/zygote_whitelist/nce-stderr'
    log_mount.parent.mkdir(parents=True, exist_ok=True)
    log_mount.touch()
    image = out / 'system.img'
    with image.open('wb') as disk:
        size = sum(path.stat().st_size for _, path in system_files) + 64 * 1024 * 1024
        disk.truncate((size + 64 * 1024 * 1024 - 1) & -(64 * 1024 * 1024))
    run(['/opt/homebrew/opt/e2fsprogs/sbin/mke2fs', '-F', '-q', '-t', 'ext4', '-b', '4096',
         '-O', '^has_journal', '-E', 'lazy_itable_init=0', '-d', system_root, image])
    # ClatCoordinator validates the APEX executable's ownership, mode and label at JNI load.
    metadata = out / 'system-metadata.debugfs'
    commands = []
    for path, mode, uid, gid, label in (
        ('/apex/com.android.tethering/bin/for-system', '040750', 1029, 1000, 'system_file'),
        ('/apex/com.android.tethering/bin/for-system/clatd', '0106755', 1029, 1029, 'clatd_exec')):
        commands += [f'set_inode_field {path} mode {mode}', f'set_inode_field {path} uid {uid}',
                     f'set_inode_field {path} gid {gid}',
                     f'ea_set {path} security.selinux u:object_r:{label}:s0']
    metadata.write_text('\n'.join(commands) + '\n')
    run(['/opt/homebrew/opt/e2fsprogs/sbin/debugfs', '-w', '-f', metadata, image],
        stdout=subprocess.DEVNULL)
    shutil.rmtree(system_root)
    payload_path = str((out / 'payload.bin').resolve()).replace('\\', '\\\\').replace('"', '\\"')
    (out / 'payload.S').write_text('.section __TEXT,__const\n.p2align 4\n.globl _nce_payload\n'
                                 '_nce_payload:\n.incbin "' + payload_path + '"\n')
