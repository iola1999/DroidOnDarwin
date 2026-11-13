#!/usr/bin/env python3
"""在已验证的内核上注入 BPF 发布失败，检查解释执行、charge 和执行池归还。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess

from process_observer import OBSERVATION_MAX_BYTES, observe

ROOT = Path(__file__).resolve().parents[1]
GUEST = ROOT / 'experiments/native-linux/userspace/bpf-jit-failure-test.c'
HOST_FIXTURE = r'''
static unsigned diagnostic_period, diagnostic_publications, diagnostic_failures;
static unsigned diagnostic_allocations, diagnostic_frees;
static long *diagnostic_charge;

static void *diagnostic_alloc(unsigned long bytes)
{
    void *result = translation_bpf_code_alloc(bytes);
    diagnostic_allocations += result != NULL;
    return result;
}

static int diagnostic_free(void *address)
{
    int result = translation_bpf_code_free(address);
    diagnostic_frees += result == 0;
    return result;
}

static int diagnostic_publish(const void *address, unsigned long bytes)
{
    void *alias = translation_bpf_code_write_alias(address, bytes);
    /* 最终发布包含 header 的长度；失效区域填充 ARM64 BRK 指令。 */
    if (alias && bytes >= sizeof(unsigned) && *(unsigned *)alias == bytes) {
        diagnostic_publications++;
        if (diagnostic_period && diagnostic_publications % diagnostic_period == 0) {
            diagnostic_failures++;
            printf("{\"test\":\"bpf_failure_injected\",\"publication\":%u,\"bytes\":%lu}\n",
                   diagnostic_publications, bytes);
            return -1;
        }
    }
    return translation_bpf_code_publish(address, bytes);
}
'''


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--control', action='store_true', help='旧首次失败清空入口的对照，预期实际 SIGSEGV')
    args = parser.parse_args()
    baseline, out = args.baseline.resolve(), args.output.resolve()
    report_path = baseline / 'summary.json'
    report = json.loads(report_path.read_text())
    config = report['configuration']
    assert all(r['returncode'] == 0 and r['validated'] for r in report['runs'])
    assert config['bpf_jit'] and config['execution'] == 'direct' and config['cpus'] == 1
    assert config['workload'] == 'core' and config['mapping'] == 'pte' and config['code_memory'] == 'dual'
    compile_guest = next(c.copy() for c in report['commands'] if '-c' in c and c[c.index('-c') + 1].endswith('/translation/linux-guest.c'))
    producer = Path(compile_guest[compile_guest.index('-c') + 1]).parents[3]
    changed = [name for name, value in report['sha256'].items() if digest(producer / name) != value]
    assert not changed, changed
    kernel = producer / next(name for name in report['sha256'] if name.endswith('/lkl-image.bin'))
    elf = kernel.parent / 'lkl.so'
    nm = '/opt/homebrew/opt/llvm/bin/llvm-nm'
    symbols = subprocess.check_output([nm, '-n', str(elf)], text=True)
    found = [line.split()[0] for line in symbols.splitlines() if line.endswith(' bpf_jit_current')]
    assert len(found) == 1
    charge_offset = int(found[0], 16)
    out.mkdir(parents=True, exist_ok=False)
    state = {'status': 'running', 'scope': 'Host callback publication failure; actual BPF runtime and charge; RW/offset allocation not measured',
             'baseline': str(baseline), 'baseline_sha256': digest(report_path), 'configuration': config,
             'producer_inputs_changed': changed, 'kernel_sha256': digest(kernel), 'charge_offset': charge_offset,
             'commands': [], 'runs': [], 'sha256': {str(p): digest(p) for p in
                                                  (Path(__file__), GUEST, report_path, elf, kernel,
                                                   Path(observe.__code__.co_filename).resolve())}}
    for name, value in report['sha256'].items():
        if name not in changed:
            state['sha256'][str(producer / name)] = value

    def save():
        (out / 'summary.json').write_text(json.dumps(state, ensure_ascii=False, indent=2) + '\n')

    def run(command, log):
        state['commands'].append(command)
        save()
        subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT, timeout=180)

    source = producer / 'experiments/native-linux/translation/linux-runner.c'
    runner = source.read_text()
    assert runner.count('static long (*call)(long, long *);') == 1
    runner = runner.replace('static long (*call)(long, long *);', 'static long (*call)(long, long *);\n' + HOST_FIXTURE)
    runner = runner.replace('    unsigned char *kernel = loaded.base;',
                            f'    unsigned char *kernel = loaded.base;\n    diagnostic_charge = (void *)(kernel + {charge_offset});\n'
                            '    const char *period = getenv("NCE_BPF_FAILURE_PERIOD");\n    REQUIRE(period && (!strcmp(period,"0") || !strcmp(period,"1") || !strcmp(period,"2")));\n'
                            '    diagnostic_period = (unsigned)(period[0] - \'0\');')
    for name in ('alloc', 'free', 'publish'):
        before = f'    nce_lkl_host.bpf_code_{name} = translation_bpf_code_{name};'
        assert runner.count(before) == 1
        runner = runner.replace(before, f'    nce_lkl_host.bpf_code_{name} = diagnostic_{name};')
    before = '    REQUIRE(halt() == 0);'
    assert runner.count(before) == 1
    runner = runner.replace(before, '    printf("{\\"test\\":\\"bpf_failure_resources\\",\\"charge_bytes\\":%ld,\\"allocations\\":%u,\\"frees\\":%u,\\"publications\\":%u,\\"failures\\":%u}\\n",\n'
                            '           *diagnostic_charge, diagnostic_allocations, diagnostic_frees, diagnostic_publications, diagnostic_failures);\n' + before)
    runner_path = out / 'runner.c'
    runner_path.write_text(runner)
    state['sha256'][str(runner_path)] = digest(runner_path)
    environment = {k: v for k, v in os.environ.items() if not k.startswith(('NCE_', 'ANGLE_'))}
    environment.update(NCE_TRANSLATION_GUEST_LAYOUT='sparse', NCE_TRANSLATION_DATA_TBI='1', NCE_TRANSLATION_ONLINE_CPUS='1',
                       NCE_TRANSLATION_BATCH=str(config['instruction_batch']), NCE_TRANSLATION_PARALLEL_PROFILE='0',
                       NCE_TRANSLATION_UACCESS_STOP=config['uaccess_stop'], NCE_TRANSLATION_PREEMPTION=config['preemption'],
                       NCE_TRANSLATION_MEMORY_MS='0', NCE_TRANSLATION_WORKLOAD='core', NCE_TRANSLATION_CACHE=config['cache_invalidation'],
                       NCE_TRANSLATION_EXEC_CACHE=config['exec_cache'], NCE_TRANSLATION_TLB=config['tlb_invalidation'],
                       NCE_TRANSLATION_LINUX_FASTPATH='0', NCE_TRANSLATION_CACHE_MIB=str(config['translation_cache_bytes'] // (1024 * 1024)))
    for name, value in config['direct'].items():
        environment['NCE_DIRECT_' + name.upper()] = str(int(value)) if isinstance(value, bool) else str(value)
    save()
    try:
        with (out / 'build.log').open('w') as log:
            hosts = {}
            for entry in ('cli', 'gcd'):
                command = next(c.copy() for c in report['commands'] if '-o' in c and c[c.index('-o') + 1] == str(baseline / entry))
                command = [str(runner_path) if x == str(source) else x for x in command]
                command[command.index('-o') + 1] = str(out / entry)
                command += ['-I', str(source.parent)]
                run(command, log)
                hosts[entry] = out / entry
                state['sha256'][str(hosts[entry])] = digest(hosts[entry])
            variants = [('single-first', 0, 1)] if args.control else [('single-normal', 0, 0), ('single-first', 0, 1), ('subprogram-normal', 1, 0), ('subprogram-first', 1, 1), ('subprogram-second', 1, 2)]
            for variant, subprogram, period in variants:
                directory = out / variant
                directory.mkdir()
                compile_command = compile_guest.copy()
                compile_command[compile_command.index('-c') + 1] = str(GUEST)
                compile_command[compile_command.index('-o') + 1] = str(directory / 'guest.o')
                compile_command += ['-idirafter', str(producer / 'work/native-linux-nce/processes/linux-uapi/include'),
                                    f'-DSUBPROGRAM={subprogram}', f'-DFAIL_PUBLICATION={int(period > 0)}', '-MD', '-MF', str(directory / 'guest.d')]
                link = next(c.copy() for c in report['commands'] if c[-1] == str(baseline / 'guest.elf'))
                link = [str(directory / 'guest.o') if x == str(baseline / 'guest.o') else x for x in link
                        if not (x.startswith(str(baseline) + '/') and x.endswith('.o') and x != str(baseline / 'guest.o'))]
                link[-1] = str(directory / 'guest.elf')
                run(compile_command, log)
                dependencies = shlex.split((directory / 'guest.d').read_text().replace('\\\n', ' ').partition(':')[2])
                for path in (*dependencies, directory / 'guest.d', *[p for p in link[1:] if Path(p).is_file()]):
                    state['sha256'][str(path)] = digest(path)
                run(link, log)
                state['sha256'][str(directory / 'guest.elf')] = digest(directory / 'guest.elf')
                runtime = ('sigreturn.elf', 'dynamic.elf', 'libc.so', 'dependency.so', 'plugin.so', 'dash.elf', 'bionic-test.elf')
                runtime += tuple(p.name for p in sorted(baseline.glob('exec-aslr-*.elf')))
                for name in runtime:
                    (directory / name).symlink_to(baseline / name)
                (directory / 'late.elf').symlink_to(directory / 'guest.elf')
                env = dict(environment, NCE_TRANSLATION_FILES=str(directory), NCE_BPF_FAILURE_PERIOD=str(period))
                for entry, host in hosts.items():
                    command = [str(host), str(kernel)]
                    state['commands'].append(command)
                    save()
                    stdout_path, stderr_path = directory / f'{entry}.jsonl', directory / f'{entry}.log'
                    with stdout_path.open('w') as stdout, stderr_path.open('w') as stderr:
                        observed = observe(command, env, stdout, stderr, 30)
                    raw = stdout_path.read_bytes()
                    assert len(raw) <= OBSERVATION_MAX_BYTES
                    rows = [json.loads(line) for line in raw.splitlines()]
                    resource = [r for r in rows if r.get('test') == 'bpf_failure_resources']
                    completed = [r for r in rows if r.get('test') == 'bpf_failure_executed']
                    injected = [r for r in rows if r.get('test') == 'bpf_failure_injected']
                    expected_injections = 8 if period else 0
                    valid = (observed['returncode'] == observed['actual_exit'] == (-11 if args.control else 0)
                             and not observed['timed_out'] and not observed['errors'] and not observed['cleanup_signals']
                             and observed['process_group_empty'] and not observed['output_limit_exceeded'])
                    if args.control:
                        valid = valid and len(injected) == 1 and not completed
                    else:
                        valid = (valid and len(resource) == 1 and resource[0]['charge_bytes'] == 0
                                 and resource[0]['allocations'] == resource[0]['frees']
                                 and len(completed) == 8 and len(injected) == expected_injections
                                 and resource[0]['failures'] == expected_injections)
                    state['runs'].append({'variant': variant, 'entry': entry, **observed, 'validated': valid,
                                          'rows': rows, 'environment': {k: v for k, v in env.items() if k.startswith('NCE_')},
                                          'stdout_sha256': digest(stdout_path), 'stderr_sha256': digest(stderr_path)})
                    save()
                    print(json.dumps({'variant': variant, 'entry': entry, **observed, 'validated': valid}), flush=True)
                    assert valid, 'BPF publication failure fixture: ' + variant
        state['inputs_unchanged'] = all(digest(name) == value for name, value in state['sha256'].items())
        assert state['inputs_unchanged']
        state['status'] = 'passed'
    except BaseException as error:
        state.update(status='failed', error=str(error), error_type=type(error).__name__)
        raise
    finally:
        save()


if __name__ == '__main__':
    main()
