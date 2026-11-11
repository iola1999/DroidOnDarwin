#!/usr/bin/env python3
"""分别分析 Settings 帧完成与可选 Metal 显示回调，保留无效帧和采集边界。"""
import argparse
import csv as csv_module
import io
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import subprocess
from interaction_presentation import analyze_presentation

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


def trace_event_counts(text):
    return dict(Counter(re.findall(r'\d+\.\d+: (\w+):', text)))


def userspace_begin_count(text):
    return len(re.findall(r': (?:tracing_mark_write|print):[^\n]*?\bB\|\d+\|', text))


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
    window_tracking = result.get('visibility_tracking') == 'Android window AppKit notifications and action boundaries'
    visible_key = 'display_visible' if window_tracking else 'app_visible'
    known = [event for event in result['events'] if 'app_visible' in event and 'host_monotonic_ns' in event]
    if not known or beginning is None or ending is None:
        return {'available': False}
    before = [event for event in known if event['host_monotonic_ns'] <= beginning]
    selected = before[-1:] + [event for event in known if beginning < event['host_monotonic_ns'] <= ending]
    return {'available': bool(selected), 'tracking': result.get('visibility_tracking', 'action boundaries only'),
            'all_visible': bool(before) and all(event.get(visible_key, False) for event in selected),
            'all_active': bool(before) and all(event.get('app_active') for event in selected),
            'all_activity_asserted': bool(before) and all(event.get('benchmark_activity') for event in selected),
            'events': [{key: event.get(key) for key in ('host_monotonic_ns', 'event', 'app_visible', 'display_visible', 'app_active', 'benchmark_activity')} for event in selected],
            'scope': ('Android window occlusion and App activation; drawable presentation reported separately' if window_tracking else 'App-wide occlusion and activation; Android drawable visibility not confirmed')}


def settings_launch(output, actions, start, end, window_start, window_end, uncertainty, totals):
    opened = [i for i, action in enumerate(actions) if action.get('key') == 'settings']
    requested = start is not None and end is not None and any(start < i < end for i in opened)
    report = {'requested': requested, 'available': False, 'failures': []}
    if not requested:
        if (output / 'launch.jsonl').exists():
            report['failures'].append('unexpected first-open metadata')
        return report, set()
    failures = report['failures']
    if len(opened) != 1:
        failures.append('first-open capture contains a prepared or repeated Settings launch')
    try:
        records = [json.loads(line) for line in (output / 'launch.jsonl').read_text().splitlines()]
        report.update(available=True, records=records)
        if [item['phase'] for item in records] != ['prepared', 'begin', 'ready', 'end']:
            raise ValueError('invalid first-open phase sequence')
        if any(item['mode'] != 'force_stopped_process' or type(item['guest_ns']) is not int or item['guest_ns'] <= 0 for item in records):
            raise ValueError('invalid first-open mode or time')
        prepared, begin, ready, final = records
        if any(type(prepared[key]) is not int or prepared[key] != 0 for key in ('pid', 'starttime_ticks')):
            raise ValueError('Settings process was present after force-stop')
        for item in (ready, final):
            if any(type(item[key]) is not int or item[key] <= 0 for key in ('pid', 'starttime_ticks')):
                raise ValueError('missing first-open process identity')
        if type(ready['snapshot_end_ns']) is not int or not prepared['guest_ns'] <= begin['guest_ns'] <= ready['guest_ns'] <= ready['snapshot_end_ns'] <= final['guest_ns']:
            raise ValueError('invalid first-open time order')
        if window_start is None or window_end is None or begin['guest_ns'] < window_start - uncertainty or ready['snapshot_end_ns'] > window_end + uncertainty:
            raise ValueError('first-open operation is outside capture window')
        if any(ready[key] != final[key] for key in ('pid', 'starttime_ticks')):
            raise ValueError('Settings process identity changed during capture')
        snapshot = (output / 'launch-frames.txt').read_text()
        snapshot_rows, errors = read_frames(snapshot)
        if errors:
            raise ValueError('malformed first-open frame snapshot')
        snapshot_totals = read_frame_totals(snapshot)
        report.update(available=True, records=records, ready_frame_totals=snapshot_totals,
                      begin_to_ready_ms=(ready['guest_ns'] - begin['guest_ns']) / 1e6,
                      ready_snapshot_ms=(ready['snapshot_end_ns'] - ready['guest_ns']) / 1e6,
                      scope='force-stop followed by process launch; existing app data and ART caches retained; ready includes polling and dumps; no first-install claim',
                      window_lifecycle_scope='ready/end HWUI snapshots and process totals; unrendered windows before ready are unobserved')
        allowed = set()
        previous = {item['window']: item for item in snapshot_totals}
        current = {item['window']: item for item in totals}
        if len(previous) != len(snapshot_totals) or len(current) != len(totals) or previous.keys() != current.keys() or None not in previous or len(previous) < 2:
            raise ValueError('first-open frame windows are missing, duplicated or changed')
        for name, item in previous.items():
            final_item = current[name]
            if (not begin['guest_ns'] <= item['since_ns'] <= ready['snapshot_end_ns'] or
                    item['since_ns'] != final_item['since_ns'] or
                    'total_frames' not in item or final_item.get('total_frames', -1) < item['total_frames']):
                raise ValueError('first-open statistics restarted or predate launch')
            if name:
                if item['total_frames'] > sum(row['window'] == name for row in snapshot_rows):
                    raise ValueError('first-open ready frame history is incomplete')
                allowed.add((name, item['since_ns']))
        if previous[None]['total_frames'] != sum(item['total_frames'] for name, item in previous.items() if name):
            raise ValueError('first-open ready process and window totals differ')
        if current[None]['total_frames'] != sum(item['total_frames'] for name, item in current.items() if name):
            raise ValueError('first-open final process and window totals differ')
        if failures:
            return report, set()
        return report, allowed
    except (OSError, ValueError, KeyError, TypeError) as error:
        failures.append('first-open validation: ' + str(error))
        return report, set()


def analyze(directory):
    request = json.loads((directory / 'request.json').read_text())
    result = json.loads((directory / 'result.json').read_text())
    output = directory / 'interaction'
    pair_file = output / 'clock-pairs.jsonl'
    pairs = [json.loads(line) for line in pair_file.read_text().splitlines()] if pair_file.exists() else []
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
    frame_file = output / 'frames.txt'
    frame_text = frame_file.read_text() if frame_file.exists() else ''
    rows, errors = read_frames(frame_text)
    totals = read_frame_totals(frame_text)
    launch, new_windows = settings_launch(output, request['actions'], action_start, action_end,
                                         beginning + offset if beginning is not None else None,
                                         ending + offset if ending is not None else None, uncertainty or 0, totals)
    failures.extend(launch['failures'])
    if beginning is not None:
        for total in totals:
            if total['window'] and total['since_ns'] > beginning + offset + (uncertainty or 0) and (total['window'], total['since_ns']) not in new_windows:
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
    frame_failures = set()
    valid_window = beginning is not None and ending is not None and beginning < ending
    window_start = beginning + offset if valid_window else None
    window_end = ending + offset if valid_window else None
    for row in rows:
        values = row['values']
        identity = (row['window'], values.get('FrameTimelineVsyncId'), values['IntendedVsync'])
        start, end = values['IntendedVsync'], values['FrameCompleted']
        valid_start = 0 < start < 2**63 - 1
        completed = valid_start and start <= end < 2**63 - 1
        row['completion_status'] = ('completed' if completed else
                                    'unreported' if valid_start and end in (0, 2**63 - 1) else 'invalid')
        if completed:
            row['duration_ms'] = (end - start) / 1e6
        if not valid_start or not valid_window:
            boundary = 'unknown'
        elif start < window_start:
            boundary = ('overlap_unknown' if not completed else
                        'crosses_start' if end > window_start else 'before_window')
        elif start >= window_end:
            boundary = 'after_window'
        elif not completed:
            boundary = 'completion_unavailable'
            # An unavailable or invalid completion timestamp cannot establish
            # a duration bound; retain only the interval to the window end.
            row['elapsed_to_window_end_ms'] = (window_end - start) / 1e6
        else:
            boundary = 'crosses_end' if end > window_end else 'within_window'
        row['window_boundary'] = boundary
        start_uncertain = bool(valid_window and valid_start and uncertainty and
                               any(abs(start - bound) < uncertainty for bound in (window_start, window_end)))
        end_uncertain = bool(valid_window and completed and uncertainty and
                             start < window_end + uncertainty and end > window_start - uncertainty and
                             any(abs(end - bound) < uncertainty for bound in (window_start, window_end)))
        row['clock_boundary_uncertain'] = start_uncertain or end_uncertain
        if identity not in seen:
            if boundary in ('crosses_start', 'crosses_end'):
                frame_failures.add('frame crosses capture window boundary')
            if boundary == 'completion_unavailable':
                frame_failures.add('frame starting in capture window has no valid completion timestamp')
            if boundary == 'overlap_unknown':
                frame_failures.add('frame starting before capture window has unknown overlap')
            if not valid_start:
                frame_failures.add('frame has no valid start timestamp')
            if row['clock_boundary_uncertain']:
                frame_failures.add('frame capture window boundary is uncertain')
        reason = None
        if identity in seen:
            reason = 'duplicate'
        elif values['Flags'] != 0:
            reason = 'nonzero_flags'
        elif not completed:
            reason = 'incomplete_or_invalid_time'
        elif not valid_window:
            reason = 'unknown_window'
        elif not window_start <= start < window_end:
            reason = 'outside_start_window'
        elif start_uncertain:
            reason = 'clock_uncertainty_boundary'
        seen.add(identity)
        row['excluded'] = reason
        counts[reason or 'accepted'] += 1
        if reason:
            continue
        row['phases_ms'] = {name: (values[b] - values[a]) / 1e6 for name, (a, b) in PHASES.items()
                            if a in values and b in values and 0 < values[a] <= values[b] < 2**63 - 1}
        accepted.append(row)
    if not accepted:
        failures.append('no completed frames with unambiguous start in the operation window')
    failures.extend(sorted(frame_failures))
    exported_frame_window_complete = not failures
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
    presentation_path = output / 'presentation.json'
    presentation = {'available': False}
    if presentation_path.exists():
        presentation = analyze_presentation(json.loads(presentation_path.read_text()), beginning, ending, distribution)
        presentation['analyzer_sha256'] = hashlib.sha256(Path(__file__).with_name('interaction_presentation.py').read_bytes()).hexdigest()
        failures.extend(presentation['failures'])
    elif result.get('presentation_capture'):
        failures.append('missing requested drawable presentation capture')
    files = [directory / name for name in ('request.json', 'result.json')] + sorted(output.glob('*'))
    trace = output / 'kernel.ftrace'
    trace_text = trace.read_text() if trace.exists() else ''
    return {'version': 2, 'analyzer_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(), 'runID': result['runID'], 'buildID': result['buildID'],
            'configuration': result['configuration'], 'hardware': result['hardware'], 'os': result['os'],
            'benchmark_status': result['status'], 'failures': failures,
            'host_visibility': host_visibility(result, beginning, ending),
            'scope': ('Settings HWUI completion and separate Metal drawable presentation; Android/host frame identity unmeasured' if presentation['available'] else 'Settings HWUI completion, wall time including scheduling/waits; host presentation unmeasured; no FPS claim'),
            'clocks': clocks, 'alignment_uncertainty_ns': uncertainty if selected and all(c['resolution_known'] for c in selected) else None,
            'alignment_bracket_half_width_ns': uncertainty,
            'host_window_ns': [beginning, ending], 'guest_window_ns': [value + offset if value is not None else None for value in (beginning, ending)],
            'exported_frame_window_complete': exported_frame_window_complete,
            'settings_launch': launch,
            'duration_scope': 'exported zero-flag completed frames with unambiguous IntendedVsync in [start,end); full duration including completion after end; missing frames and unavailable completions are not in quantiles',
            'presentation': presentation, 'frame_totals': totals, 'frame_counts': dict(counts), 'frames': rows, 'duration': distribution([row['duration_ms'] for row in accepted]),
            'phase_duration': {name: distribution([row['phases_ms'][name] for row in accepted if name in row['phases_ms']]) for name in PHASES},
            'cpu_stats': cpu_stats, 'ring_capacity_known': False, 'execution': execution,
            'trace_event_counts': trace_event_counts(trace_text), 'userspace_begin_markers': userspace_begin_count(trace_text),
            'source_sha256': {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in files if path.is_file()}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--trace-processor', type=Path)
    parser.add_argument('--require-visible', action='store_true', help='要求采集期间 Mac App 持续可见且有状态变化记录')
    parser.add_argument('--require-userspace-trace', action='store_true', help='要求导出的 trace 含用户态标记；旧调度采集可省略')
    args = parser.parse_args()
    report = analyze(args.run)
    if args.require_userspace_trace and not report['userspace_begin_markers']:
        report['failures'].append('userspace trace markers missing')
    if args.require_visible:
        visible = report['host_visibility']
        if not visible.get('all_visible') or visible.get('tracking') not in ('AppKit notifications and action boundaries', 'Android window AppKit notifications and action boundaries'):
            report['failures'].append('continuous visible Mac capture not confirmed')
        if report['presentation'].get('available') and not report['presentation'].get('counts', {}).get('presented_in_window'):
            report['failures'].append('no drawable presentation in visible capture window')
    trace_window_known = all(value is not None for value in report['guest_window_ns']) and report['guest_window_ns'][0] < report['guest_window_ns'][1]
    if args.trace_processor and not trace_window_known:
        report['failures'].append('trace processor requires a completed capture window')
    if args.trace_processor and trace_window_known and (args.run / 'interaction/kernel.ftrace').exists():
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
