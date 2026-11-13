#!/usr/bin/env python3
"""直接调用部署头文件和顺序验收函数，检查固定观察器的生命周期边界。"""
import argparse
import ast
from copy import deepcopy
import hashlib
import json
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import tempfile
from types import SimpleNamespace

sys.dont_write_bytecode = True
import stock_init_system_observer as observer

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / 'experiments/native-linux/translation/stock-init-system-observer.h'
BOOT = '01234567-89ab-cdef-0123-456789abcdef'
FILE_LIMIT = 64 << 10
FIXTURE = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

#define BASE 0x100000UL
#define TARGET 0x200000UL
#define FILESYSTEM 0x200100UL
#define REQUIRE(value) do { if (!(value)) { fprintf(stderr, "require_failed:%d\n", __LINE__); _exit(77); } } while (0)
static int copy_user(void *, unsigned long, unsigned long, int);
static long run_syscall(long, const long *);
static int stock_init_trace_string(unsigned char *, unsigned long, unsigned long);
#include STOCK_INIT_OBSERVER_HEADER

static unsigned char scratch[PAGE];
static struct {
    unsigned char data[65536], label[256];
    unsigned long bytes, label_bytes;
    unsigned uid, gid, mode;
    int exists;
} files[STOCK_INIT_SYSTEM_OBSERVER_FILE_COUNT];
static const char *scenario;
static unsigned maps, unmaps, creates, writes, reads, owners, modes, setlabels, getlabels;
static unsigned policy_changes, after_third_exec;
static unsigned long max_write, max_read;
static unsigned active, write_eintr, read_eintr;
static int fault(const char *name) { return !strcmp(scenario, name); }
static void *memory(unsigned long address, unsigned long bytes)
{
    assert(active && address >= BASE && bytes <= PAGE && address - BASE <= PAGE - bytes);
    return scratch + address - BASE;
}
static int copy_user(void *value, unsigned long address, unsigned long bytes, int write)
{
    if (fault("copy_error")) return -1;
    if (fault("label_copy_error") && getlabels && !write) return -14;
    assert(write == 0 || write == 1);
    if (write) memcpy(memory(address, bytes), value, bytes);
    else memcpy(value, memory(address, bytes), bytes);
    return 0;
}
static int stock_init_trace_string(unsigned char *value, unsigned long address, unsigned long bytes)
{
    const char *text = address == TARGET ? (fault("other_mount") ? "/sys" : "/proc") :
                       address == FILESYSTEM ? (fault("other_filesystem") ? "ext4" : "proc") : NULL;
    assert(text && strlen(text) + 1 <= bytes);
    memcpy(value, text, strlen(text) + 1);
    return fault("trace_error") ? -1 : 0;
}
static unsigned index_for_fd(long fd)
{
    assert(fd >= 10 && fd < 10 + STOCK_INIT_SYSTEM_OBSERVER_FILE_COUNT);
    return (unsigned)fd - 10;
}
static void field(unsigned char *buffer, unsigned offset, const void *value, unsigned bytes)
{
    memcpy(buffer + offset, value, bytes);
}
static long run_syscall(long number, const long *args)
{
    if (number == 172) return fault("non_pid_one") ? 2 : 1;
    if (number == 222) {
        assert(!active && args[0] == 0 && args[1] == PAGE && args[2] == 3 && args[3] == 0x22 && args[4] == -1 && args[5] == 0);
        if (fault("mapping_error")) return -12;
        memset(scratch, 0, sizeof(scratch)); active = 1; ++maps;
        return BASE;
    }
    if (number == 215) {
        assert(active && args[0] == BASE && args[1] == PAGE);
        if (fault("unmap_error")) return -22;
        active = 0; ++unmaps; return 0;
    }
    if (number == 56) {
        const char *path = memory(args[1], 256);
        assert(args[0] == -100);
        if (!strcmp(path, "/proc/sys/kernel/random/boot_id")) { assert(args[2] == 0x80000); return 9; }
        unsigned i;
        for (i = 0; i < STOCK_INIT_SYSTEM_OBSERVER_FILE_COUNT; ++i) if (!strcmp(path, stock_init_system_observer_files[i].path)) break;
        assert(i < STOCK_INIT_SYSTEM_OBSERVER_FILE_COUNT && args[3] == 0644);
        if (args[2] == 0xa00c2) {
            assert(!files[i].exists);
            if (fault("open_error")) return -17;
            files[i].exists = 1; files[i].mode = 0644; ++creates;
        } else assert(args[2] == 0xa0000 && files[i].exists);
        return 10 + i;
    }
    if (number == 63) {
        assert(args[0] == 9 && args[1] == BASE + 256 && args[2] == 64);
        const char *boot = fault("changed_boot") && maps > 1 ? "11234567-89ab-cdef-0123-456789abcdef\n" :
                           fault("malformed_boot") ? "g1234567-89ab-cdef-0123-456789abcdef\n" :
                           "01234567-89ab-cdef-0123-456789abcdef\n";
        memcpy(memory(args[1], 37), boot, 37);
        return fault("short_boot") ? 36 : 37;
    }
    if (number == 57) { assert(args[0] == 9 || (args[0] >= 10 && args[0] < 10 + STOCK_INIT_SYSTEM_OBSERVER_FILE_COUNT)); return fault("close_error") ? -5 : 0; }
    unsigned i = index_for_fd(args[0]);
    if (number == 55) {
        assert(maps == 1 && args[1] == 0 && args[2] == 0); ++owners;
        files[i].uid = args[1]; files[i].gid = args[2]; return fault("owner_error") ? -1 : 0;
    }
    if (number == 52) { assert(maps == 1 && args[1] == 0644); ++modes; files[i].mode = args[1]; return fault("mode_error") ? -1 : 0; }
    if (number == 64) {
        assert(maps == 1 && args[1] == BASE + 1024 && args[2] > 0 && args[2] <= PAGE - 1024);
        unsigned long bytes = args[2];
        if (bytes > max_write) max_write = bytes;
        ++writes;
        if (fault("short_io") && !write_eintr++) return -4;
        if (fault("zero_write")) return 0;
        if (fault("oversized_write")) return bytes + 1;
        if (fault("write_error")) return -5;
        if (fault("short_io") && bytes > 73) bytes = 73;
        assert(files[i].bytes + bytes <= stock_init_system_observer_files[i].bytes);
        assert(!memcmp(memory(args[1], bytes), stock_init_system_observer_files[i].data + files[i].bytes, bytes));
        memcpy(files[i].data + files[i].bytes, memory(args[1], bytes), bytes);
        files[i].bytes += bytes; return bytes;
    }
    if (number == 7) {
        ++setlabels;
        return -13;
    }
    if (number == 291) {
        assert(args[1] == BASE + 256 && *(char *)memory(args[1], 1) == 0);
        assert(args[2] == 0x1000 && args[3] == 0x21f && args[4] == BASE + 1024);
        if (fault("stat_error")) return -5;
        unsigned char status[256] = {0};
        uint32_t mask = fault("stat_mask") ? 0x1f : 0x21f, links = fault("stat_links") ? 2 : 1;
        uint32_t uid = fault("stat_uid") ? 2000 : files[i].uid, gid = fault("stat_gid") ? 2000 : files[i].gid;
        uint16_t mode = 0100000 | (fault("stat_mode") ? 0600 : files[i].mode);
        uint64_t bytes = files[i].bytes + (fault("stat_bytes") ? 1 : 0);
        field(status, 0, &mask, 4); field(status, 16, &links, 4); field(status, 20, &uid, 4);
        field(status, 24, &gid, 4); field(status, 28, &mode, 2); field(status, 40, &bytes, 8);
        memcpy(memory(args[4], 256), status, 256); return 0;
    }
    if (number == 67) {
        assert(args[1] == BASE + 1024 && args[2] > 0 && args[2] <= PAGE - 1024 && args[3] >= 0);
        unsigned long bytes = args[2], offset = args[3]; ++reads;
        if (bytes > max_read) max_read = bytes;
        if (fault("short_io") && !read_eintr++) return -4;
        if (fault("zero_read")) return 0;
        if (fault("oversized_read")) return bytes + 1;
        if (fault("read_error")) return -5;
        if (fault("short_io") && bytes > 97) bytes = 97;
        assert(offset + bytes <= files[i].bytes);
        memcpy(memory(args[1], bytes), files[i].data + offset, bytes);
        if (fault("changed_content")) *(unsigned char *)memory(args[1], 1) ^= 1;
        return bytes;
    }
    if (number == 10) {
        assert(after_third_exec && !strcmp(memory(args[1], 17), "security.selinux") && args[2] == BASE + 1024 && args[3] == PAGE - 1024);
        ++getlabels;
        if (fault("getlabel_error")) return -61;
        if (fault("label_zero")) return 0;
        if (fault("label_oversized")) return PAGE - 1024 + 1;
        if (fault("label_big")) { memset(memory(args[2], 300), 0x5a, 300); return 300; }
        unsigned long bytes = files[i].label_bytes - (fault("label_without_nul") ? 1 : 0);
        if (fault("label_extra_nul")) ++bytes;
        memcpy(memory(args[2], bytes), files[i].label, bytes);
        if (fault("changed_label")) *(unsigned char *)memory(args[2], 1) ^= 1;
        return bytes;
    }
    assert(0); return -1;
}
int main(int argc, char **argv)
{
    assert(argc == 2 && sizeof(long) == 8 && STOCK_INIT_SYSTEM_OBSERVER_FILE_COUNT == 2); scenario = argv[1];
    struct rlimit log = {65536, 65536}, core = {0, 0};
    assert(!setrlimit(RLIMIT_FSIZE, &log) && !setrlimit(RLIMIT_CORE, &core));
    assert(!setvbuf(stdout, NULL, _IONBF, 0));
    assert(!setenv("NCE_STOCK_INIT_FIRST_STAGE", fault("not_first_stage") ? "0" : "1", 1));
    assert(!setenv("NCE_STOCK_INIT_MODE", fault("lifecycle") ? "lifecycle" : fault("recovery") ? "recovery" : "original", 1));
    long args[6] = {0, TARGET, FILESYSTEM, 0, 0, 0};
    puts("{\"stock_init_block\":40,\"pid\":1,\"result\":0,\"paths_hex\":[\"70726f63\",\"2f70726f63\",\"70726f63\"]}");
    stock_init_system_observer_after_syscall(40, args, 0, 0, 0);
    assert(maps == 0 && creates == 0);
    puts("{\"stock_init_exec_layout\":true,\"pid\":1,\"exec_index\":1}");
    stock_init_system_observer_after_syscall(221, args, 0, 1, 1);
    assert(maps == 0 && creates == 0);
    puts("{\"stock_init_block\":40,\"pid\":1,\"result\":0,\"paths_hex\":[\"70726f63\",\"2f70726f63\",\"70726f63\"]}");
    stock_init_system_observer_after_syscall(40, args, fault("failed_mount") ? -5 : 0, 0, fault("before_init_exec") ? 0 : 1);
    if (fault("non_pid_one") || fault("lifecycle") || fault("recovery") || fault("not_first_stage") ||
        fault("other_mount") || fault("other_filesystem") || fault("trace_error") || fault("failed_mount") || fault("before_init_exec")) {
        assert(maps == 0 && creates == 0 && !active);
    } else {
        assert(maps == 1 && unmaps == 1 && creates == 2 && getlabels == 0 && !active);
        stock_init_system_observer_after_syscall(40, args, 0, 0, 1);
        assert(maps == 1 && creates == 2);
        unsigned long last_exec = fault("later_exec") ? 4 : 3;
        for (unsigned long index = 2; index <= last_exec; ++index) {
            if (index == 3) {
                assert(maps == 2 && unmaps == 2 && setlabels == 0 && getlabels == 0);
                const char *label = fault("system_file_label") ? "u:object_r:system_file:s0" : "u:object_r:tmpfs:s0";
                for (unsigned i = 0; i < STOCK_INIT_SYSTEM_OBSERVER_FILE_COUNT; ++i) {
                    assert(files[i].exists && files[i].label_bytes == 0);
                    memcpy(files[i].label, label, strlen(label) + 1);
                    files[i].label_bytes = strlen(label) + 1; ++policy_changes;
                }
                after_third_exec = 1;
                puts("{\"stock_init_security\":221,\"result\":0,\"exec_index\":3,\"path_hex\":\"2f73797374656d2f62696e2f696e6974\"}");
            }
            printf("{\"stock_init_exec_layout\":true,\"pid\":1,\"exec_index\":%lu}\n", index);
            stock_init_system_observer_after_syscall(221, args, 0, index, index);
        }
        assert(maps == last_exec && unmaps == last_exec && creates == 2 && owners == 2 && modes == 2 && setlabels == 0 && getlabels == 2 * (last_exec - 2) && !active);
        assert(policy_changes == 2);
        for (unsigned i = 0; i < STOCK_INIT_SYSTEM_OBSERVER_FILE_COUNT; ++i) {
            assert(files[i].bytes == stock_init_system_observer_files[i].bytes && files[i].label_bytes == 20);
            assert(!memcmp(files[i].label, stock_init_system_observer_files[i].selinux, 20));
        }
        if (!fault("short_io")) assert(writes == (PAGE == 4096 ? 4u : 2u) && reads == last_exec * writes);
        else assert(write_eintr > 1 && read_eintr > 1 && writes > 50 && reads > 50);
    }
    fprintf(stderr, "{\"fixture\":true,\"page\":%u,\"maps\":%u,\"unmaps\":%u,\"creates\":%u,\"writes\":%u,\"reads\":%u,\"max_write\":%lu,\"max_read\":%lu,\"setlabels\":%u,\"getlabels\":%u,\"policy_changes\":%u}\n",
            (unsigned)PAGE, maps, unmaps, creates, writes, reads, max_write, max_read, setlabels,
            getlabels, policy_changes);
    return 0;
}
'''


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def label_diagnostic_valid(page, scenario, rows, manifest):
    diagnostics = [row for row in rows if 'stock_init_system_observer_label_error' in row]
    cases = ('getlabel_error', 'changed_label', 'label_without_nul', 'label_zero', 'label_big',
             'label_oversized', 'label_copy_error', 'label_extra_nul', 'system_file_label')
    if scenario not in cases:
        return not diagnostics
    label = manifest['payloads'][0]['selinux'].encode() + b'\0'
    count, copy_bytes, copy_result, actual = len(label), len(label), 0, label
    if scenario == 'getlabel_error':
        count, copy_bytes, copy_result, actual = -61, 0, -1, b''
    elif scenario == 'label_zero':
        count, copy_bytes, copy_result, actual = 0, 0, -1, b''
    elif scenario == 'label_oversized':
        count, copy_bytes, copy_result, actual = page - 1024 + 1, 0, -1, b''
    elif scenario == 'label_big':
        count, copy_bytes, actual = 300, 256, b'Z' * 256
    elif scenario == 'label_without_nul':
        count, copy_bytes, actual = len(label) - 1, len(label) - 1, label[:-1]
    elif scenario == 'label_extra_nul':
        count, copy_bytes, actual = len(label) + 1, len(label) + 1, label + b'\0'
    elif scenario == 'system_file_label':
        actual = b'u:object_r:system_file:s0\0'
        count = copy_bytes = len(actual)
    elif scenario == 'label_copy_error':
        copy_result, actual = -14, b''
    else:
        actual = bytes([label[0] ^ 1]) + label[1:]
    expected = {'stock_init_system_observer_label_error': True,
                'phase': 'exec', 'exec_index': 3, 'pid': 1,
                'path': manifest['payloads'][0]['path'], 'label_bytes': count,
                'expected_label_bytes': len(label), 'label_copy_bytes': copy_bytes,
                'label_copy_result': copy_result, 'label_hex': actual.hex()}
    return diagnostics == [expected] and len(diagnostics[0]['label_hex']) <= 512


def sequence_checks(rows, manifest):
    results = []

    def check(name, candidate, allowed=False, *, required=True):
        try:
            value = observer.observe_deployment(candidate, manifest, required=required)
        except (ValueError, TypeError, KeyError, IndexError) as error:
            results.append({'name': name, 'passed': not allowed, 'accepted': False, 'error_type': type(error).__name__})
        else:
            results.append({'name': name, 'passed': allowed, 'accepted': True, 'result': value})

    check('actual_deployment_and_two_execs', rows, True)
    check('unselected_no_deployment', [], True, required=False)
    check('unselected_with_deployment', rows, required=False)
    files = [i for i, row in enumerate(rows) if row.get('stock_init_system_observer_file') is True]
    ready = [i for i, row in enumerate(rows) if row.get('stock_init_system_observer_ready') is True]
    raw = next(i for i, row in enumerate(rows) if row.get('stock_init_exec_layout') is True and row['exec_index'] == 1)
    proc = next(i for i, row in enumerate(rows) if i > raw and row.get('stock_init_block') == 40)
    execs = [i for i, row in enumerate(rows) if row.get('stock_init_exec_layout') is True and row['exec_index'] > 1]
    boundary = next(i for i, row in enumerate(rows) if row.get('stock_init_security') == 221)

    def exec2_after_exec3(candidate):
        group = candidate[files[2]:ready[1] + 1]
        del candidate[files[2]:ready[1] + 1]
        position = next(i for i, row in enumerate(candidate)
                        if row.get('stock_init_exec_layout') is True and row['exec_index'] == 3)
        candidate[position + 1:position + 1] = group

    mutations = [
        ('missing_file', lambda r: r.pop(files[0])),
        ('missing_ready', lambda r: r.pop(ready[0])),
        ('missing_second_exec', lambda r: r.pop(execs[1])),
        ('label_modified', lambda r: r[files[4]].__setitem__('label_hex', '00')),
        ('label_nul_missing', lambda r: r[files[4]].__setitem__('label_hex', r[files[4]]['label_hex'][:-2])),
        ('label_before_policy', lambda r: r[files[0]].__setitem__('label_hex', r[files[4]]['label_hex'])),
        ('label_post_policy_missing', lambda r: r[files[4]].__setitem__('label_hex', '')),
        ('boot_id_modified', lambda r: r[ready[1]].__setitem__('boot_id', '11234567-89ab-cdef-0123-456789abcdef')),
        ('content_false', lambda r: r[files[0]].__setitem__('content_matches', False)),
        ('ready_before_files', lambda r: r.insert(files[0], r.pop(ready[0]))),
        ('exec_readback_before_trigger', lambda r: r.insert(execs[0], r.pop(files[2]))),
        ('exec2_group_after_exec3', exec2_after_exec3),
        ('files_reordered', lambda r: r.__setitem__(slice(files[0], files[0] + 2), [r[files[1]], r[files[0]]])),
        ('duplicate_file', lambda r: r.insert(files[0], deepcopy(r[files[0]]))),
        ('proc_failed', lambda r: r[proc].__setitem__('result', -1)),
        ('proc_other_pid', lambda r: r[proc].__setitem__('pid', 2)),
        ('proc_pid_bool', lambda r: r[proc].__setitem__('pid', True)),
        ('proc_result_bool', lambda r: r[proc].__setitem__('result', False)),
        ('proc_number_float', lambda r: r[proc].__setitem__('stock_init_block', 40.0)),
        ('raw_missing', lambda r: r.pop(raw)),
        ('raw_duplicate', lambda r: r.insert(raw, deepcopy(r[raw]))),
        ('raw_pid_bool', lambda r: r[raw].__setitem__('pid', True)),
        ('raw_index_bool', lambda r: r[raw].__setitem__('exec_index', True)),
        ('only_auxiliary_proc_before_raw', lambda r: r.pop(proc)),
        ('exec_pid_bool', lambda r: r[execs[0]].__setitem__('pid', True)),
        ('exec_index_bool', lambda r: r[execs[0]].__setitem__('exec_index', True)),
        ('old_policy_phase', lambda r: r[files[4]].__setitem__('phase', 'policy')),
        ('exec3_boundary_missing', lambda r: r.pop(boundary)),
        ('exec3_boundary_wrong_path', lambda r: r[boundary].__setitem__('path_hex', '/init'.encode().hex())),
        ('exec3_boundary_failed', lambda r: r[boundary].__setitem__('result', -13)),
        ('exec3_boundary_result_bool', lambda r: r[boundary].__setitem__('result', False)),
        ('exec3_boundary_after_layout', lambda r: r.insert(execs[1], r.pop(boundary))),
    ]
    for name, mutation in mutations:
        candidate = deepcopy(rows)
        mutation(candidate)
        check(name, candidate)
    for kind, positions, fields in (
        ('file', files[:1], ('stock_init_system_observer_file', 'exec_index', 'pid', 'uid', 'gid', 'nlink', 'content_matches')),
        ('ready', ready[:1], ('stock_init_system_observer_ready', 'exec_index', 'pid', 'files', 'scratch_unmapped')),
    ):
        for position in positions:
            for field in fields:
                candidate = deepcopy(rows)
                value = candidate[position][field]
                candidate[position][field] = int(value) if type(value) is bool else bool(value)
                check(kind + '_' + field + '_boolean_number', candidate)
    for name, values in (('duplicate_exec_index', {2: 2, 3: 2}),
                         ('decreasing_exec_index', {2: 3, 3: 2}),
                         ('zero_exec_index', {2: 0, 3: 3})):
        candidate = deepcopy(rows)
        for row in candidate:
            if row.get('exec_index') in values:
                row['exec_index'] = values[row['exec_index']]
        check(name, candidate)
    return results


def probe_hook_checks():
    source = (ROOT / 'scripts/probe_stock_init.py').read_text()
    assignments = [node for node in ast.walk(ast.parse(source)) if isinstance(node, ast.Assign) and
                   any(isinstance(target, ast.Name) and target.id == 'observed' for target in node.targets)]
    assert len(assignments) == 1
    expression = compile(ast.Expression(assignments[0].value), '<actual probe observed expression>', 'eval')
    checks = []
    for enabled in (False, True):
        text = eval(expression, {'args': SimpleNamespace(sdk_ramdisk=True, sdk_system_observer=enabled),
                                 'before': '            long result = run_syscall(number, args);'})
        if enabled:
            positions = [text.index(value) for value in ('stock_init_trace_string(exec_path',
                         'long result = run_syscall(number, args);', 'exec_index = ++stock_init_exec_count;',
                         'stock_init_trace_security(', 'stock_init_trace_exec(', 'stock_init_system_observer_after_syscall(')]
            passed = (positions == sorted(positions) and text.count('stock_init_system_observer_after_syscall(') == 1 and
                      'stock_init_system_observer_before_syscall' not in text)
        else:
            passed = 'stock_init_system_observer_' not in text
        checks.append({'name': 'probe_after_hook_' + str(enabled), 'passed': passed})
    return checks


def tokenizer_checks(core, build_dir, output, manifest):
    """用原 tokenizer 和 ExpandProps 函数源码检查实际生成的命令参数。"""
    util = (core / 'init/util.cpp').read_text()
    start = util.index('Result<std::string> ExpandProps(')
    expand = util[start:util.index('\n}\n', start) + 3]
    fixture = r'''
#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>
#include <sys/resource.h>
#include "tokenizer.h"
#include "stock-init-system-observer-data.h"
using namespace android::init;
struct ErrorValue {
    template<class T> ErrorValue& operator<<(const T&) { return *this; }
};
static ErrorValue Error() { return {}; }
template<class T> class Result {
    bool good_;
    T value_;
public:
    Result(T value) : good_(true), value_(std::move(value)) {}
    Result(const ErrorValue&) : good_(false) {}
    bool ok() const { return good_; }
    const T& operator*() const { return value_; }
};
namespace android { namespace base {
static std::string GetProperty(const std::string&, const char*) {
    assert(false); return {};
}
}}
static int SelinuxGetVendorAndroidVersion() { return 35; }
#define __ANDROID_API_R__ 30
#define LOG(level) std::cerr
''' + expand + r'''
int main(int argc, char** argv) {
    struct rlimit log = {65536, 65536}, core = {0, 0};
    assert(!setrlimit(RLIMIT_FSIZE, &log) && !setrlimit(RLIMIT_CORE, &core));
    assert(STOCK_INIT_SYSTEM_OBSERVER_FILE_COUNT == 2);
    std::string data;
    if (argc == 1) {
        const auto& file = stock_init_system_observer_files[1];
        data.assign(reinterpret_cast<const char*>(file.data), file.bytes);
    } else if (argc == 2 && std::string(argv[1]) == "wrapper") {
        const auto& file = stock_init_system_observer_files[0];
        std::cout.write(reinterpret_cast<const char*>(file.data), file.bytes);
        return 0;
    } else {
        assert(argc == 2);
        std::ifstream input(argv[1], std::ios::binary);
        assert(input.good());
        data.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    assert(data.size() <= 65536);
    data.push_back('\n'); data.push_back('\0');
    parse_state state = {data.data(), nullptr, 0, 0};
    std::vector<std::string> args;
    for (;;) {
        int token = next_token(&state);
        if (token == T_TEXT) args.emplace_back(state.text);
        else if (token == T_NEWLINE || token == T_EOF) break;
        else assert(false);
    }
    if (args.size() != 5 || args[0] != "service" || args[2] != "/system/bin/sh" || args[3] != "-c") return 78;
    auto expanded = ExpandProps(args[4]);
    if (!expanded.ok()) return 79;
    std::cout.write((*expanded).data(), (*expanded).size());
    return 0;
}
'''
    source = build_dir / 'tokenizer-fixture.cpp'
    source.write_text(fixture)
    macros = build_dir / 'android-base/macros.h'
    macros.parent.mkdir()
    macros.write_text('#pragma once\n#define FALLTHROUGH_INTENDED [[fallthrough]]\n')
    binary = build_dir / 'tokenizer-fixture'
    compiler = shutil.which('clang++')
    if compiler is None:
        raise RuntimeError('缺少 clang++')
    command = [compiler, '-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror',
               '-I', str(build_dir), '-I', str(core / 'init'),
               str(core / 'init/tokenizer.cpp'), str(source), '-o', str(binary)]
    with (output / 'build-tokenizer.log').open('w') as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=30)
    results = []
    script = Path(manifest['script_source']).read_bytes()
    process = subprocess.run([str(binary)], capture_output=True, timeout=5)
    results.append({'name': 'original_tokenizer_generated_script', 'passed': process.returncode == 0 and process.stdout == script,
                    'script_bytes': len(script), 'expanded_sha256': hashlib.sha256(process.stdout).hexdigest()})
    process = subprocess.run([str(binary), 'wrapper'], capture_output=True, timeout=5)
    init = (core / 'init/init.cpp').read_text()
    load = init[init.index('static void LoadBootScripts('):init.index('\nvoid PropertyChanged(')]
    imports = re.findall(r'parser\.ParseConfig\("([^"]+)"\)', load) + [observer.RC_PATH]
    expected = ''.join('import ' + path + '\n' for path in imports).encode()
    results.append({'name': 'original_default_import_order', 'passed': process.returncode == 0 and process.stdout == expected})
    for name, body in (
        ('literal_dollars_quotes_comments', b'#x\nprintf \'%s\\n\' "$v" "$$"\n'),
        ('tab_and_expansion_forms', b'#x\t$' + b'{value} $(cat /dev/null) $((1 + 2)) "$1" "$?"\n'),
    ):
        rc = build_dir / (name + '.rc')
        rc.write_text('service t /system/bin/sh -c ' + observer._shell_token(body, rc) + '\n')
        process = subprocess.run([str(binary), str(rc)], capture_output=True, timeout=5)
        results.append({'name': 'original_tokenizer_' + name, 'passed': process.returncode == 0 and process.stdout == body})
    rc = build_dir / 'raw-dollar.rc'
    rc.write_text('service t /system/bin/sh -c ' + observer._shell_token(b'echo "$v"\n', rc).replace('$$v', '$v') + '\n')
    process = subprocess.run([str(binary), str(rc)], capture_output=True, timeout=5)
    results.append({'name': 'original_expandprops_raw_dollar_rejected', 'passed': process.returncode == 79})
    for name, body in (('empty', b''), ('nul', b'a\0b'), ('cr', b'a\rb')):
        try:
            observer._shell_token(body, build_dir / name)
        except ValueError:
            passed = True
        else:
            passed = False
        results.append({'name': 'shell_token_' + name + '_rejected', 'passed': passed})
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    common = Path(subprocess.check_output(['git', 'rev-parse', '--git-common-dir'], cwd=ROOT, text=True, timeout=10).strip())
    common = common if common.is_absolute() else ROOT / common
    main_root = common.resolve().parent
    output = args.output.resolve()
    if not output.is_relative_to(main_root / 'work/sdk-plain-validation-v1'):
        parser.error('输出需要位于 MAIN/work/sdk-plain-validation-v1')
    output.mkdir(parents=True, exist_ok=False)
    core = main_root / 'work/stock-init-keymint/work/keymint-investigation/system-core'
    ramdisk = main_root / 'work/native-linux-nce/android-reference/sdk/system-images/android-36/google_apis_ps16k/arm64-v8a/ramdisk.img'
    sources = (Path(__file__).resolve(), HEADER, Path(observer.__file__).resolve(),
               observer.ASSETS / 'stock-init-system-observer.sh', observer.ASSETS / 'stock-init-system-observer.rc',
               ramdisk, *(ROOT / 'scripts' / name for name in
                          ('stock_init_first_stage.py', 'stock_init_sdk_ramdisk.py', 'stock_init_plain_data.py')),
               ROOT / 'scripts/probe_stock_init.py',
               *(core / 'init' / name for name in ('tokenizer.cpp', 'tokenizer.h', 'util.cpp', 'init.cpp')))
    source_hashes = {str(path): sha(path) for path in sources}
    result = {'test': 'stock_init_system_observer_deployment', 'passed': False, 'host': platform.platform(),
              'source_sha256': source_hashes, 'fixtures': [], 'checks': [],
              'scope': '宿主内存中的固定 syscall/copy_user fixture；直接调用真实部署头文件与顺序验收函数。'}
    skipped = ('non_pid_one', 'lifecycle', 'recovery', 'not_first_stage', 'other_mount', 'other_filesystem',
               'trace_error', 'failed_mount', 'before_init_exec')
    failures = ('mapping_error', 'copy_error', 'open_error', 'owner_error', 'mode_error', 'zero_write',
                'oversized_write', 'write_error', 'zero_read', 'oversized_read', 'read_error', 'stat_error',
                'stat_mask', 'stat_links', 'stat_uid', 'stat_gid', 'stat_mode', 'stat_bytes', 'changed_content',
                'getlabel_error', 'changed_label', 'label_without_nul', 'changed_boot',
                'malformed_boot', 'short_boot', 'unmap_error', 'close_error', 'label_zero', 'label_big',
                'label_oversized', 'label_copy_error', 'label_extra_nul', 'system_file_label')
    try:
        compiler = shutil.which('clang')
        if not compiler:
            raise RuntimeError('缺少 clang')
        result['compiler_version'] = subprocess.check_output([compiler, '--version'], text=True, timeout=10).splitlines()[0]
        with tempfile.TemporaryDirectory(prefix='fixture-', dir=output) as temporary:
            build_dir = Path(temporary)
            manifest = observer.prepare(build_dir)
            result['generated_header_sha256'] = manifest['header_sha256']
            result['checks'] = tokenizer_checks(core, build_dir, output, manifest)
            from stock_init_first_stage import prepare as prepare_first_stage
            from stock_init_plain_data import transform_fstab
            from stock_init_sdk_ramdisk import read as read_ramdisk
            parsed = read_ramdisk(ramdisk)
            fstab = next(row['data'] for row in parsed['entries'] if row['path'] == 'first_stage_ramdisk/fstab.ranchu')
            mounted = {'system_observer': manifest, 'exec_elf_images': {},
                       'sdk_plain_data': {'fstab_candidate_sha256': hashlib.sha256(transform_fstab(fstab)).hexdigest()}}
            first_stage_dir = build_dir / 'first-stage'
            first_stage_dir.mkdir()
            prepared = prepare_first_stage(ramdisk, first_stage_dir, mounted, plain_data=True, early_overcommit=True)
            result['checks'].append({'name': 'actual_observer_first_stage_preparation',
                                     'passed': prepared['sdk_ramdisk']['cmdline'] ==
                                               'androidboot.hardware=ranchu sysctl.vm.overcommit_memory=1 ' + observer.CMDLINE_OPTION})
            old_contract = deepcopy(mounted)
            old_contract['system_observer']['contract'] = observer.CONTRACT - 1
            try:
                prepare_first_stage(ramdisk, first_stage_dir, old_contract, plain_data=True)
            except ValueError:
                result['checks'].append({'name': 'first_stage_previous_contract_rejected', 'passed': True})
            else:
                result['checks'].append({'name': 'first_stage_previous_contract_rejected', 'passed': False})
            source = build_dir / 'deployment-fixture.c'
            source.write_text(FIXTURE)
            for page in (4096, 16384):
                binary = build_dir / ('fixture-' + str(page))
                command = [compiler, '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-DPAGE=' + str(page),
                           '-DSTOCK_INIT_OBSERVER_HEADER=' + json.dumps(str(HEADER)), '-I', str(build_dir),
                           str(source), '-o', str(binary)]
                with (output / ('build-' + str(page) + '.log')).open('w') as log:
                    subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=30)
                for scenario in ('normal', 'short_io', 'later_exec', *skipped, *failures):
                    stdout_path = output / f'{page}-{scenario}.jsonl'
                    stderr_path = output / f'{page}-{scenario}.stderr'
                    with stdout_path.open('w') as stdout, stderr_path.open('w') as stderr:
                        process = subprocess.run([str(binary), scenario], stdout=stdout, stderr=stderr, timeout=5)
                    expected = 77 if scenario in failures else 0
                    bounded = stdout_path.stat().st_size <= FILE_LIMIT and stderr_path.stat().st_size <= FILE_LIMIT
                    case = {'page': page, 'scenario': scenario, 'expected_returncode': expected,
                            'returncode': process.returncode, 'passed': process.returncode == expected and bounded,
                            'stdout_bytes': stdout_path.stat().st_size, 'stderr_bytes': stderr_path.stat().st_size}
                    result['fixtures'].append(case)
                    rows = [json.loads(line) for line in stdout_path.read_text().splitlines()]
                    case['label_diagnostic_valid'] = label_diagnostic_valid(page, scenario, rows, manifest)
                    case['passed'] = case['passed'] and case['label_diagnostic_valid']
                    if process.returncode == 0 and scenario in ('normal', 'short_io', 'later_exec'):
                        validated = observer.observe_deployment(rows, manifest, required=True)
                        last_exec = 4 if scenario == 'later_exec' else 3
                        assert validated['boot_id'] == BOOT and validated['verified_exec_indices'] == list(range(2, last_exec + 1))
                        counters = json.loads(stderr_path.read_text())
                        assert counters['maps'] == counters['unmaps'] == last_exec and counters['creates'] == 2
                        assert counters['setlabels'] == 0 and counters['getlabels'] == 2 * (last_exec - 2)
                        assert counters['policy_changes'] == 2
                        assert counters['max_read'] <= page - 1024 and counters['max_write'] <= page - 1024
                        if page == 4096 and scenario == 'normal':
                            result['checks'].extend(sequence_checks(rows, manifest) + probe_hook_checks())
                    elif process.returncode == 0 and scenario in skipped:
                        assert observer.observe_deployment(rows, manifest, required=False) == {}
                    elif process.returncode == 77:
                        try:
                            observer.observe_deployment(rows, manifest, required=True)
                        except (ValueError, TypeError, KeyError, IndexError):
                            pass
                        else:
                            case['passed'] = False
            if any(sha(Path(path)) != digest for path, digest in source_hashes.items()):
                raise ValueError('测试期间生产源码发生变化')
            result['passed'] = bool(result['checks']) and all(case['passed'] for case in result['fixtures'] + result['checks'])
    except Exception as error:
        result['error'] = {'type': type(error).__name__, 'message': str(error)}
    finally:
        (output / 'result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps({'passed': result['passed'], 'fixtures': len(result['fixtures']), 'sequence_checks': len(result['checks']),
                      'result': str(output / 'result.json')}))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
