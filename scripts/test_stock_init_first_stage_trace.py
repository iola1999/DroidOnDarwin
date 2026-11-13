#!/usr/bin/env python3
"""用宿主 fixture 检查原 first-stage 路径过滤、DM 输入采样与参数读取范围。"""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = r'''#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct { unsigned long base, size; } regions[16];
static unsigned region_count, copy_calls, pid_calls, private_attempts;
static unsigned long private_start, private_end;

static void region(const void *pointer, size_t size)
{
    assert(region_count < 16);
    regions[region_count++] = (typeof(regions[0])){(unsigned long)pointer, size};
}

static unsigned long copy_user(void *destination, unsigned long address, unsigned long bytes, int write)
{
    ++copy_calls;
    if (write || address + bytes < address)
        return bytes;
    if (private_start && address < private_end && address + bytes > private_start) {
        ++private_attempts;
        return bytes;
    }
    for (unsigned i = 0; i < region_count; ++i) {
        unsigned long offset = address - regions[i].base;
        if (address >= regions[i].base && offset <= regions[i].size && bytes <= regions[i].size - offset) {
            memcpy(destination, (void *)address, bytes);
            return 0;
        }
    }
    return bytes;
}

static long run_syscall(long number, const long *args)
{
    assert(number == 172);
    for (unsigned i = 0; i < 6; ++i)
        assert(!args[i]);
    ++pid_calls;
    return 1234;
}

#include STOCK_INIT_TRACE_HEADER

static void args_json(FILE *stream, const long *args)
{
    fprintf(stream, "[%lu,%lu,%lu,%lu,%lu,%lu]", (unsigned long)args[0], (unsigned long)args[1],
            (unsigned long)args[2], (unsigned long)args[3], (unsigned long)args[4], (unsigned long)args[5]);
}

int main(int argc, char **argv)
{
    (void)stock_init_trace_write;
    (void)stock_init_trace_security;
    (void)stock_init_trace_result;
    for (int i = 1; i < argc; ++i)
        region(argv[i], strlen(argv[i]) + 1);
    if (argc == 7 && !strcmp(argv[1], "block")) {
        long number = strtol(argv[2], NULL, 10), result = strtol(argv[3], NULL, 10);
        long args[6] = {-100, (long)argv[4], 0755, 0x103, 0, 0};
        if (number == 36) {
            args[0] = (long)argv[4];
            args[1] = -100;
            args[2] = (long)argv[5];
        } else if (number == 39 || number == 49 || number == 51) {
            args[0] = (long)argv[4];
            args[1] = 2;
        } else if (number == 40) {
            args[0] = (long)argv[4];
            args[1] = (long)argv[5];
            args[2] = (long)argv[6];
        }
        stock_init_trace_block(number, args, result);
        fprintf(stderr, "{\"copy_calls\":%u,\"pid_calls\":%u,\"args\":", copy_calls, pid_calls);
        args_json(stderr, args);
        fputs("}\n", stderr);
        return 0;
    }
#ifdef NCE_STOCK_INIT_RAMDISK
    if (argc == 6 && !strcmp(argv[1], "dm")) {
        union stock_init_dm_message message = {0};
        struct stock_init_dm_info *info = &message.info;
        info->version[0] = 4;
        info->version[1] = 48;
        info->data_start = sizeof(*info);
        info->target_count = 1;
        info->flags = 1;
        info->dev = 0x103;
        memcpy(info->name, "system", sizeof("system"));
        struct stock_init_dm_target *target = (void *)(message.data + info->data_start);
        target->sector_start = 0;
        target->length = 2048;
        snprintf(target->type, sizeof(target->type), "%s", argv[2]);
        unsigned char *parameters = (void *)(target + 1);
        size_t size = strlen(argv[3]) + 1;
        assert(sizeof(*info) + sizeof(*target) + size < sizeof(message));
        memcpy(parameters, argv[3], size);
        target->next = (sizeof(*target) + size + 7) & ~7U;
        info->data_size = info->data_start + target->next;
        size_t readable = sizeof(message);
        const char *variant = argv[4];
        long number = 29, args[6] = {13, 0xc138fd09U, (long)&message};
        if (!strcmp(variant, "count_zero")) info->target_count = 0;
        if (!strcmp(variant, "count_two")) info->target_count = 2;
        if (!strcmp(variant, "flags_high")) info->flags = UINT32_MAX;
        if (!strcmp(variant, "bad_start")) info->data_start = 304;
        if (!strcmp(variant, "unaligned_start")) info->data_start = 316;
        if (!strcmp(variant, "bad_size")) info->data_size = sizeof(*info) + sizeof(*target);
        if (!strcmp(variant, "version_other")) info->version[0] = 5;
        if (!strcmp(variant, "name_unterminated")) memset(info->name, 'A', sizeof(info->name));
        if (!strcmp(variant, "type_unterminated")) memset(target->type, 'x', sizeof(target->type));
        if (!strcmp(variant, "header_unreadable")) readable = 300;
        if (!strcmp(variant, "target_unreadable")) readable = sizeof(*info) + sizeof(*target) - 1;
        if (!strcmp(variant, "parameter_unreadable")) readable = sizeof(*info) + sizeof(*target);
        if (!strcmp(variant, "parameter_unterminated")) memset(parameters, '1', target->next - sizeof(*target));
        if (!strcmp(variant, "address_overflow")) args[2] = (long)(UINT64_MAX - 31);
        if (!strcmp(variant, "address_null")) args[2] = 0;
        if (!strcmp(variant, "request_other")) args[1] = 0xc138fd08U;
        if (!strcmp(variant, "number_other")) number = 28;
        region(&message, readable);
        if (strcmp(argv[2], "linear")) {
            private_start = (unsigned long)parameters;
            private_end = (unsigned long)&message + sizeof(message);
        }
        struct stock_init_dm_info input = *info;
        struct stock_init_trace_dm_sample sample;
        stock_init_trace_dm_before(number, args, &sample);
        unsigned before = copy_calls;
        /* 返回后用户 buffer 已变化，after 必须只使用调用前的 sample。 */
        memset(&message, 0xa5, sizeof(message));
        stock_init_trace_dm_after(&sample, args, strtol(argv[5], NULL, 10));
        fprintf(stderr, "{\"copy_before\":%u,\"copy_after\":%u,\"private_attempts\":%u,\"pid_calls\":%u,\"args\":",
                before, copy_calls - before, private_attempts, pid_calls);
        args_json(stderr, args);
        fprintf(stderr, ",\"header\":{\"version\":[%u,%u,%u],\"data_size\":%u,\"data_start\":%u,\"target_count\":%u,\"flags\":%u,\"dev\":%llu}}\n",
                input.version[0], input.version[1], input.version[2], input.data_size, input.data_start,
                input.target_count, input.flags, (unsigned long long)input.dev);
        return 0;
    }
#endif
    return 2;
}
'''


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def invoke(binary, arguments):
    command = [str(binary), *map(str, arguments)]
    process = subprocess.run(command, capture_output=True, text=True, timeout=10, check=True)
    return command, [json.loads(line) for line in process.stdout.splitlines()], json.loads(process.stderr)


def check_blocks(binary, raw):
    cases = [
        ('mkdir', 34, '/first_stage_ramdisk', '', '', True, False),
        ('unlink', 35, '/init', '', '', True, False),
        ('symlink', 36, '/dev/block/vdb1', '/dev/block/by-name/super', '', True, False),
        ('umount', 39, '/system', '', '', True, False),
        ('chdir', 49, '/system', '', '', True, False),
        ('chroot', 51, '/system', '', '', True, False),
        ('block_node', 33, '/dev/block/vdb1', '', '', True, False),
        ('block_open', 56, '/dev/block/by-name/super', '', '', True, False),
        ('sys_open', 56, '/sys/devices/platform/ncedisk/dev', '', '', True, False),
        ('sys_node', 33, '/sys/no-node', '', '', True, False),
        ('sys_root', 56, '/sys', '', '', True, False),
        ('block_root', 56, '/dev/block', '', '', True, False),
        ('sys_prefix_collision', 56, '/system', '', '', False, False),
        ('block_prefix_collision', 33, '/dev/block-extra', '', '', False, False),
        ('ordinary_open', 56, '/etc/fstab', '', '', False, False),
        ('legacy_loop', 33, '/dev/loop0', '', '', True, True),
        ('legacy_mapper', 56, '/dev/device-mapper', '', '', True, True),
        ('legacy_dm', 56, '/dev/dm-0', '', '', True, True),
        ('legacy_metadata', 56, '/metadata/vold', '', '', True, True),
        ('mount', 40, '/dev/block/dm-0', '/system', 'erofs', True, True),
        ('ioctl', 29, '', '', '', True, True),
        ('unrelated_close', 57, '', '', '', False, False),
    ]
    output = []
    for name, number, first, second, third, raw_expected, old_expected in cases:
        result = -13 if name in ('sys_node', 'umount') else 0
        command, rows, calls = invoke(binary, ('block', number, result, first, second, third))
        expected = raw_expected if raw else old_expected
        assert len(rows) == int(expected), name
        assert calls['pid_calls'] == int(expected), name
        if expected:
            paths = [] if number == 29 else [first, second, third] if number == 40 else [first, second] if number == 36 else [first]
            assert rows == [{'stock_init_block': number, 'pid': 1234, 'result': result,
                             'args': calls['args'], 'paths_hex': [path.encode().hex() for path in paths]}], name
        output.append({'case': name, 'command': command, 'rows': rows, 'calls': calls, 'passed': True})
    return output


def check_dm(binary):
    secret = 'TRACE_NONLINEAR_PARAMETER_PAYLOAD'
    cases = [
        ('numeric', 'linear', '259:3 4096', 'normal', 0, 0),
        ('numeric_zero', 'linear', '0:0 0', 'normal', 0, -19),
        ('numeric_max', 'linear', '4095:1048575 18446744073709551615', 'normal', 0, 0),
        ('path', 'linear', '/dev/block/vdb1 2048', 'normal', 0, 0),
        ('path_nested', 'linear', '/dev/block/platform/nce/super 0', 'normal', 0, 0),
        ('nonlinear', 'crypt', secret, 'normal', 0, -22),
        ('nonlinear_unreadable', 'default-key', secret, 'parameter_unreadable', 0, -14),
        ('flags_high', 'linear', '259:3 4096', 'flags_high', 0, 0),
    ]
    for name, parameters in (
            ('negative_major', '-1:3 0'), ('signed_major', '+1:3 0'), ('major_overflow', '4096:3 0'),
            ('minor_overflow', '1:1048576 0'), ('sector_overflow', '1:3 18446744073709551616'),
            ('signed_sector', '1:3 +0'), ('negative_sector', '1:3 -0'), ('extra_parameter', '1:3 0 extra'),
            ('extra_space', '1:3  0'), ('tab_delimiter', '1:3\t0'), ('relative_path', 'dev/block/vdb1 0'),
            ('outside_path', '/dev/ncesystem 0'), ('prefix_path', '/dev/blockade/vdb1 0'),
            ('dot_path', '/dev/block/./vdb1 0'), ('dotdot_path', '/dev/block/../vdb1 0'),
            ('empty_component', '/dev/block//vdb1 0'), ('trailing_slash', '/dev/block/vdb1/ 0')):
        cases.append((name, 'linear', parameters, 'normal', -22, -22))
    for variant, error in (
            ('count_zero', -7), ('count_two', -7), ('bad_start', -22), ('unaligned_start', -22),
            ('bad_size', -22), ('version_other', -22), ('name_unterminated', -22),
            ('type_unterminated', -22), ('header_unreadable', -14), ('target_unreadable', -14),
            ('parameter_unreadable', -14), ('parameter_unterminated', -22),
            ('address_overflow', -22), ('address_null', -14), ('request_other', None), ('number_other', None)):
        cases.append((variant, 'linear', '1:3 0', variant, error, -22))
    cases.append(('parameter_limit', 'linear', '9' * 300, 'normal', -7, -22))
    output = []
    for name, kind, parameters, variant, error, result in cases:
        command, rows, calls = invoke(binary, ('dm', kind, parameters, variant, result))
        assert calls['copy_after'] == calls['private_attempts'] == 0, name
        assert len(rows) == (0 if error is None else 1), name
        assert calls['pid_calls'] == len(rows), name
        if error is None:
            assert calls['copy_before'] == 0, name
        else:
            row = rows[0]
            assert row['stock_init_dm_table_load'] is True and row['pid'] == 1234 and row['fd'] == 13, name
            assert row['result'] == result and row['read_result'] == error, name
            header = calls['header']
            if variant in ('header_unreadable', 'address_overflow', 'address_null'):
                header = {key: [0, 0, 0] if key == 'version' else 0 for key in header}
            assert all(row[key] == value for key, value in header.items()), name
            name_hex = ('A' * 128 if variant == 'name_unterminated' else '' if not header['version'][0] else 'system').encode().hex()
            assert row['name_hex'] == name_hex, name
            target_expected = variant not in ('count_zero', 'count_two', 'bad_start', 'unaligned_start', 'bad_size',
                                              'version_other', 'name_unterminated', 'header_unreadable',
                                              'target_unreadable', 'address_overflow', 'address_null')
            assert len(row['targets']) == int(target_expected), name
            if target_expected:
                target = row['targets'][0]
                expected_type = 'x' * 16 if variant == 'type_unterminated' else kind
                base = {'type_hex': expected_type.encode().hex(), 'sector_start': 0, 'length': 2048}
                if kind == 'linear' and not error:
                    backing, start = parameters.split(' ')
                    base['backing_start_sector'] = int(start)
                    if backing.startswith('/'):
                        base['backing_path_hex'] = backing.encode().hex()
                    else:
                        base['backing_major'], base['backing_minor'] = map(int, backing.split(':'))
                assert target == base, name
            assert secret not in json.dumps(row) and secret.encode().hex() not in json.dumps(row), name
        output.append({'case': name, 'command': command, 'rows': rows, 'calls': calls, 'passed': True})
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--trace-header', type=Path, default=ROOT / 'experiments/native-linux/translation/stock-init-trace.h')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    result = {'test': 'stock_init_first_stage_trace_contract', 'passed': False,
              'host': platform.platform(), 'scope': '宿主内存 fixture；实际 guest ioctl、LP 映射与磁盘未运行。'}
    try:
        header = args.trace_header.resolve(strict=True)
        abi = header.with_name('stock-init-dm-abi.h')
        fixture = out / 'fixture.c'
        fixture.write_text(FIXTURE)
        inputs = {str(path): sha(path) for path in (Path(__file__).resolve(), header, abi, fixture)}
        result['inputs'] = inputs
        compiler = shutil.which('clang')
        if compiler is None:
            raise RuntimeError('缺少 clang')
        result['compiler'] = subprocess.run([compiler, '--version'], capture_output=True, text=True, timeout=10, check=True).stdout
        binaries, builds = {}, []
        for raw in (False, True):
            mode = 'raw' if raw else 'legacy'
            binary = out / (mode + '-fixture')
            command = [compiler, '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                       *(['-DNCE_STOCK_INIT_RAMDISK'] if raw else []),
                       '-DSTOCK_INIT_TRACE_HEADER=' + json.dumps(str(header)), str(fixture), '-o', str(binary)]
            with (out / (mode + '-build.log')).open('w') as log:
                subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=60, check=True)
            binaries[raw] = binary
            builds.append({'mode': mode, 'command': command, 'binary_sha256': sha(binary)})
        result['builds'] = builds
        result['legacy_blocks'] = check_blocks(binaries[False], False)
        result['raw_blocks'] = check_blocks(binaries[True], True)
        result['dm'] = check_dm(binaries[True])
        result['inputs_unchanged'] = all(sha(Path(path)) == digest for path, digest in inputs.items())
        result['passed'] = result['inputs_unchanged']
    except (OSError, ValueError, RuntimeError, AssertionError, subprocess.SubprocessError) as error:
        result['error'] = str(error)
    (out / 'result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps({'passed': result['passed'], 'result': str(out / 'result.json'),
                      'legacy_cases': len(result.get('legacy_blocks', [])), 'raw_cases': len(result.get('raw_blocks', [])),
                      'dm_cases': len(result.get('dm', []))}, ensure_ascii=False))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
