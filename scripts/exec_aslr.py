"""构建 ELF 地址验收文件，并按其真实 program headers 检查运行报告。"""
import json
from pathlib import Path
import shutil
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
EXP = ROOT / 'experiments/native-linux'
MODES = {'off': (0, 0), 'mmap_only': (1, 0), 'full': (2, 0), 'personality': (2, 0x40000)}


def elf_spec(path):
    data = path.read_bytes()
    if data[:7] != b'\x7fELF\x02\x01\x01':
        raise ValueError('需要 AArch64 ELF：' + str(path))
    kind, machine = struct.unpack_from('<HH', data, 16)
    entry, phoff = struct.unpack_from('<QQ', data, 24)
    phsize, phnum = struct.unpack_from('<HH', data, 54)
    if machine != 183 or phsize != 56 or not 0 < phnum <= 32:
        raise ValueError('ELF program headers 不匹配：' + str(path))
    loads, phdr_vaddr, interpreter = [], None, None
    for i in range(phnum):
        ptype, flags, offset, vaddr, _, filesz, memsz, align = struct.unpack_from('<IIQQQQQQ', data, phoff + i * phsize)
        if ptype == 3:
            interpreter = data[offset:offset + filesz].rstrip(b'\0').decode()
        elif ptype == 1:
            loads.append({'vaddr': vaddr, 'offset': offset, 'filesz': filesz, 'memsz': memsz, 'align': align, 'flags': flags})
            if offset <= phoff < offset + filesz:
                phdr_vaddr = vaddr + phoff - offset
    if not loads or phdr_vaddr is None or len(loads) > 16:
        raise ValueError('缺少可核对的 PT_LOAD/AT_PHDR：' + str(path))
    return {'type': kind, 'entry': entry, 'phdr_vaddr': phdr_vaddr, 'phnum': phnum,
            'phentsize': phsize, 'interpreter': interpreter, 'loads': loads}


def build(out, libc, guest_page_bytes):
    llvm = Path('/opt/homebrew/opt/llvm/bin')
    resource = subprocess.check_output([llvm / 'clang', '-print-resource-dir'], text=True).strip()
    obj = out / 'exec-aslr-fixture.o'
    commands = [[llvm / 'clang', '--target=aarch64-linux-musl', '-mno-outline-atomics', '-fPIE', '-O2', '-g',
                 '-Wall', '-Wextra', '-Werror', '-DNCE_EXEC_ASLR_IMAGE',
                 f'-DNCE_GUEST_PAGE_SHIFT={guest_page_bytes.bit_length() - 1}', '-nostdinc',
                 '-isystem', libc / 'include', '-isystem', Path(resource) / 'include',
                 '-c', EXP / 'translation/exec-aslr-image.c', '-o', obj]]
    files = [Path(__file__), EXP / 'translation/exec-aslr-image.c', EXP / 'translation/exec-aslr.h']
    specs = {}
    variants = {'static': ('rcrt1.o', guest_page_bytes, []),
                'static-2m': ('rcrt1.o', 1 << 21, []),
                'pie': ('Scrt1.o', guest_page_bytes, []),
                'pie-256k': ('Scrt1.o', 1 << 18, []),
                'pie-2m': ('Scrt1.o', 1 << 21, []),
                'pie-nonzero': ('Scrt1.o', guest_page_bytes, ['--image-base=0x200000']),
                'exec': ('crt1.o', guest_page_bytes, ['--image-base=0x1000000']),
                'exec-near-end': ('Scrt1.o', guest_page_bytes, ['--image-base=0x7fff000000'])}
    subprocess.run(list(map(str, commands[0])), check=True, timeout=120)
    for name, (crt, alignment, extra) in variants.items():
        path = out / ('exec-aslr-' + name + '.elf')
        dynamic = crt == 'Scrt1.o'
        inputs = [libc / ('lib/' + crt), libc / 'lib/crti.o', obj,
                  libc / ('lib/libc.so' if dynamic else 'lib/libc.a'),
                  ROOT / 'work/native-linux-nce/compiler-rt-build/libbuiltins.a', libc / 'lib/crtn.o']
        link = [*inputs[:3], *(['-L', libc / 'lib', '-lc'] if dynamic else [inputs[3]]), *inputs[4:]]
        command = [shutil.which('ld.lld'), *([] if name in ('exec', 'exec-near-end') else ['-pie']),
                   *(['--dynamic-linker=/lib/ld-musl-aarch64.so.1'] if dynamic else ['--no-dynamic-linker']),
                   '-z', f'max-page-size={alignment}', '-z', f'common-page-size={guest_page_bytes}',
                   *extra, *link, '-o', path]
        subprocess.run(list(map(str, command)), check=True, timeout=120)
        commands.append(command)
        files += [path, *[p for p in inputs if p != obj]]
        specs[name] = elf_spec(path)
    specs['loader'] = specs['pie']
    interpreter = elf_spec(ROOT / 'work/native-linux-nce/processes/libc.so')
    metadata = {'images': specs, 'interpreter': interpreter,
                'scope': 'PTE core ELF/interpreter/brk/sigreturn; ET_EXEC is linked at 16 MiB for low/sparse guests'}
    manifest = out / 'exec-aslr-fixtures.json'
    manifest.write_text(json.dumps(metadata, indent=2) + '\n')
    return metadata, [list(map(str, command)) for command in commands], sorted(set([*files, manifest]))


def image_valid(actual, expected, guest, page):
    if type(actual) is not dict or set(actual) != {'bias', 'first_vaddr', 'type', 'loads'}:
        return False
    bias = actual['bias']
    if type(bias) is not int or not 0 <= bias < 1 << 64 or actual['type'] != expected['type']:
        return False
    if actual['first_vaddr'] != min(load['vaddr'] & -page for load in expected['loads']):
        return False
    loads = [{**{key: value for key, value in load.items() if key != 'vaddr'}, 'address': bias + load['vaddr']}
             for load in expected['loads']]
    if actual['loads'] != loads:
        return False
    return all(guest['guest_base'] <= load['address'] < guest['guest_end'] and
               load['memsz'] <= guest['guest_end'] - load['address'] and
               load['filesz'] <= load['memsz'] and load['address'] % page == load['offset'] % page and
               (load['align'] <= 1 or not bias % load['align']) for load in loads)


def validate_exec_layout(rows, images, guest, page):
    if not images or '/system/bin/init' not in images:
        return False
    program = images['/system/bin/init']
    interpreter = images.get(program['interpreter'])
    if not interpreter or program['type'] != 3 or interpreter['type'] != 3:
        return False
    events = [row for row in rows if 'stock_init_exec_layout' in row or
              (row.get('stock_init_security') == 221 and
               row.get('path_hex') == '/system/bin/init'.encode().hex() and row.get('result') == 0)]
    if not events or len(events) % 2 or any('stock_init_exec_layout' in events[i] or
                                          'stock_init_exec_layout' not in events[i + 1]
                                          for i in range(0, len(events), 2)):
        return False
    samples = events[1::2]
    if not any(row.get('pid') == 1 for row in samples):
        return False
    fields = ('pid', 'sp', 'pc', 'phdr', 'entry', 'at_base', 'page_bytes', 'phnum', 'phentsize',
              'phdr_mapping', 'entry_mapping', 'pc_mapping')
    for row in samples:
        if (set(row) != {'stock_init_exec_layout', 'read_result', *fields} or
                row['stock_init_exec_layout'] is not True or type(row['read_result']) is not int or row['read_result'] != 0 or
                any(type(row[key]) is not int or not 0 <= row[key] < 1 << 64 for key in fields) or row['pid'] <= 0 or
                row['page_bytes'] != page or row['phnum'] != program['phnum'] or row['phentsize'] != program['phentsize'] or
                not guest['guest_base'] <= row['sp'] < guest['guest_end'] or row['sp'] % 16 or
                row['entry'] < program['entry'] or row['pc'] != row['at_base'] + interpreter['entry']):
            return False
        bias = row['entry'] - program['entry']
        if row['phdr'] != bias + program['phdr_vaddr']:
            return False
        for spec, base in ((program, bias), (interpreter, row['at_base'])):
            if any(not guest['guest_base'] <= base + load['vaddr'] < guest['guest_end'] or
                   load['memsz'] > guest['guest_end'] - base - load['vaddr'] or
                   (load['align'] > 1 and base % load['align']) for load in spec['loads']):
                return False
        for address, mapping, spec, base in ((row['phdr'], row['phdr_mapping'], program, bias),
                                             (row['entry'], row['entry_mapping'], program, bias),
                                             (row['pc'], row['pc_mapping'], interpreter, row['at_base'])):
            load = next((load for load in spec['loads'] if base + load['vaddr'] <= address < base + load['vaddr'] + load['memsz']), None)
            prot = mapping & (page - 1)
            if (load is None or not mapping or mapping & -page >= 1 << 30 or prot & ~7 or not prot & 1 or
                    bool(prot & 4) != bool(load['flags'] & 1) or (prot & 2 and not load['flags'] & 2)):
                return False
    return True


def validate(rows, fixtures, guest, page):
    if not fixtures:
        return False
    elf_base = guest['guest_base'] + 0x2c000000
    if [row for row in rows if row.get('config') == 'linux_exec_aslr'] != [
            {'config': 'linux_exec_aslr', 'elf_base': elf_base, 'brk_range_bytes': 1 << 30}]:
        return False
    names = [name for name in fixtures['images'] if (name != 'exec' or guest['mode'] != 'host') and
             (name != 'exec-near-end' or guest['mode'] == 'sparse')]
    expected_results = [{'test': 'linux_exec_aslr', 'fixture': name, 'mode': mode, 'passed': True}
                        for mode in MODES for name in names]
    if [row for row in rows if row.get('test') == 'linux_exec_aslr'] != expected_results:
        return False
    samples = [row for row in rows if 'linux_exec_aslr_sample' in row]
    if len(samples) != len(expected_results) * 4:
        return False
    for mode, (randomize, personality) in MODES.items():
        for name in names:
            selected = [row for row in samples if row.get('mode') == mode and row.get('fixture') == name]
            spec = fixtures['images'][name]
            if len(selected) != 4:
                return False
            randomized = randomize and not personality
            for i, row in enumerate(selected):
                fields = ('phdr', 'entry', 'at_base', 'start_brk', 'brk', 'stack', 'sigreturn', 'personality')
                if (set(row) != {'linux_exec_aslr_sample', 'fixture', 'mode', 'iteration', 'program', 'interpreter', *fields} or
                        row['linux_exec_aslr_sample'] is not True or row['iteration'] != i or row['personality'] != personality or
                        any(type(row[key]) is not int or not 0 <= row[key] < 1 << 64 for key in fields) or
                        not image_valid(row['program'], spec, guest, page) or
                        not guest['guest_base'] <= row['start_brk'] <= row['brk'] <= guest['guest_end'] or
                        not guest['guest_base'] <= row['stack'] < guest['guest_end'] or
                        not guest['guest_base'] <= row['sigreturn'] < guest['guest_end']):
                    return False
                interp = row['interpreter']
                if spec['interpreter']:
                    if (not image_valid(interp, fixtures['interpreter'], guest, page) or
                            row['at_base'] != (0 if name == 'loader' else interp['bias'])):
                        return False
                elif interp != {'bias': 0, 'first_vaddr': 0, 'type': 0, 'loads': []} or row['at_base']:
                    return False
                kernel_image = interp if name == 'loader' else row['program']
                kernel_spec = fixtures['interpreter'] if name == 'loader' else spec
                if (row['phdr'] != kernel_image['bias'] + kernel_spec['phdr_vaddr'] or
                        row['entry'] != kernel_image['bias'] + kernel_spec['entry']):
                    return False
                brk_image = interp if name == 'loader' else row['program']
                baseline = max((load['address'] + load['memsz'] + page - 1) & -page for load in brk_image['loads'])
                moved = spec['type'] == 3 and (not spec['interpreter'] or name == 'loader')
                if randomized and randomize == 2:
                    baseline = elf_base if moved else baseline + page
                    if not baseline <= row['start_brk'] < min(baseline + (1 << 30), guest['guest_end']):
                        return False
                elif row['start_brk'] != baseline:
                    return False
                bias = row['program']['bias']
                alignment = max(load['align'] for load in spec['loads'])
                if spec['type'] == 2:
                    if bias:
                        return False
                elif spec['interpreter'] and name != 'loader':
                    if guest['mode'] == 'sparse':
                        rnd_max = ((1 << (24 if page == 16384 else 26)) - 1) * page
                    else:
                        rnd_max = ((1 << (17 if page == 16384 else 19)) - 1) * page
                    first = row['program']['first_vaddr']
                    low = (elf_base & -alignment) - first
                    high = ((elf_base + (rnd_max if randomized else 0)) & -alignment) - first
                    if not low <= bias <= high:
                        return False
            for field in ('program', 'interpreter', 'start_brk', 'brk', 'sigreturn'):
                values = {row[field]['bias'] if field in ('program', 'interpreter') else row[field] for row in selected}
                varied = randomized and (field != 'program' or spec['type'] == 3) and (
                    field != 'interpreter' or spec['interpreter']) and (field not in ('start_brk', 'brk') or spec['type'] == 3 or randomize == 2)
                if (varied and len(values) < 2) or (not varied and len(values) != 1):
                    return False
    cow = [row for row in rows if row.get('test') == 'linux_exec_aslr_fork_cow']
    if len(cow) != 1 or cow[0].get('passed') is not True:
        return False
    c = cow[0]
    if (set(c) != {'test', 'passed', 'parent_phdr', 'child_phdr', 'parent_sigreturn', 'child_sigreturn', 'heap', 'elf_private', 'heap_private'} or
            c['parent_phdr'] == c['child_phdr'] or c['parent_sigreturn'] == c['child_sigreturn'] or
            c['elf_private'] != 19 or c['heap_private'] != 23 or c['heap'] % page or
            any(type(c[key]) is not int or not guest['guest_base'] <= c[key] < guest['guest_end']
                for key in ('parent_phdr', 'child_phdr', 'parent_sigreturn', 'child_sigreturn', 'heap'))):
        return False
    if [row for row in rows if row.get('test') == 'linux_exec_aslr_syscall_range'] != [
            {'test': 'linux_exec_aslr_syscall_range', 'passed': True}]:
        return False
    rejects = [row for row in rows if row.get('test') == 'linux_exec_aslr_rejected_range']
    return len(rejects) == 6 and all(
        row.get('mode') == name and row.get('passed') is True and
        set(row) == {'test', 'mode', 'passed', 'status', 'errno'} and
        ((row['status'] == 11 and row['errno'] == 0) or (row['status'] == 0 and row['errno'] in (8, 13, 12, 22)))
        for row, name in zip(rejects, ('below_guest', 'above_guest', 'brk_above_guest', 'align_below_guest', 'static_align_below_guest', 'interp_align_below_guest')))
