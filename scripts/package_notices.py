"""收集两端应用实际使用的开源声明，并保留来源路径。"""
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[1]


def write_notices(resources, *, ios=False):
    android = ROOT / 'work/native-linux-nce/android'
    paths = [ROOT / 'LICENSE', ROOT / 'work/native-linux-nce/lkl-source/COPYING',
             ROOT / 'work/native-linux-nce/lkl-source/LICENSES/preferred/GPL-2.0',
             ROOT / 'work/native-linux-nce/musl-1.2.5/COPYRIGHT',
             ROOT / 'work/native-linux-nce/dash-0.5.12/COPYING',
             ROOT / 'work/native-linux-nce/compiler-rt-20.1.8.src/LICENSE.TXT']
    paths += [android / name for name in [
        'android36/NOTICE.txt', 'android36-aosp-components/NOTICE.txt', 'art/NOTICE',
        'bionic-source/libc/NOTICE', 'bionic-source/linker/NOTICE', 'bionic-source/libdl/NOTICE',
        'scudo/LICENSE.TXT', 'libunwind-20.1.8.src/LICENSE.TXT', 'fmt/LICENSE', 'dlmalloc/NOTICE',
        'libnativehelper/NOTICE', 'cpu_features/LICENSE', 'tinyxml2/LICENSE.txt', 'lz4/lib/LICENSE',
        'lzma/NOTICE', 'libbase/NOTICE', 'liblog/liblog/NOTICE', 'cronet-boringssl/src/LICENSE',
        'vixl/LICENCE', 'boringssl/src/LICENSE']]
    gfx = ROOT / 'work/gfxstream-src'
    angle = ROOT / 'third_party/gpu/webkit/Source/ThirdParty/ANGLE'
    paths += [gfx / 'LICENSE', *sorted(p for p in (gfx / 'third_party').rglob('LICENSE*') if p.is_file())]
    paths += sorted(p for p in angle.rglob('*') if p.is_file() and
                    p.name.upper().startswith(('LICENSE', 'COPYING')) and '.xcarchive/' not in str(p))
    if ios:
        paths += sorted((ROOT / 'src/app/Resources/legal').glob('LICENSE-*.txt'))
    notices = resources / 'ThirdPartyNotices.txt'
    with notices.open('wb') as output:
        for path in paths:
            output.write(f'\n===== {path.relative_to(ROOT)} =====\n'.encode())
            # Android SDK NOTICE files mix UTF-8 with isolated Latin-1 copyright signs.
            content = path.read_bytes().decode('utf-8', errors='surrogateescape').replace('\udca9', '©')
            output.write(content.encode('utf-8'))
    acknowledgement = resources / 'Acknowledgements.md'
    shutil.copyfile(ROOT / 'ACKNOWLEDGEMENTS.md', acknowledgement)
    android_notice = resources / 'NOTICE.xml.gz'
    shutil.copyfile(android / 'android36/etc/NOTICE.xml.gz', android_notice)
    return [acknowledgement, notices, android_notice]
