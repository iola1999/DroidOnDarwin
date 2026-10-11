"""分析 Metal drawable 的获取与显示时间；不推断 Android 帧的一一对应。"""
import math


def analyze_presentation(data, beginning, ending, distribution):
    errors = []
    if data.get('version') != 1:
        return {'available': True, 'failures': ['unknown presentation schema']}
    for name in ('overflow', 'duplicate_callbacks', 'clock_errors'):
        if data.get(name) != 0:
            errors.append(f'presentation {name}: {data.get(name)}')
    samples = [data.get('begin'), data.get('end')]
    offsets = []
    for row in samples:
        if not row:
            errors.append('missing presentation clock boundary')
            continue
        before, after, media = (row.get(k) for k in ('host_before_ns', 'host_after_ns', 'media_seconds'))
        if not all(isinstance(v, (int, float)) and math.isfinite(v) for v in (before, after, media)) or before <= 0 or after < before:
            errors.append('invalid presentation clock sample')
            continue
        offsets.append(((before + after) / 2 - media * 1e9, (after - before) / 2))
    offset = sum(v[0] for v in offsets) / len(offsets) if offsets else None
    uncertainty = max((abs(v[0] - offset) + v[1] for v in offsets), default=None)
    if beginning is None or ending is None or beginning >= ending:
        errors.append('missing host capture window')
    rows = data.get('frames', [])
    counts = {'captured': len(rows), 'in_window': 0, 'pending_acquisition': 0, 'drawable_unavailable': 0,
              'pending_callback': 0, 'zero_presented_time': 0, 'outside_presentation_window': 0, 'presented_in_window': 0}
    seen = set()
    acquire, queue = [], []
    presented = {}
    for frame in rows:
        start, end = frame.get('acquire_begin_ns'), frame.get('acquire_end_ns')
        if not isinstance(start, int) or start <= 0:
            errors.append('invalid drawable acquisition start')
            continue
        inside = beginning is not None and ending is not None and beginning <= start <= ending
        if not inside:
            continue
        counts['in_window'] += 1
        if end is None:
            counts['pending_acquisition'] += 1
            continue
        if not isinstance(end, int) or end < start:
            errors.append('invalid drawable acquisition end')
            continue
        acquire.append((end - start) / 1e6)
        if frame.get('drawable_unavailable'):
            counts['drawable_unavailable'] += 1
            continue
        identity = (frame.get('layer'), frame.get('drawable_id'))
        if not isinstance(identity[0], str) or not isinstance(identity[1], int) or identity[1] < 0 or identity in seen:
            errors.append('invalid or duplicate drawable identity')
            continue
        seen.add(identity)
        callback, time = frame.get('callback_ns'), frame.get('presented_time_seconds')
        if callback is None:
            counts['pending_callback'] += 1
            continue
        if not isinstance(callback, int) or callback < end or not isinstance(time, (int, float)) or not math.isfinite(time) or time < 0:
            errors.append('invalid drawable callback')
            continue
        if time == 0:
            counts['zero_presented_time'] += 1
            continue
        if offset is None:
            continue
        host = time * 1e9 + offset
        if host + uncertainty < end or host - uncertainty > callback:
            errors.append('presentation time outside acquisition/callback range')
            continue
        if not beginning <= host <= ending:
            counts['outside_presentation_window'] += 1
            continue
        counts['presented_in_window'] += 1
        presented.setdefault(identity[0], []).append(host)
        queue.append(max(0, host - end) / 1e6)
    intervals = []
    layers = []
    for layer, timestamps in presented.items():
        times = sorted(set(timestamps))
        values = [(b - a) / 1e6 for a, b in zip(times, times[1:])]
        intervals.extend(values)
        layers.append({'layer': layer, 'drawables': len(timestamps), 'unique_present_times': len(times), 'interval_ms': distribution(values)})
    return {'available': True, 'failures': errors, 'counts': counts,
            'alignment_offset_ns': offset, 'alignment_uncertainty_ns': uncertainty,
            'acquire_wait_ms': distribution(acquire), 'drawable_to_present_ms': distribution(queue),
            'unique_presentation_interval_ms': distribution(intervals), 'layers': layers,
            'scope': 'host drawable display; repeated content and Android frame identity unmeasured; pending and zero timestamps excluded'}
