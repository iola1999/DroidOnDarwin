#!/usr/bin/env python3
"""用宿主 syscall fixture 检查实际 sysfs C 读取、错误回收及固定来源字段。"""
import argparse
import hashlib
import json
from pathlib import Path
import posixpath
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / 'experiments/native-linux/translation/stock-init-first-stage-devices.h'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def fixture_model(ranchu, data, metadata, data_sectors, gpt):
    model = []
    parent = '/sys/devices/platform/a003c00.virtio_mmio'

    def link(path, target):
        model.append({'operation': 'readlink', 'path': path, 'value': target})

    def read(path, value):
        model.append({'operation': 'read', 'path': path, 'value': value})

    link(parent + '/subsystem', '../../../bus/platform' if ranchu else None)
    link('/sys/class/block/' + ('ncedisk0' if ranchu else 'vdc'), None)
    sizes = [data_sectors, gpt['images'][0]['bytes'] // 512, gpt['images'][1]['bytes'] // 512]
    canonical = {}
    for slot, name, exists in [(0, 'vdc' if ranchu else 'ncedisk0', data), (1, 'ncedisk1', 1), (2, 'ncedisk2', metadata)]:
        directory = parent + '/block/' + name if ranchu else '/sys/devices/virtual/block/' + name
        canonical[name] = directory
        path = '/sys/class/block/' + name
        link(path, posixpath.relpath(directory, '/sys/class/block') if exists else None)
        if not exists:
            continue
        read(path + '/dev', f'254:{slot}\n')
        read(path + '/uevent', f'MAJOR=254\nMINOR={slot}\nDEVNAME={name}\nDEVTYPE=disk\nDISKSEQ={70 + slot}\n')
        read(path + '/ro', '1\n' if slot == 1 else '0\n')
        read(path + '/size', f'{sizes[slot]}\n')
        link(path + '/device', posixpath.relpath(parent, directory) if ranchu else None)
    partitions = [(1, 'ncedisk1', part, 13 + index, True)
                  for index, part in enumerate(gpt['images'][0]['partitions'])]
    partitions += [(2, 'ncedisk2', gpt['images'][1]['partitions'][0], 15, bool(metadata))]
    for slot, disk, part, minor, exists in partitions:
        name = f'{disk}p{part["number"]}'
        directory = canonical[disk] + '/' + name
        path = '/sys/class/block/' + name
        link(path, posixpath.relpath(directory, '/sys/class/block') if exists else None)
        if not exists:
            continue
        read(path + '/dev', f'259:{minor}\n')
        read(path + '/uevent', f'MAJOR=259\nMINOR={minor}\nDEVNAME={name}\nDEVTYPE=partition\nDISKSEQ={70 + slot}\nPARTN={part["number"]}\nPARTNAME={part["name"]}\n')
        read(path + '/ro', '1\n' if slot == 1 else '0\n')
        read(path + '/partition', f'{part["number"]}\n')
        read(path + '/start', f'{part["first_lba"]}\n')
        read(path + '/size', f'{part["bytes"] // 512}\n')
        link(path + '/subsystem', posixpath.relpath('/sys/class/block', directory))
    return model


FIXTURE = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct fixture_entry { int group, reading; const char *path, *value; };
#include "fixture-data.h"
static const char *fault;
static int group, fd = -1, opened, closed, unexpected, allocations, allocation_failures;
static int target_open_calls, target_read_calls, target_link_calls, calls;
static size_t offset;
static const struct fixture_entry *opened_entry;
static char long_text[4098];

static int same(const char *name) { return !strcmp(fault, name); }
static int target_file(const struct fixture_entry *entry) {
    return entry && !strcmp(entry->path, "/sys/class/block/vdc/dev");
}
static void *fixture_realloc(void *pointer, size_t size) {
    ++allocations;
    int fail = !allocation_failures &&
        ((same("alloc-first") && allocations == 1) ||
         (same("alloc-open") && allocations == 4) ||
         (same("alloc-read") && fd >= 0 && target_file(opened_entry)) ||
         (same("alloc-read-growth") && fd >= 0 && target_file(opened_entry) && target_read_calls == 8) ||
         (same("alloc-open-growth") && fd < 0 && target_open_calls == 8) ||
         (same("alloc-link-growth") && target_link_calls == 8));
    if (fail) { ++allocation_failures; return NULL; }
    return realloc(pointer, size);
}
static long call(long number, long args[6]);
#define realloc fixture_realloc
#include "stock-init-first-stage-devices.h"
#undef realloc

static const struct fixture_entry *lookup(const char *path, int reading) {
    for (unsigned i = 0; i < sizeof(fixture_entries) / sizeof(*fixture_entries); ++i) {
        const struct fixture_entry *entry = &fixture_entries[i];
        if (entry->group == group && !strcmp(entry->path, path)) {
            if (entry->reading != reading) { ++unexpected; return NULL; }
            return entry;
        }
    }
    ++unexpected;
    return NULL;
}

static long call(long number, long args[6]) {
    ++calls;
    if (number == 57) {
        if (fd < 0 || fd != args[0]) { ++unexpected; return -9; }
        int target = target_file(opened_entry);
        fd = -1;
        ++closed;
        if (target && same("close-error")) return -5;
        if (target && same("close-eintr")) return -4;
        if (target && same("close-positive")) return 1;
        return 0;
    }
    if (number == 63) {
        if (fd < 0 || fd != args[0] || !args[1] || args[2] < 1) { ++unexpected; return -22; }
        if (same("partition-ro-error") && !strcmp(opened_entry->path, "/sys/class/block/ncedisk1p2/ro")) return -13;
        int target = target_file(opened_entry);
        if (target) {
            int attempt = target_read_calls++;
            if (same("read-eintr") && attempt == 0) return -4;
            if (same("alloc-read-growth") && attempt < 8) return -4;
            if (same("read-error")) return -13;
            if (same("first-error")) return -5;
            if (same("read-partial-error") && attempt > 0) return -13;
            if (same("read-overshoot")) return args[2] + 1;
        }
        const char *value = opened_entry->value;
        if (target && (same("read-limit") || same("read-long"))) value = long_text;
        size_t size = strlen(value), count = args[2];
        if (target && (same("read-short") || same("read-partial-error")) && count > 2) count = 2;
        if (offset >= size) return 0;
        if (count > size - offset) count = size - offset;
        memcpy((void *)args[1], value + offset, count);
        offset += count;
        return count;
    }
    if (number != 56 && number != 78) { ++unexpected; return -38; }
    if (args[0] != -100 || !args[1]) { ++unexpected; return -22; }
    int reading = number == 56;
    const struct fixture_entry *entry = lookup((const char *)args[1], reading);
    if (!entry) return -22;
    if (same("missing-part") && !strncmp(entry->path, "/sys/class/block/ncedisk1p2", strlen("/sys/class/block/ncedisk1p2"))) return -2;
    if (reading) {
        if (args[2] != 0x80000 || fd >= 0) { ++unexpected; return -22; }
        if (target_file(entry)) {
            int attempt = target_open_calls++;
            if (same("open-eintr") && attempt == 0) return -4;
            if (same("open-many-eintr") && attempt < 19) return -4;
            if (same("alloc-open-growth") && attempt < 8) return -4;
            if (same("open-error")) return -13;
        }
        if (!entry->value) return -2;
        opened_entry = entry;
        offset = 0;
        fd = 100 + opened++;
        return fd;
    }
    if (!args[2] || args[3] != 4097) { ++unexpected; return -22; }
    if (same("alias-error") && !strcmp(entry->path, "/sys/class/block/ncedisk0")) return -13;
    int target = !strcmp(entry->path, "/sys/devices/platform/a003c00.virtio_mmio/subsystem");
    if (target) {
        int attempt = target_link_calls++;
        if (same("link-eintr") && attempt == 0) return -4;
        if (same("alloc-link-growth") && attempt < 8) return -4;
        if (same("link-error") || same("first-error")) return -13;
        if (same("link-overshoot")) return args[3] + 1;
    }
    if (!entry->value) return -2;
    const char *value = entry->value;
    if (target && (same("link-limit") || same("link-long"))) value = long_text;
    size_t count = strlen(value);
    if (count > (size_t)args[3]) count = args[3];
    memcpy((void *)args[2], value, count);
    return count;
}

int main(int argc, char **argv) {
    if (argc != 5) return 2;
    fault = argv[1];
    int ranchu = atoi(argv[2]), data = atoi(argv[3]), metadata = atoi(argv[4]);
    group = ranchu >= 0 && ranchu <= 1 && data >= 0 && data <= 1 && metadata >= 0 && metadata <= 1
        ? (ranchu << 2) | (data << 1) | metadata : -1;
    memset(long_text, '7', same("read-limit") || same("link-limit") ? 4096 : 4097);
    long result = stock_init_first_stage_devices(ranchu, data, metadata);
    printf("{\"fixture\":true,\"result\":%ld,\"calls\":%d,\"opened\":%d,\"closed\":%d,\"fd_remaining\":%d,\"unexpected\":%d,\"allocation_failures\":%d}\n", result, calls, opened, closed, fd >= 0, unexpected, allocation_failures);
    return 0;
}
'''


def validate_rows(rows, model, flags, expected_result, fault):
    footer = rows.pop()
    summary = rows.pop()
    assert footer['fixture'] is True and footer['result'] == expected_result
    assert footer['opened'] == footer['closed'] and footer['fd_remaining'] == 0 and footer['unexpected'] == 0
    assert summary == {'stock_init_first_stage_devices': True, 'ranchu': flags[0], 'data': flags[1],
                       'metadata': flags[2], 'result': expected_result}
    expected = [] if expected_result == -22 else model
    actual = [(row['operation'], bytes.fromhex(row['path_hex']).decode()) for row in rows]
    assert actual == [(item['operation'], item['path']) for item in expected]
    for row in rows:
        assert set(row) == {'stock_init_first_stage_sysfs', 'operation', 'path_hex', 'value_hex', 'open_results',
                            'read_results', 'close_result', 'readlink_results', 'result'}
        assert row['stock_init_first_stage_sysfs'] is True and type(row['result']) is int
        assert len(bytes.fromhex(row['value_hex'])) <= 4097
        for key in ('open_results', 'read_results', 'readlink_results'):
            assert row[key] is None or (row[key] and all(type(value) is int for value in row[key]))
        if row['operation'] == 'readlink':
            assert row['open_results'] is None and row['read_results'] is None and row['close_result'] is None
        elif row['open_results'] is not None and row['open_results'][-1] >= 0:
            assert type(row['close_result']) is int
        else:
            assert row['read_results'] is None and row['close_result'] is None
    if fault == 'none':
        for row, item in zip(rows, model):
            value = item['value']
            assert row['value_hex'] == (value.encode().hex() if value is not None else '')
            assert row['result'] == (0 if value is not None else -2)
            if item['operation'] == 'read':
                assert row['read_results'] == [len(value), 0] and row['close_result'] == 0
            else:
                assert row['readlink_results'] == ([len(value)] if value is not None else [-2])
    by_path = {bytes.fromhex(row['path_hex']).decode(): row for row in rows}
    target = by_path.get('/sys/class/block/vdc/dev')
    link = by_path.get('/sys/devices/platform/a003c00.virtio_mmio/subsystem')
    if fault in ('open-eintr', 'read-eintr', 'link-eintr'):
        selected = {'open-eintr': (target, 'open_results'), 'read-eintr': (target, 'read_results'), 'link-eintr': (link, 'readlink_results')}[fault]
        assert selected[0][selected[1]][0] == -4 and selected[0]['result'] == 0
    if fault == 'open-many-eintr':
        assert target['open_results'][:-1] == [-4] * 19 and target['open_results'][-1] >= 0
    if fault == 'read-short':
        assert target['read_results'] == [2, 2, 2, 0] and target['value_hex'] == b'254:0\n'.hex()
    if fault == 'read-partial-error':
        assert target['read_results'] == [2, -13] and target['value_hex'] == b'25'.hex() and target['close_result'] == 0
    if fault == 'open-error':
        assert target['open_results'] == [-13] and target['read_results'] is None and target['close_result'] is None
    if fault == 'read-error':
        assert target['read_results'] == [-13] and target['close_result'] == 0
    if fault in ('close-error', 'close-eintr', 'close-positive'):
        assert target['close_result'] == {'close-error': -5, 'close-eintr': -4, 'close-positive': 1}[fault]
        assert footer['opened'] == footer['closed']
    if fault == 'first-error':
        assert link['result'] == -13 and target['result'] == -5 and summary['result'] == -13
    if fault == 'alias-error':
        selected = by_path['/sys/class/block/ncedisk0']
        assert selected['readlink_results'] == [-13] and selected['result'] == -13
    if fault == 'partition-ro-error':
        selected = by_path['/sys/class/block/ncedisk1p2/ro']
        assert selected['read_results'] == [-13] and selected['close_result'] == 0 and selected['result'] == -13
    if fault in ('read-limit', 'read-long', 'link-limit', 'link-long'):
        selected = target if fault.startswith('read') else link
        limit = 4097 if fault.endswith('long') else 4096
        assert bytes.fromhex(selected['value_hex']) == b'7' * limit
        assert selected['result'] == (-7 if limit == 4097 else 0)
    if fault.startswith('alloc-'):
        assert footer['allocation_failures'] == 1
        if fault == 'alloc-read':
            assert target['open_results'][-1] >= 0 and target['read_results'] is None and target['close_result'] == 0
        if fault == 'alloc-open':
            assert target['open_results'] is None and target['close_result'] is None
        if fault == 'alloc-read-growth':
            assert target['read_results'] == [-4] * 8 and target['close_result'] == 0
        if fault == 'alloc-open-growth':
            assert target['open_results'] == [-4] * 8 and target['close_result'] is None
        if fault == 'alloc-link-growth':
            assert link['readlink_results'] == [-4] * 8
        if fault == 'alloc-first':
            assert link['readlink_results'] is None
    return rows + [summary], footer


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', type=Path, required=True)
    parser.add_argument('--gpt-report', type=Path, required=True)
    parser.add_argument('--output', type=Path, default=ROOT / 'work/first-stage-device-source-review-v1/host-fixture-v3')
    args = parser.parse_args()
    archive_path = args.archive.resolve()
    gpt_report = args.gpt_report.resolve()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    source_sha = digest(HEADER)
    archive_bytes = subprocess.check_output(['zstd', '-dc', str(archive_path)], timeout=60)
    archive = json.loads(archive_bytes)
    input_data = archive['reports']['init_4k']['data']['input']['data_image']
    data_sectors = input_data['bytes'] // 512
    gpt = json.loads(gpt_report.read_text())
    models = {(ranchu, data, metadata): fixture_model(ranchu, data, metadata, data_sectors, gpt)
              for ranchu in (0, 1) for data in (0, 1) for metadata in (0, 1)}
    generated = ['static const struct fixture_entry fixture_entries[] = {']
    for flags, model in models.items():
        group = flags[0] << 2 | flags[1] << 1 | flags[2]
        for item in model:
            value = json.dumps(item['value']) if item['value'] is not None else 'NULL'
            generated.append(f'    {{{group}, {int(item["operation"] == "read")}, {json.dumps(item["path"])}, {value}}},')
    generated.append('};')
    (out / 'fixture-data.h').write_text('\n'.join(generated) + '\n')
    (out / 'fixture.c').write_text(FIXTURE)
    compiler = shutil.which('clang')
    command = [compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(HEADER.parent),
               str(out / 'fixture.c'), '-o', str(out / 'fixture')]
    compiled = subprocess.run(command, text=True, capture_output=True, timeout=30)
    (out / 'compile.stdout').write_text(compiled.stdout)
    (out / 'compile.stderr').write_text(compiled.stderr)
    (out / 'commands.json').write_text(json.dumps({'test': {'argv': [sys.executable, *sys.argv], 'cwd': str(Path.cwd())},
                                                 'compile': command}, indent=2) + '\n')
    compiled.check_returncode()
    cases = [('none', flags, 0) for flags in models]
    errors = {'open-error': -13, 'read-error': -13, 'read-partial-error': -13, 'close-error': -5,
              'close-eintr': -4, 'close-positive': -5, 'link-error': -13, 'first-error': -13,
              'read-long': -7, 'link-long': -7, 'read-overshoot': -5, 'link-overshoot': -5,
              'alloc-first': -12, 'alloc-open': -12, 'alloc-read': -12, 'alloc-read-growth': -12,
              'alloc-open-growth': -12, 'alloc-link-growth': -12, 'missing-part': -2,
              'alias-error': -13, 'partition-ro-error': -13}
    cases += [(fault, (1, 1, 1), errors.get(fault, 0)) for fault in
              ['open-eintr', 'open-many-eintr', 'read-eintr', 'read-short', 'link-eintr',
               'read-limit', 'link-limit', *errors]]
    cases += [('none', flags, -22) for flags in [(2, 1, 1), (1, 2, 1), (1, 1, 2), (-1, 1, 1), (1, -1, 1), (1, 1, -1)]]
    results = []
    for index, (fault, flags, expected) in enumerate(cases):
        command = [str(out / 'fixture'), fault, *map(str, flags)]
        ran = subprocess.run(command, text=True, capture_output=True, timeout=5)
        ran.check_returncode()
        assert not ran.stderr
        rows, footer = validate_rows([json.loads(line) for line in ran.stdout.splitlines()],
                                     models.get(flags, []), flags, expected, fault)
        result = {'case': index, 'fault': fault, 'flags': flags, 'passed': True, 'results': rows, 'fixture': footer}
        results.append(result)
        if fault == 'none' and flags in ((1, 1, 1), (0, 1, 1)):
            name = 'raw-baseline.json' if flags[0] else 'default-baseline.json'
            (out / name).write_text(json.dumps({'scope': 'Synthetic host syscall fixture of the actual C header; no guest or disk access.',
                                               'synthetic_device_numbers': {'whole_major': 254, 'partition_major': 259, 'partition_minors': [13, 14, 15]},
                                               'ranchu': flags[0], 'data': flags[1], 'metadata': flags[2],
                                               'results': rows}, ensure_ascii=False, indent=2) + '\n')
    result = {'test': 'first_stage_sysfs_actual_c_host_fixture', 'passed': True, 'case_count': len(results),
              'scope': 'Synthetic sysfs values and Linux syscall returns; actual header compiled and executed on macOS. No kernel, guest or disk operation.',
              'inputs': {'header': {'path': str(HEADER), 'sha256': source_sha},
                         'test_script': {'path': str(Path(__file__).resolve()), 'sha256': digest(Path(__file__))},
                         'fixed_gpt_report': {'path': str(gpt_report), 'sha256': digest(gpt_report)},
                         'archived_capacity': {'path': str(archive_path), 'sha256': digest(archive_path),
                                               'decoded_sha256': hashlib.sha256(archive_bytes).hexdigest(),
                                               'json_path': 'reports.init_4k.data.input.data_image.bytes', 'bytes': input_data['bytes']}},
              'synthetic_device_numbers': {'whole_major': 254, 'partition_major': 259, 'partition_minors': [13, 14, 15]},
              'sources_unchanged': digest(HEADER) == source_sha, 'cases': results,
              'cleanup': {'removed': [{'path': str(out / 'fixture'), 'bytes': (out / 'fixture').stat().st_size, 'sha256': digest(out / 'fixture')}],
                          'retained': ['fixture.c', 'fixture-data.h', 'compile.stdout', 'compile.stderr', 'commands.json',
                                       'raw-baseline.json', 'default-baseline.json', 'result.json']}}
    (out / 'fixture').unlink()
    (out / 'result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n')
    assert result['sources_unchanged']
    print(json.dumps({'result': str(out / 'result.json'), 'sha256': digest(out / 'result.json'),
                      'cases': len(results), 'passed': True, 'header_sha256': source_sha}))


if __name__ == '__main__':
    main()
