#!/usr/bin/env python3
"""用已保存报告执行原 metadata 汇总代码，核对完成条件和失败结果保存。"""
import argparse
import ast
from copy import deepcopy
import hashlib
import json
from pathlib import Path
from types import SimpleNamespace

from probe_stock_init import ROOT, validate
from stock_init_metadata_volume import ORIGINAL_SHA, rc_results


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def completion_code(path):
    tree = ast.parse(path.read_text(), filename=str(path))
    main = next(node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == 'main')
    blocks = [node for node in main.body if isinstance(node, ast.If) and
              isinstance(node.test, ast.Attribute) and node.test.attr == 'metadata_image' and
              any(isinstance(target, ast.Subscript) and isinstance(target.value, ast.Name) and
                  target.value.id == 'result' and isinstance(target.slice, ast.Constant) and
                  target.slice.value == 'metadata_rc_validated'
                  for assignment in ast.walk(node) if isinstance(assignment, ast.Assign)
                  for target in assignment.targets)]
    if len(blocks) != 1:
        raise ValueError('需要唯一的原 metadata 完成检查代码块')
    return compile(ast.Module(body=[blocks[0]], type_ignores=[]), str(path), 'exec')


def stage_input(report):
    return {'runs': [{key: deepcopy(run[key]) for key in
                     ('entry', 'mode', 'phase', 'observation_validated', 'metadata_after', 'metadata_rc')}
                    for run in report['runs']]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--historical-report', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists():
        parser.error('--output 需要新目录')
    output.mkdir(parents=True)
    paths = (args.report.resolve(), args.historical_report.resolve(), Path(__file__).resolve(),
             ROOT / 'scripts/probe_stock_init.py', ROOT / 'scripts/stock_init_metadata_volume.py')
    result = {'test': 'stock_init_metadata_completion_contract', 'passed': False,
              'scope': '仅执行原 CLI 的 metadata 汇总 AST 代码块；输入来自已保存报告。原盘摘要函数使用固定返回，不读取镜像，不启动客体或编译。',
              'report': str(paths[0]), 'historical_report': str(paths[1]), 'cases': [],
              'sha256': {str(path): sha(path) for path in paths}}
    try:
        report = json.loads(paths[0].read_text())
        code = completion_code(paths[3])
        actual = stage_input(report)

        def checked(name, stage, lifecycle_only=False, metadata_enabled=True, digest=ORIGINAL_SHA, expected_error=None, expected_rc=True):
            directory = output / name
            directory.mkdir()
            original = deepcopy(stage)
            calls = []

            def fixed_sha(path):
                calls.append(str(path))
                return digest

            namespace = {'args': SimpleNamespace(metadata_image=metadata_enabled, lifecycle_only=lifecycle_only),
                         'result': deepcopy(stage), 'out': directory, 'json': json,
                         'previous_generation': {'metadata': report['metadata_final_generation']},
                         'metadata_original': Path('__nce_unread_metadata_original__'),
                         'ORIGINAL_SHA': ORIGINAL_SHA, 'sha': fixed_sha}
            error = None
            try:
                exec(code, namespace)
            except ValueError as failure:
                error = str(failure)
            summary = directory / 'summary.json'
            saved = json.loads(summary.read_text()) if summary.exists() else None
            enabled = metadata_enabled is True
            passed = error == expected_error and ((saved is not None and
                        saved.get('metadata_rc_validated') is expected_rc and
                        saved == namespace['result'] and calls == ['__nce_unread_metadata_original__']) if enabled else
                        saved is None and namespace['result'] == original and not calls)
            result['cases'].append({'case': name, 'lifecycle_only': lifecycle_only,
                                    'metadata_enabled': enabled, 'error': error, 'expected_error': expected_error,
                                    'summary_saved': saved is not None, 'metadata_rc_validated': saved.get('metadata_rc_validated') if saved else None,
                                    'passed': passed})

        checked('full_actual', actual)
        recovery = next(run for run in report['runs'] if run['phase'] == 'recovery' and run['entry'] == 'cli')
        stage_index = next(index for index, run in enumerate(actual['runs']) if run['phase'] == 'recovery' and run['entry'] == 'cli')
        for field, value in (('mode', 0o754), ('uid', 1), ('gid', 1), ('label_hex', 'u:object_r:metadata_file:s0\0'.encode().hex())):
            rows = deepcopy(recovery['results'])
            directory = next(row for row in rows if row.get('stock_init_metadata_directory') and row['path'] == '/metadata/apex/sessions')
            directory[field] = value
            if field == 'label_hex':
                directory['label_result'] = len(value) // 2
            structurally_valid = validate(rows, recovery['mode'], True,
                                          guest_page_bytes=report['source_configuration']['guest_page_bytes'],
                                          kernel_log=recovery['stderr'], original_input=report['input'],
                                          exec_elf_images=report['input'].get('exec_elf_images'))
            observed = rc_results(rows)
            if not structurally_valid or observed['directories_match_original_rc'] is not False:
                raise ValueError('需要 ABI 有效且与原 rc 不同的目录属性：' + field)
            stage = deepcopy(actual)
            stage['runs'][stage_index]['metadata_rc'] = observed
            checked('full_different_rc_' + field, stage, expected_error='metadata 原目录或 APEX sessions 验收失败', expected_rc=False)

        original = next(run for run in report['runs'] if run['mode'] == 'original' and run['entry'] == 'cli')
        rows = [deepcopy(row) for row in original['results'] if 'stock_init_metadata_directory' in row or
                (row.get('stock_init_block') == 56 and row.get('paths_hex') == ['/metadata/apex/sessions'.encode().hex()])]
        for row in rows:
            if 'stock_init_block' in row:
                row['result'] = -13
        stage = deepcopy(actual)
        index = next(index for index, run in enumerate(stage['runs']) if run['mode'] == 'original' and run['entry'] == 'cli')
        stage['runs'][index]['metadata_rc'] = rc_results(rows)
        checked('full_sessions_open_failed', stage, expected_error='metadata 原目录或 APEX sessions 验收失败', expected_rc=False)

        lifecycle = {'runs': [deepcopy(run) for run in actual['runs'] if run['phase'] == 'lifecycle']}
        checked('lifecycle_only_actual', lifecycle, lifecycle_only=True, expected_rc=False)
        checked('full_lifecycle_inputs', lifecycle, expected_error='metadata 持久化或原盘摘要检查失败', expected_rc=False)
        checked('full_original_digest_changed', actual, digest='0' * 64,
                expected_error='metadata 持久化或原盘摘要检查失败')
        checked('lifecycle_only_original_digest_changed', lifecycle, lifecycle_only=True, digest='0' * 64,
                expected_error='metadata 持久化或原盘摘要检查失败', expected_rc=False)

        historical = json.loads(paths[1].read_text())
        if historical['input'].get('metadata_image'):
            raise ValueError('历史报告需要保留无 metadata 的实际输入')
        checked('historical_no_metadata', {'runs': []}, metadata_enabled=False, expected_rc=None)
        result['historical_checks'] = [{'entry': run['entry'], 'phase': run['phase'],
                                       'passed': validate(run['results'], run['mode'], True,
                                                          guest_page_bytes=historical['source_configuration'].get('guest_page_bytes', 16384),
                                                          kernel_log=run['stderr'], original_input=historical['input'],
                                                          exec_elf_images=historical['input'].get('exec_elf_images'))}
                                      for run in historical['runs']]
        result['inputs_unchanged'] = all(sha(Path(path)) == digest for path, digest in result['sha256'].items())
        result['passed'] = result['inputs_unchanged'] and all(row['passed'] for row in result['cases'] + result['historical_checks'])
    except (OSError, KeyError, TypeError, ValueError, IndexError, StopIteration) as error:
        result['error'] = str(error)
    result['case_count'] = len(result['cases'])
    result['failed_cases'] = [row for row in result['cases'] if not row['passed']]
    (output / 'result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps({'passed': result['passed'], 'cases': result['case_count'], 'output': str(output / 'result.json')}))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
