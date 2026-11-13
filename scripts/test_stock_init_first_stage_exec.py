#!/usr/bin/env python3
"""用原 ramdisk ELF 和已归档报告检查 first-stage 执行关联契约。"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

from exec_aslr import elf_spec_bytes, validate as validate_core, validate_exec_layout
from stock_init_sdk_ramdisk import read as read_ramdisk

RAW_PATHS = ('/init', '/system/bin/init')


def changed(rows, index, **fields):
    result = list(rows)
    result[index] = {**rows[index], **fields}
    return result


def exec_rows(rows):
    paths = {path.encode().hex() for path in RAW_PATHS}
    return [row for row in rows if 'stock_init_exec_layout' in row or
            row.get('stock_init_security') == 221 and row.get('path_hex') in paths or
            'stock_init_observation' in row or row.get('test') == 'translated_context_cleanup' or
            row.get('result') in ('passed', 'failed')]


def variants(rows, page):
    layouts = [i for i, row in enumerate(rows) if 'stock_init_exec_layout' in row]
    security = [i - 1 for i in layouts]
    summary = len(rows) - 2
    first = layouts[0]
    cases = []
    for positions, name in ((layouts[:1], 'missing_layout'), (security[:1], 'missing_security'),
                             ((security[1], layouts[1]), 'missing_middle_pair'),
                             ((security[-1], layouts[-1]), 'missing_last_pair'),
                             ((summary,), 'missing_terminal_count')):
        cases.append((name, [row for i, row in enumerate(rows) if i not in positions]))
    for positions, name in ((layouts, 'reordered_layouts'), (security, 'reordered_security')):
        result = list(rows)
        result[positions[0]], result[positions[1]] = result[positions[1]], result[positions[0]]
        cases.append((name, result))
    result = list(rows)
    result[:4] = rows[2:4] + rows[:2]
    cases += [('reordered_complete_pairs', result), ('duplicate_layout', rows + [rows[first]]),
              ('layout_before_security', [rows[first]] + [row for i, row in enumerate(rows) if i != first]),
              ('terminal_count_after_result', rows[:summary] + rows[summary + 1:] + [rows[summary]])]
    for value, name in ((None, 'none'), (False, 'bool'), (1.0, 'float'), ('1', 'string'),
                        (0, 'zero'), (-1, 'negative'), (len(layouts) - 1, 'too_small'),
                        (len(layouts) + 1, 'too_large'), (1 << 64, 'overflow')):
        cases.append(('terminal_count_' + name, changed(rows, summary, stock_init_exec_count=value)))
    result = list(rows)
    result[summary] = {key: value for key, value in rows[summary].items() if key != 'stock_init_exec_count'}
    cases.append(('missing_terminal_count_field', result))
    for index, name in ((security[0], 'security'), (first, 'layout')):
        for value, label in ((None, 'none'), (False, 'bool'), (1.0, 'float'), ('1', 'string'),
                             (0, 'zero'), (-1, 'negative'), (2, 'skipped'), (1 << 64, 'overflow')):
            cases.append((name + '_index_' + label, changed(rows, index, exec_index=value)))
        result = list(rows)
        result[index] = {key: value for key, value in rows[index].items() if key != 'exec_index'}
        cases.append(('missing_' + name + '_index', result))
    for field, value in (('read_result', -14), ('phdr', rows[first]['phdr'] + page),
                         ('entry', rows[first]['entry'] + 4), ('pc', rows[first]['pc'] + 4),
                         ('at_base', rows[first]['at_base'] + page), ('phnum', rows[first]['phnum'] + 1),
                         ('phentsize', 64), ('phdr_mapping', 0), ('entry_mapping', 1),
                         ('pc_mapping', 3), ('entry_mapping', (1 << 30) | 5)):
        cases.append(('invalid_' + field, changed(rows, first, **{field: value})))
    cases.append(('failed_exec', changed(rows, security[0], result=-13)))
    return cases


def reindex(rows):
    result = []
    index = 0
    for row in rows:
        if row.get('stock_init_security') == 221:
            index += 1
        if 'exec_index' in row:
            row = {**row, 'exec_index': index}
        elif row.get('test') == 'translated_context_cleanup':
            row = {**row, 'stock_init_exec_count': index}
        result.append(row)
    return result


def raw_fixture(rows, spec, page):
    def mapping(address):
        load = next(load for load in spec['loads'] if load['vaddr'] <= address < load['vaddr'] + load['memsz'])
        flags = load['flags']
        return page | ((1 if flags & 4 else 0) | (2 if flags & 2 else 0) | (4 if flags & 1 else 0))

    first = {**rows[1], 'exec_index': 1, 'pid': 1, 'pc': spec['entry'], 'phdr': spec['phdr_vaddr'],
             'entry': spec['entry'], 'at_base': 0, 'phnum': spec['phnum'], 'phentsize': spec['phentsize'],
             'phdr_mapping': mapping(spec['phdr_vaddr']), 'entry_mapping': mapping(spec['entry']),
             'pc_mapping': mapping(spec['entry'])}
    security = {**rows[0], 'path_hex': '/init'.encode().hex()}
    # 仅合成新增的静态执行对与 PTE；动态地址和 stack 来自实际旧报告。
    return reindex([security, first, *rows])


def check_init(report, spec, page):
    images = report['input']['exec_elf_images']
    raw_images = {'/init': spec, **images}
    output = []
    for run in report['runs']:
        rows = run['results']
        guest = next(row for row in rows if row.get('config') == 'translation_guest_layout')
        assert run['observation_validated'] is True
        if run['mode'] != 'original':
            assert not any('stock_init_exec_layout' in row for row in rows)
            assert not validate_exec_layout(rows, images, guest, page)
            output.append({'entry': run['entry'], 'mode': run['mode'], 'no_init_exec_layout': True})
            continue
        assert validate_exec_layout(rows, images, guest, page)
        selected = exec_rows(rows)
        assert selected[-2]['stock_init_exec_count'] == 3
        assert validate_exec_layout(selected, images, guest, page)
        rejected = []
        for name, result in variants(selected, page):
            assert not validate_exec_layout(result, images, guest, page), name
            rejected.append(name)
        assert not validate_exec_layout(rows, None, guest, page)
        rejected.append('missing_elf_input')
        repeated = changed(selected, 3, **{**selected[1], 'exec_index': 2, 'pid': selected[3]['pid']})
        assert validate_exec_layout(repeated, images, guest, page)

        raw = raw_fixture(selected, spec, page)
        assert validate_exec_layout(raw, raw_images, guest, page, init_paths=RAW_PATHS)
        only_static = reindex(raw[:2] + raw[-3:])
        assert validate_exec_layout(only_static, raw_images, guest, page, init_paths=RAW_PATHS)
        assert not validate_exec_layout(raw, raw_images, guest, page)
        assert not validate_exec_layout(selected, raw_images, guest, page, init_paths=RAW_PATHS)
        assert not validate_exec_layout(raw, images, guest, page, init_paths=RAW_PATHS)
        assert not validate_exec_layout(raw, raw_images, guest, page, init_paths=('/init',))
        raw_rejected = ['raw_without_explicit_paths', 'dynamic_without_initial_raw_exec',
                        'missing_raw_elf_input', 'unsupported_init_paths']
        raw_cases = variants(raw, page)
        raw_cases += [('raw_first_pid_not_one', changed(raw, 1, pid=2)),
                      ('raw_first_pair_removed_and_renumbered', reindex(raw[2:])),
                      ('dynamic_before_static_with_valid_indices', reindex(raw[2:4] + raw[:2] + raw[4:])),
                      ('static_repeated_after_first', reindex(raw[:2] + raw[:2] + raw[4:])),
                      ('static_relocated_by_load_alignment', changed(raw, 1, entry=spec['entry'] + 16384,
                                                                    phdr=spec['phdr_vaddr'] + 16384,
                                                                    pc=spec['entry'] + 16384)),
                      ('static_phdr_executable', changed(raw, 1, phdr_mapping=raw[1]['phdr_mapping'] | 4)),
                      ('static_phdr_writable', changed(raw, 1, phdr_mapping=raw[1]['phdr_mapping'] | 2)),
                      ('static_pc_unreadable', changed(raw, 1, pc_mapping=page | 4)),
                      ('static_stack_unaligned', changed(raw, 1, sp=raw[1]['sp'] + 1)),
                      ('dynamic_pc_changed_after_static', changed(raw, 3, pc=raw[3]['pc'] + 4))]
        for name, result in raw_cases:
            assert not validate_exec_layout(result, raw_images, guest, page, init_paths=RAW_PATHS), name
            raw_rejected.append(name)
        for fields, name in (({'type': 3}, 'raw_elf_not_et_exec'),
                              ({'interpreter': images['/system/bin/init']['interpreter']}, 'raw_elf_has_interpreter')):
            invalid_images = {**raw_images, '/init': {**spec, **fields}}
            assert not validate_exec_layout(raw, invalid_images, guest, page, init_paths=RAW_PATHS), name
            raw_rejected.append(name)
        output.append({'entry': run['entry'], 'mode': run['mode'], 'actual_exec_count': 3,
                       'actual_accepted': True, 'repeated_dynamic_address_accepted': True,
                       'rejected_cases': rejected, 'synthetic_raw': {
                           'exec_count': raw[-2]['stock_init_exec_count'], 'accepted': True,
                           'static_only_count': 1, 'static_only_accepted': True,
                           'rejected_cases': raw_rejected}})
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('reports', type=Path, nargs='+')
    parser.add_argument('--ramdisk', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    ramdisk = read_ramdisk(args.ramdisk)
    init, = [entry for entry in ramdisk['entries'] if entry['path'] == 'init']
    spec = elf_spec_bytes(init['data'], '/init')
    assert spec['type'] == 2 and spec['interpreter'] is None and spec['entry'] == 0x30c000
    assert len(spec['loads']) == 4 and all(load['align'] == 16384 for load in spec['loads'])
    results, sources = [], []
    for path in args.reports:
        packed = path.read_bytes()
        decoded = (subprocess.run(['zstd', '-d', '-q', '-c', '-'], input=packed,
                                  capture_output=True, timeout=30, check=True).stdout
                   if path.suffix == '.zst' else packed)
        payload = json.loads(decoded)
        sources.append({'path': str(path.resolve()), 'bytes': len(packed),
                        'sha256': hashlib.sha256(packed).hexdigest(), 'decoded_bytes': len(decoded),
                        'decoded_sha256': hashlib.sha256(decoded).hexdigest()})
        reports = ([(name, payload['reports'][name]) for name in ('core_4k', 'core_16k', 'init_4k', 'init_16k')]
                   if 'reports' in payload else [(path.stem, payload)])
        for name, report in reports:
            report = report['data'] if 'data' in report else report
            page = report.get('source_configuration', report.get('configuration'))['guest_page_bytes']
            if 'exec_aslr_fixtures' in report:
                runs = []
                for run in report['runs']:
                    guest = next(row for row in run['results'] if row.get('config') == 'translation_guest_layout')
                    assert run['validated'] is True
                    assert validate_core(run['results'], report['exec_aslr_fixtures'], guest, page)
                    runs.append({'entry': run['entry'], 'actual_core_accepted': True})
            else:
                runs = check_init(report, spec, page)
            results.append({'input': str(path.resolve()), 'report': name, 'guest_page_bytes': page, 'runs': runs})
    output = {'test': 'stock_init_first_stage_exec_contract', 'passed': True,
              'scope': '旧实际报告兼容性与真实 /init ELF 的合成 raw 执行关联；未运行 raw 客体。',
              'guest_executed': False, 'sources': sources,
              'ramdisk': {key: ramdisk[key] for key in ('input', 'input_bytes', 'input_sha256', 'decoded_bytes', 'decoded_sha256')},
              'init': {'bytes': init['bytes'], 'sha256': init['sha256'], 'elf_spec': spec}, 'reports': results,
              'source_sha256': {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                                for path in (Path(__file__), Path(__file__).with_name('exec_aslr.py'),
                                             Path(__file__).with_name('stock_init_sdk_ramdisk.py'))}}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(output, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps({'passed': True, 'actual_reports': len(results),
                      'actual_original': sum(run.get('actual_accepted', False) for result in results for run in result['runs']),
                      'synthetic_raw_only': True}))


if __name__ == '__main__':
    main()
