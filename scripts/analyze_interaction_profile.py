#!/usr/bin/env python3
"""分析单轮 Settings 帧采集；保留无效帧和采集边界，帧完成时间不含宿主显示确认。"""
import argparse
import csv as csv_module
import io
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import subprocess

REQUIRED = ('Flags', 'IntendedVsync', 'FrameCompleted')
PHASES = {
    'before_input': ('IntendedVsync', 'HandleInputStart'),
    'main_to_sync': ('HandleInputStart', 'SyncQueued'),
    'render_queue': ('SyncQueued', 'SyncStart'),
    'render_setup': ('SyncStart', 'IssueDrawCommandsStart'),
    'issue_to_swap': ('IssueDrawCommandsStart', 'SwapBuffers'),
    'swap_to_completed': ('SwapBuffers', 'FrameCompleted'),
}


def percentile(values, fraction):
    values = sorted(values)
    position = (len(values) - 1) * fraction
    low = int(position)
    high = min(low + 1, len(values) - 1)
    return values[low] + (values[high] - values[low]) * (position - low)


def distribution(values):
    if not values:
        return {'count': 0}
    return {'count': len(values), 'p50_ms': percentile(values, .5), 'p95_ms': percentile(values, .95),
            'p99_ms': percentile(values, .99), 'max_ms': max(values),
            'p95_small_sample': len(values) < 200, 'p99_small_sample': len(values) < 1000,
            'tail_count_reference': {'p95': len(values) // 20, 'p99': len(values) // 100},
            'percentile_method': 'linear interpolation; small-sample flag uses fewer than 10 expected tail observations; frames are not independent runs',
            'above_ms': {str(limit): sum(value > limit for value in values) for limit in (16.666667, 100, 250, 1000)}}


def read_frames(text):
    rows, errors = [], []
    window, header, inside = None, None, False
    for number, line in enumerate(text.splitlines(), 1):
        if line.startswith('Window:'):
            window = line.partition(':')[2].strip()
        if line.strip() == '---PROFILEDATA---':
            inside = not inside
            header = None
            continue
        if not inside or not line.strip():
            continue
        fields = [field.strip() for field in line.rstrip(',').split(',')]
        if header is None:
            if not set(REQUIRED) <= set(fields):
                errors.append({'line': number, 'error': 'missing required frame columns'})
            header = fields
            continue
        try:
            if len(fields) != len(header):
                raise ValueError('frame column count mismatch')
            row = dict(zip(header, map(int, fields)))
            if not set(REQUIRED) <= row.keys():
                raise ValueError('missing required frame values')
            rows.append({'window': window, 'line': number, 'values': row})
        except ValueError as error:
            errors.append({'line': number, 'error': str(error)})
    if inside:
        errors.append({'error': 'unterminated frame section'})
    return rows, errors


def read_frame_totals(text):
    totals = []
    window = None
    for line in text.splitlines():
        if line.startswith('Window:'):
            window = line.partition(':')[2].strip()
        match = re.fullmatch(r'Stats since: (\d+)ns', line)
        if match:
            totals.append({'window': window, 'since_ns': int(match[1])})
        match = re.fullmatch(r'Total frames rendered: (\d+)', line)
        if match and totals:
            totals[-1]['total_frames'] = int(match[1])
    return totals


def clock_alignment(pairs):
    good = [pair for pair in pairs if pair['status'] == 0 and pair['host_after_ns'] >= pair['host_before_ns']]
    groups = {}
    for pair in good:
        groups.setdefault(pair['command'], []).append(pair)
    selected = []
    for command, values in groups.items():
        best = min(values, key=lambda p: p['host_after_ns'] - p['host_before_ns'])
        selected.append({'command': command, 'offset_ns': best['guest_monotonic_ns'] -
                         (best['host_after_ns'] + best['host_before_ns']) / 2,
                         'uncertainty_ns': (best['host_after_ns'] - best['host_before_ns']) / 2 + max(best.get('host_resolution_ns', 0), best.get('guest_resolution_ns', 0)),
                         'resolution_known': best.get('resolution_status') == 0 and best.get('host_resolution_ns', 0) > 0 and best.get('guest_resolution_ns', 0) > 0})
    return selected


def read_thread_map(text):
    records = []
    current = None
    for line in text.splitlines():
        match = re.fullmatch(r'\[thread global_pid=(\d+) global_tid=(\d+)\]', line)
        if match:
            current = {'global_pid': int(match[1]), 'global_tid': int(match[2])}
            records.append(current)
        elif current is not None:
            key, _, value = line.partition(':')
            if key in ('NSpid', 'NStgid'):
                current[key] = [int(part) for part in value.split()]
            elif key == 'Name':
                current['name'] = value.strip()
    return records


def execution_summary(output):
    path = output / 'execution.json'
    if not path.exists():
        return {'available': False}
    raw = json.loads(path.read_text())
    snapshots = []
    for name in ('threads-before.txt', 'threads-after.txt'):
        if (output / name).exists():
            snapshots.extend({**row, 'snapshot': name} for row in read_thread_map((output / name).read_text()))
    for thread in raw['threads']:
        # Android init mounts procfs inside its child PID namespace. The host
        # snapshot retains both the initial IDs used by ftrace and Android IDs.
        matches = [row for row in snapshots if len(row.get('NSpid', [])) > 1 and len(row.get('NStgid', [])) > 1
                   and row['NSpid'][-1] == thread['android_tid'] and row['NStgid'][-1] == thread['android_pid']]
        ids = {(row['global_pid'], row['global_tid']) for row in matches}
        thread['namespace_match'] = 'unique' if len(ids) == 1 else 'missing' if not ids else 'ambiguous'
        if len(ids) == 1:
            thread['global_pid'], thread['global_tid'] = next(iter(ids))
            thread['names'] = sorted({row.get('name', '') for row in matches})
            thread['observed_in'] = sorted({row['snapshot'] for row in matches})
    raw['available'] = True
    raw['stage_totals'] = [{key: sum(thread['stages'][i][key] for thread in raw['threads'])
                           for key in ('wall_ns', 'cpu_ns', 'calls')} for i in range(len(raw['stages']))]
    raw['thread_mapping_scope'] = 'before/after snapshots; new or exited threads may be missing; PID reuse requires ftrace lifecycle inspection'
    return raw


def host_visibility(result, beginning, ending):
    known = [event for event in result['events'] if 'app_visible' in event and 'host_monotonic_ns' in event]
    if not known or beginning is None or ending is None:
        return {'available': False}
    before = [event for event in known if event['host_monotonic_ns'] <= beginning]
    selected = before[-1:] + [event for event in known if beginning < event['host_monotonic_ns'] <= ending]
    return {'available': bool(selected), 'tracking': result.get('visibility_tracking', 'action boundaries only'),
            'all_visible': bool(before) and all(event['app_visible'] for event in selected),
            'all_active': bool(before) and all(event.get('app_active') for event in selected),
            'all_activity_asserted': bool(before) and all(event.get('benchmark_activity') for event in selected),
            'events': [{key: event.get(key) for key in ('host_monotonic_ns', 'event', 'app_visible', 'app_active', 'benchmark_activity')} for event in selected],
            'scope': 'App-wide occlusion and activation; actual drawable presentation unmeasured'}


def analyze(directory):
    request = json.loads((directory / 'request.json').read_text())
    result = json.loads((directory / 'result.json').read_text())
    output = directory / 'interaction'
    pairs = [json.loads(line) for line in (output / 'clock-pairs.jsonl').read_text().splitlines()]
    clocks = clock_alignment(pairs)
    starts = [item for item in clocks if item['command'] in (2, 3)]
    ends = [item for item in clocks if item['command'] == 4]
    failures = []
    if result['status'] != 'completed':
        failures.append('benchmark did not complete')
    if not starts or not ends:
        failures.append('missing capture clock calibration')
    selected = starts + ends
    offset = sum(item['offset_ns'] for item in selected) / len(selected) if selected else 0
    uncertainty = max((abs(item['offset_ns'] - offset) + item['uncertainty_ns'] for item in selected), default=None)
    action_start = next((i for i, action in enumerate(request['actions']) if action.get('key') in ('frames-start', 'trace-start')), None)
    action_end = next((i for i, action in enumerate(request['actions']) if action.get('key') == 'capture-end'), None)
    beginning = next((event['host_monotonic_ns'] for event in result['events']
                      if event['event'] == 'action_submitted' and event.get('action') == action_start), None)
    ending = next((event['host_monotonic_ns'] for event in result['events']
                   if event['event'] == 'action_started' and event.get('action') == action_end), None)
    if beginning is None or ending is None or beginning >= ending:
        failures.append('missing completed capture window')
    frame_text = (output / 'frames.txt').read_text()
    rows, errors = read_frames(frame_text)
    totals = read_frame_totals(frame_text)
    if beginning is not None:
        for total in totals:
            if total['window'] and total['since_ns'] > beginning + offset + (uncertainty or 0):
                failures.append('window statistics restarted during capture: ' + total['window'])
    process_total = next((total.get('total_frames') for total in totals if total['window'] is None), None)
    window_total = sum(total.get('total_frames', 0) for total in totals if total['window'])
    if process_total is not None and process_total > window_total:
        failures.append('process frame total exceeds exported window totals')
    failures.extend(error['error'] for error in errors)
    if not rows:
        failures.append('no frame records')
    for total in totals:
        if total['window'] and total.get('total_frames', 0) > sum(row['window'] == total['window'] for row in rows):
            failures.append('window frame history is shorter than its rendered total: ' + total['window'])
    counts, seen, accepted = Counter(), set(), []
    for row in rows:
        values = row['values']
        identity = (row['window'], values.get('FrameTimelineVsyncId'), values['IntendedVsync'])
        start, end = values['IntendedVsync'], values['FrameCompleted']
        reason = None
        if identity in seen:
            reason = 'duplicate'
        elif values['Flags'] != 0:
            reason = 'nonzero_flags'
        elif not (0 < start <= end < 2**63 - 1):
            reason = 'incomplete_or_invalid_time'
        elif beginning is None or ending is None:
            reason = 'unknown_window'
        elif start < beginning + offset or end > ending + offset:
            reason = 'outside_or_crossing_window'
        if reason is None and uncertainty is not None and (start < beginning + offset + uncertainty or end > ending + offset - uncertainty):
            reason = 'clock_uncertainty_boundary'
        seen.add(identity)
        row['excluded'] = reason
        counts[reason or 'accepted'] += 1
        if reason:
            continue
        row['duration_ms'] = (end - start) / 1e6
        row['phases_ms'] = {name: (values[b] - values[a]) / 1e6 for name, (a, b) in PHASES.items()
                            if a in values and b in values and 0 < values[a] <= values[b] < 2**63 - 1}
        accepted.append(row)
    if not accepted:
        failures.append('no valid frames entirely within the operation window')
    cpu_stats = {}
    for path in sorted(output.glob('cpu*.txt')):
        values = {}
        for line in path.read_text().splitlines():
            key, separator, value = line.partition(':')
            if separator and re.fullmatch(r'\s*\d+\s*', value):
                values[key.strip()] = int(value)
        cpu_stats[path.stem] = values
        for field in ('overrun', 'commit overrun', 'dropped events'):
            if field not in values:
                failures.append(f'{path.stem}: missing {field}')
            elif values[field]:
                failures.append(f'{path.stem}: {field}={values[field]}')
    if action_start is not None and request['actions'][action_start]['key'] == 'trace-start':
        if not cpu_stats or not (output / 'kernel.ftrace').is_file():
            failures.append('scheduler capture missing')
    execution = execution_summary(output)
    if execution['available'] and (execution['overflow'] or execution['clock_errors'] or execution.get('fallback_overflow')):
        failures.append('execution collection overflow or clock errors')
    if execution['available'] and 'fallbacks' in execution:
        fallback_total = execution['stage_totals'][2]
        if any(sum(row[key] for row in execution['fallbacks']) != fallback_total[key] for key in ('cpu_ns', 'wall_ns', 'calls')):
            failures.append('fallback attribution does not cover all Unicorn intervals')
    if execution['available'] and not execution['threads']:
        failures.append('execution capture contains no threads')
    files = [directory / name for name in ('request.json', 'result.json')] + sorted(output.glob('*'))
    return {'version': 1, 'analyzer_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(), 'runID': result['runID'], 'buildID': result['buildID'],
            'configuration': result['configuration'], 'hardware': result['hardware'], 'os': result['os'],
            'benchmark_status': result['status'], 'failures': failures,
            'host_visibility': host_visibility(result, beginning, ending),
            'scope': 'Settings HWUI completion, wall time including scheduling/waits; host presentation unmeasured; no FPS claim',
            'clocks': clocks, 'alignment_uncertainty_ns': uncertainty if selected and all(c['resolution_known'] for c in selected) else None,
            'alignment_bracket_half_width_ns': uncertainty,
            'host_window_ns': [beginning, ending], 'guest_window_ns': [value + offset if value is not None else None for value in (beginning, ending)],
            'frame_totals': totals, 'frame_counts': dict(counts), 'frames': rows, 'duration': distribution([row['duration_ms'] for row in accepted]),
            'phase_duration': {name: distribution([row['phases_ms'][name] for row in accepted if name in row['phases_ms']]) for name in PHASES},
            'cpu_stats': cpu_stats, 'ring_capacity_known': False, 'execution': execution,
            'source_sha256': {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in files if path.is_file()}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--trace-processor', type=Path)
    parser.add_argument('--require-visible', action='store_true', help='要求采集期间 Mac App 持续可见且有状态变化记录')
    args = parser.parse_args()
    report = analyze(args.run)
    if args.require_visible:
        visible = report['host_visibility']
        if not visible.get('all_visible') or visible.get('tracking') != 'AppKit notifications and action boundaries':
            report['failures'].append('continuous visible Mac capture not confirmed')
    if args.trace_processor and (args.run / 'interaction/kernel.ftrace').exists():
        begin, end = (int(value) for value in report['guest_window_ns'])
        overlap = f"max(0,min(ts+dur,{end})-max(ts,{begin}))"
        query = f"""select name,value,severity from stats where value != 0 and severity != 'info';
select t.tid,t.name,s.state,sum({overlap}) as wall_ns
from thread_state s join thread t using(utid)
where dur > 0 and ts < {end} and ts+dur > {begin}
group by t.utid,s.state order by t.tid,s.state;"""
        sql = args.output.with_suffix('.sql')
        sql.write_text(query + '\n')
        command = [str(args.trace_processor), 'query', str(args.run / 'interaction/kernel.ftrace'), '-f', str(sql)]
        parsed = subprocess.run(command, text=True, capture_output=True, timeout=60)
        csv = args.output.with_suffix('.sched.csv')
        csv.write_text(parsed.stdout)
        args.output.with_suffix('.parser.log').write_text(parsed.stderr)
        report['trace_processor'] = {'command': command, 'returncode': parsed.returncode,
                                     'sha256': hashlib.sha256(args.trace_processor.read_bytes()).hexdigest(),
                                     'query_sha256': hashlib.sha256(sql.read_bytes()).hexdigest(), 'output': str(csv)}
        if parsed.returncode:
            report['failures'].append('trace processor failed')
        else:
            stats = list(csv_module.DictReader(io.StringIO(parsed.stdout.partition('\n\n')[0])))
            report['trace_processor']['nonzero_noninfo_stats'] = stats
            if stats:
                report['failures'].append('trace processor reported nonzero diagnostic statistics')
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps({key: report[key] for key in ('runID', 'failures', 'frame_counts', 'duration', 'alignment_uncertainty_ns')}, ensure_ascii=False))
    return 1 if report['failures'] else 0


if __name__ == '__main__':
    raise SystemExit(main())
