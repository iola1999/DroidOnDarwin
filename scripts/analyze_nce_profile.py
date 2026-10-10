#!/usr/bin/env python3
"""汇总 Android 启动日志与 macOS sample 调用树，分开报告等待样本。"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re


def sample_summary(text):
    if 'Call graph:' not in text:
        raise ValueError('Missing sample call graph')
    graph = text.split('Call graph:', 1)[1].split('Total number in stack', 1)[0]
    nodes, parents = [], []
    for line in graph.splitlines():
        match = re.match(r'^(\s*[+!|: ]*?)(\d+) (.+)$', line)
        if not match:
            continue
        depth, count, name = len(match[1]), int(match[2]), match[3]
        while parents and parents[-1]['depth'] >= depth:
            parents.pop()
        node = dict(depth=depth, count=count, name=name, children=0,
                    parent=parents[-1] if parents else None)
        if parents:
            parents[-1]['children'] += count
        nodes.append(node)
        parents.append(node)

    groups, functions = Counter(), Counter()
    for node in nodes:
        own = node['count'] - node['children']
        if own < 0:
            raise ValueError('Child samples exceed parent: ' + node['name'])
        if not own:
            continue
        name = node['name'].split('  (in ', 1)[0]
        path, parent = [], node
        while parent:
            path.append(parent['name'])
            parent = parent['parent']
        ancestry = '\n'.join(path)
        # Mach IPC inside vm_remap belongs to the mapping call. Event-loop
        # Mach waits have a different caller and are kept in the wait group.
        if any(token in ancestry for token in ('nce_memory_remap ', 'vm_remap ', 'protect_none ', 'restore_code ')):
            group = 'host_mapping'
        elif any(token in name for token in ('__psynch_cvwait', '__semwait_signal', '__workq_kernreturn', 'mach_msg2_trap')):
            group = 'explicit_wait'
        elif any(token in name for token in ('_sigtramp', 'fault', 'nce_event_gate')):
            group = 'signal_handling'
        elif any(token in ancestry for token in ('prepare_resume ', 'nce_write_entry ', 'sys_icache_invalidate ')):
            group = 'resume_entry'
        elif '(in <unknown binary>)' in node['name']:
            group = 'unresolved_native_code'
        elif any(token in ancestry for token in ('libgfxstream', 'libANGLE', 'Metal')):
            group = 'host_graphics'
        else:
            group = 'other_host'
        groups[group] += own
        if group != 'explicit_wait':
            functions[name] += own
    total = sum(groups.values())
    nonwait = total - groups['explicit_wait']
    metadata = {}
    for field in ('Date/Time', 'Launch Time', 'OS Version', 'Physical footprint', 'Physical footprint (peak)'):
        match = re.search(r'^' + re.escape(field) + r':\s*(.+)$', text, re.M)
        if match:
            metadata[field] = match[1]
    return dict(metadata=metadata, all_thread_samples=total, nonwait_samples=nonwait,
                groups=dict(groups), nonwait_share={k: round(v / nonwait, 4) for k, v in groups.items()
                                                   if k != 'explicit_wait' and nonwait},
                top_nonwait_functions=functions.most_common(20))


def log_summary(text):
    mappings, timings, activities, resume = [], [], [], []
    for line_number, line in enumerate(text.splitlines(), 1):
        try:
            record = json.loads(line)
        except json.JSONDecodeError:
            record = {}
        if record.get('profile') == 'host_mapping':
            mappings.append(dict(record, line=line_number))
        if record.get('profile') == 'resume_entries':
            resume.append(dict(record, line=line_number))
        match = re.search(r'(SystemUIBootTiming|SystemServerTiming): (.+) took to complete: (\d+)ms', line)
        if match:
            timings.append(dict(tag=match[1], name=match[2], ms=int(match[3]), line=line_number,
                                timestamp=line[:18]))
        if 'ActivityTaskManager: Displayed ' in line:
            activities.append(dict(line=line_number, message=line))
    intervals = []
    previous = dict(elapsed_ms=0, data_ms=0, code_ms=0, revoke_ms=0)
    for current in mappings:
        elapsed = current['elapsed_ms'] - previous['elapsed_ms']
        if elapsed <= 0:
            raise ValueError('Mapping elapsed time must increase within one session')
        delta = {k: round(current[k] - previous[k], 3) for k in previous}
        delta['mapping_ms'] = round(sum(delta[k] for k in ('data_ms', 'code_ms', 'revoke_ms')), 3)
        delta['elapsed_end_ms'] = current['elapsed_ms']
        intervals.append(delta)
        previous = {k: current[k] for k in previous}
    selected = set()
    for tag in ('SystemServerTiming', 'SystemUIBootTiming'):
        selected.update(r['line'] for r in sorted((r for r in timings if r['tag'] == tag),
                                                  key=lambda r: r['ms'], reverse=True)[:40])
    return dict(component_timing_count=len(timings), timing_limit_per_tag=40,
                component_timings=[r for r in timings if r['line'] in selected], displayed_activities=activities,
                mapping_cumulative=mappings, mapping_intervals=intervals, resume_entries=resume,
                java_fatal_exceptions=text.count('FATAL EXCEPTION:'), anrs=text.count('ANR in '))


def source(path):
    return dict(path=str(path), sha256=hashlib.sha256(path.read_bytes()).hexdigest())


def art_summary(text):
    summaries = []
    for match in re.finditer(r'^----- pid (\d+) at ([^\n]+) -----\n(.*?)^----- end \1 -----', text, re.M | re.S):
        body = match[3]
        process = re.search(r'^Cmd line: (.+)$', body, re.M)
        if not process:
            continue
        main = re.search(r'^"main"[^\n]*\n(.*?)(?=^"|\Z)', body, re.M | re.S)
        lines = main[1].splitlines() if main else []
        java = [line.strip() for line in lines if line.strip().startswith('at ')]
        statistics = [line for line in body.splitlines() if line.startswith((
            'Total time spent in GC:', 'Total blocking GC ', 'Current JIT code cache ',
            'Current number of JIT code cache entries:', 'Total number of JIT code cache collections:',
            'Compiling baseline:', 'Compiling optimized:'))]
        summaries.append(dict(pid=int(match[1]), timestamp=match[2], process=process[1],
                              main_native_frames=[line.strip() for line in lines
                                                  if line.strip().startswith('native:')][:12],
                              main_java_frames=java, statistics=statistics))
    return summaries


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--log', required=True, type=Path)
    parser.add_argument('--results', type=Path)
    parser.add_argument('--sample', action='append', default=[], type=Path)
    parser.add_argument('--art-log', type=Path, help='Separate SIGQUIT diagnostic run; its timings are not a baseline')
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    result = dict(log=dict(source(args.log), summary=log_summary(args.log.read_text(errors='replace'))),
                  samples=[dict(source(p), summary=sample_summary(p.read_text())) for p in args.sample],
                  limits=[
                      'Nonwait shares describe sampled stack observations, not measured CPU-time percentages.',
                      'Mapping times include scheduling delay inside each call; component timers can overlap.',
                      'Unresolved native addresses include Linux, guest ELF code, branch stubs and ART JIT.',
                      'Sampling and mapping counters add overhead; this run does not measure uninstrumented startup.'
                  ])
    if args.results:
        records = [json.loads(line) for line in args.results.read_text().splitlines()]
        names = {'linux_guest_events', 'linux_host_mapping_totals', 'linux_nce_context_lifetime',
                 'android_system_boot_completed', 'android_desktop_visible_activity', 'linux_data_readonly_shutdown'}
        result['results'] = dict(source(args.results), completed=records[-1].get('result') == 'passed',
                                 records=[r for r in records if r.get('test') in names])
    if args.art_log:
        result['art_diagnostic'] = dict(source(args.art_log),
                                       snapshots=art_summary(args.art_log.read_text(errors='replace')))
        result['limits'].append('ART SIGQUIT snapshots suspend guest threads and describe only the captured instants.')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n')


if __name__ == '__main__':
    main()
