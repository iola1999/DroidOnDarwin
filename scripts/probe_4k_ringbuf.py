#!/usr/bin/env python3
"""在已验证的 Direct 基准配置上检查 4 KiB Linux BPF ringbuf 共享语义。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--baseline', type=Path, required=True, help='已通过的 4 KiB Direct core 输出目录')
    parser.add_argument('--kernel', type=Path, required=True, help='待验证的独立 4 KiB 内核目录')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    baseline, kernel, out = [p.resolve() for p in (args.baseline, args.kernel, args.output)]
    report = json.loads((baseline / 'summary.json').read_text())
    config = report['configuration']
    if (config['guest_page_bytes'] != 4096 or config['execution'] != 'direct' or config['cpus'] != 1
            or config['workload'] != 'core' or config['guest_layout'] != 'sparse' or not config['data_tbi']
            or config['code_memory'] != 'dual' or config['direct']['memory'] != 'flat'
            or not all(r['returncode'] == 0 and r['validated'] for r in report['runs'])):
        raise ValueError('需要已通过的单 CPU 4 KiB Direct flat/sparse/TBI/dual core 基准')
    baseline_root = next(Path(c[c.index('-c') + 1]).parents[3] for c in report['commands']
                         if '-c' in c and c[c.index('-c') + 1].endswith('/translation/linux-guest.c'))
    stale = [p for p, h in report['sha256'].items() if digest(baseline_root / p) != h]
    if stale:
        raise ValueError('基准输入发生变化: ' + ', '.join(stale))
    build = json.loads((kernel / 'build.json').read_text())
    if build['guest_page_bytes'] != 4096 or build['cpus'] != 1:
        raise ValueError('内核页大小或 CPU 配置不匹配')
    stale = [p for p, h in build['sha256'].items() if digest(ROOT / p) != h]
    if stale:
        raise ValueError('候选内核输入发生变化: ' + ', '.join(stale))
    out.mkdir(parents=True, exist_ok=False)
    commands = []

    def run(command):
        commands.append(list(map(str, command)))
        subprocess.run(command, check=True, timeout=180)

    # 基准引擎对象只读复用；宿主 runner 使用候选内核的新导出地址重新编译。
    for c in report['commands']:
        if c[:2] != ['xcrun', 'clang']:
            continue
        command = [str(ROOT / Path(x).relative_to(baseline_root)) if x.startswith(str(baseline_root / 'experiments')) else x for x in c]
        command = [str(kernel) if x == str(baseline_root / 'work/native-linux-nce/lkl-translation-kernel-4k-vmalloc') else x for x in command]
        command[command.index('-o') + 1] = str(out / Path(command[command.index('-o') + 1]).name)
        run(command)
    guest_source = ROOT / 'experiments/native-linux/userspace/bpf-ringbuf-test.c'
    compile_command = next(c.copy() for c in report['commands'] if '-c' in c and c[c.index('-c') + 1].endswith('/translation/linux-guest.c'))
    compile_command[compile_command.index('-c') + 1] = str(guest_source)
    compile_command[compile_command.index('-o') + 1] = str(out / 'guest.o')
    compile_command += ['-idirafter', str(baseline_root / 'work/native-linux-nce/processes/linux-uapi/include')]
    run(compile_command)
    link = next(c.copy() for c in report['commands'] if c[-1].endswith('/guest.elf'))
    link = [str(out / 'guest.o') if x == str(baseline / 'guest.o') else x for x in link
            if not (x.startswith(str(baseline)) and x.endswith('.o') and x != str(baseline / 'guest.o'))]
    link[-1] = str(out / 'guest.elf')
    run(link)
    for name in ('sigreturn.elf', 'dynamic.elf', 'libc.so', 'dependency.so', 'plugin.so', 'dash.elf', 'bionic-test.elf'):
        run(['cp', '-c', baseline / name, out / name])
    run(['cp', '-c', out / 'guest.elf', out / 'late.elf'])
    env = {k: v for k, v in os.environ.items() if not k.startswith(('NCE_', 'ANGLE_'))}
    env.update(NCE_TRANSLATION_FILES=str(out), NCE_TRANSLATION_GUEST_LAYOUT='sparse', NCE_TRANSLATION_DATA_TBI='1',
               NCE_TRANSLATION_ONLINE_CPUS='1', NCE_TRANSLATION_BATCH='50000', NCE_TRANSLATION_PARALLEL_PROFILE='0',
               NCE_TRANSLATION_UACCESS_STOP='all', NCE_TRANSLATION_PREEMPTION='instructions', NCE_TRANSLATION_MEMORY_MS='100',
               NCE_TRANSLATION_WORKLOAD='core', NCE_TRANSLATION_CACHE='pages', NCE_TRANSLATION_EXEC_CACHE='full',
               NCE_TRANSLATION_TLB='full', NCE_TRANSLATION_LINUX_FASTPATH='0', NCE_TRANSLATION_CACHE_MIB='16',
               NCE_DIRECT_MEMORY='flat', NCE_DIRECT_CODE_WATCH='1', NCE_DIRECT_CROSS_PAGE='1', NCE_DIRECT_NATIVE_EXCLUSIVE='1')
    runs = []
    for entry in ('cli', 'gcd'):
        command = [str(out / entry), str(kernel / 'lkl-image.bin')]
        commands.append(command)
        with (out / (entry + '.jsonl')).open('w') as stdout, (out / (entry + '.log')).open('w') as stderr:
            try:
                result = subprocess.run(command, env=env, stdout=stdout, stderr=stderr, timeout=180)
                returncode, timed_out = result.returncode, False
            except subprocess.TimeoutExpired:
                returncode, timed_out = -1, True
        rows = [json.loads(line) for line in (out / (entry + '.jsonl')).read_text().splitlines() if line.startswith('{')]
        expected = {(name, size): 4 for name in ('bpf_ringbuf_kernel_producer', 'bpf_ringbuf_user_producer_mappings')
                    for size in (16384, 32768, 4194304)}
        observed = {key: sum(r.get('test') == key[0] and r.get('bytes') == key[1] and r.get('passed') is True for r in rows)
                    for key in expected}
        cleanups = [r for r in rows if r.get('test') == 'bpf_ringbuf_cleanup' and r.get('passed') is True]
        contexts = [r for r in rows if r.get('test') == 'translated_context_cleanup']
        validated = (observed == expected and len(cleanups) == 4 and len(contexts) == 1
                     and contexts[0].get('allocated') == contexts[0].get('freed') == 1
                     and sum(r.get('result') == 'passed' for r in rows) == 1)
        runs.append({'entry': entry, 'returncode': returncode, 'timed_out': timed_out, 'validated': validated, 'results': rows,
                     'stdout_sha256': digest(out / (entry + '.jsonl')), 'stderr_sha256': digest(out / (entry + '.log'))})
        print(entry, returncode, flush=True)
    inputs_unchanged = (all(digest(baseline_root / p) == h for p, h in report['sha256'].items())
                        and all(digest(ROOT / p) == h for p, h in build['sha256'].items()))
    files = [guest_source, Path(__file__), ROOT / 'scripts/lkl_4k_vmalloc_patch.py',
             kernel / 'source/kernel/bpf/ringbuf.c', kernel / 'build.json', kernel / 'lkl-image.bin',
             kernel / 'lkl-image.h', out / 'guest.elf', out / 'cli', out / 'gcd']
    files += [ROOT / p for p in report['sha256'] if p.startswith('experiments/') and (ROOT / p).is_file()]
    summary = {'scope': 'macOS 4KiB Linux BPF ringbuf syscall, mmap and producer-consumer semantics; original system loader not run',
               'base_report_sha256': digest(baseline / 'summary.json'), 'base_inputs': report['sha256'], 'inputs_unchanged': inputs_unchanged,
               'commands': commands, 'environment': {k: v for k, v in env.items() if k.startswith('NCE_')},
               'sha256': {str(p): digest(p) for p in files}, 'runs': runs}
    (out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    if not inputs_unchanged or any(r['returncode'] != 0 or not r['validated'] for r in runs):
        raise SystemExit('ringbuf 功能检查失败，保留本轮输出')


if __name__ == '__main__':
    main()
