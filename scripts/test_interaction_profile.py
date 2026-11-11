#!/usr/bin/env python3
"""检查帧记录缺失、格式错误和时钟采样误差的处理。"""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from analyze_interaction_profile import analyze, read_frames, clock_alignment, distribution, read_frame_totals, read_thread_map, host_visibility, trace_event_counts, userspace_begin_count


class InteractionProfileTests(unittest.TestCase):
    def test_scheduler_only_trace_has_no_userspace_markers(self):
        text = ' app-30 [000] X.... 122.342221: sched_waking: comm=app pid=31\n'
        self.assertEqual(trace_event_counts(text), {'sched_waking': 1})
        text += ' app-30 [000] X.... 122.342222: tracing_mark_write: B|1|draw\n'
        text += ' app-30 [000] X.... 122.342223: tracing_mark_write: E|1\n'
        self.assertEqual(trace_event_counts(text), {'sched_waking': 1, 'tracing_mark_write': 2})
        self.assertEqual(userspace_begin_count(text), 1)
        self.assertEqual(userspace_begin_count(' app-30 [000] X.... 122.342222: print: kernel diagnostic\n'), 0)

    def test_android_window_visibility_overrides_app_visibility(self):
        event = {'host_monotonic_ns': 10, 'app_visible': True, 'app_active': True, 'display_visible': False}
        result = {'visibility_tracking': 'Android window AppKit notifications and action boundaries', 'events': [event]}
        self.assertFalse(host_visibility(result, 15, 40)['all_visible'])
        event['display_visible'] = True
        self.assertTrue(host_visibility(result, 15, 40)['all_visible'])
        del event['display_visible']
        self.assertFalse(host_visibility(result, 15, 40)['all_visible'])

    def test_hidden_interval_is_not_erased_by_visible_endpoints(self):
        result = {'visibility_tracking': 'AppKit notifications and action boundaries', 'events': [
            {'host_monotonic_ns': time, 'app_visible': visible, 'app_active': visible}
            for time, visible in ((10, True), (20, False), (30, True), (50, False))]}
        self.assertFalse(host_visibility(result, 15, 40)['all_visible'])
        self.assertTrue(host_visibility(result, 30, 40)['all_visible'])
        self.assertFalse(host_visibility(result, 5, 15)['all_visible'])
        self.assertFalse(host_visibility({'events': []}, 10, 40)['available'])

    def test_window_and_malformed_frames(self):
        rows, errors = read_frames('''Window: Settings
---PROFILEDATA---
Flags,IntendedVsync,FrameCompleted,
0,100,200,
0,300,
0,unknown,400,
---PROFILEDATA---
''')
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]['window'], 'Settings')
        self.assertEqual(len(errors), 2)

    def test_truncated_and_missing_columns(self):
        _, errors = read_frames('---PROFILEDATA---\nFlags,FrameCompleted\n0,4\n')
        self.assertEqual(len(errors), 3)

    def test_recreated_window_keeps_distinct_reset_time(self):
        totals = read_frame_totals('Stats since: 100ns\nTotal frames rendered: 43\nWindow: Settings\nStats since: 200ns\nTotal frames rendered: 3\n')
        self.assertEqual(totals, [{'window': None, 'since_ns': 100, 'total_frames': 43},
                                  {'window': 'Settings', 'since_ns': 200, 'total_frames': 3}])

    def test_nested_namespace_ids(self):
        rows = read_thread_map('[thread global_pid=465 global_tid=484]\nName:\tRenderThread\nNStgid:\t465\t415\nNSpid:\t484\t432\n')
        self.assertEqual(rows[0]['global_tid'], 484)
        self.assertEqual(rows[0]['NSpid'], [484, 432])
        self.assertEqual(rows[0]['NStgid'], [465, 415])
        self.assertEqual(rows[0]['name'], 'RenderThread')

    def test_clock_minimum_roundtrip_and_failure(self):
        pairs = [{'command': 3, 'status': status, 'host_before_ns': before,
                  'host_after_ns': after, 'guest_monotonic_ns': guest}
                 for status, before, after, guest in ((0, 100, 140, 30), (0, 200, 204, 102), (-5, 300, 300, 0))]
        self.assertEqual(clock_alignment(pairs), [{'command': 3, 'offset_ns': -100, 'uncertainty_ns': 2, 'resolution_known': False}])
        self.assertEqual(clock_alignment(pairs[-1:]), [])

    def test_empty_is_unknown_and_slow_samples_remain(self):
        self.assertEqual(distribution([]), {'count': 0})
        values = distribution([10, 20, 2000])
        self.assertEqual(values['above_ms']['1000'], 1)
        self.assertTrue(values['p99_small_sample'])
        self.assertEqual(values['max_ms'], 2000)


class InteractionWindowTests(unittest.TestCase):
    def make_run(self, frames, uncertainty_ns=0, offset_ns=0):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        directory = Path(temporary.name)
        output = directory / 'interaction'
        output.mkdir()
        (directory / 'request.json').write_text(json.dumps({'actions': [
            {'key': 'frames-start'}, {'key': 'capture-end'}]}))
        (directory / 'result.json').write_text(json.dumps({
            'status': 'completed', 'runID': 'window-test', 'buildID': 'synthetic',
            'configuration': {}, 'hardware': 'synthetic', 'os': 'synthetic',
            'events': [
                {'event': 'action_submitted', 'action': 0, 'host_monotonic_ns': 1_000_000_000},
                {'event': 'action_started', 'action': 1, 'host_monotonic_ns': 3_000_000_000}]}))
        (output / 'clock-pairs.jsonl').write_text('\n'.join(json.dumps({
            'command': command, 'status': 0,
            'host_before_ns': instant - uncertainty_ns, 'host_after_ns': instant + uncertainty_ns,
            'guest_monotonic_ns': instant + offset_ns})
            for command, instant in ((3, 1_000_000_000), (4, 3_000_000_000))))
        lines = ['Window: Settings', '---PROFILEDATA---',
                 'Flags,IntendedVsync,SwapBuffers,FrameCompleted,']
        lines.extend(f'{flags},{start},{start},{end},' for flags, start, end in frames)
        (output / 'frames.txt').write_text('\n'.join([*lines, '---PROFILEDATA---', '']))
        return directory

    def test_long_frame_keeps_full_duration_and_fails_cli(self):
        directory = self.make_run([(0, 1_200_000_000, 1_210_000_000),
                                   (0, 1_300_000_000, 3_300_000_000)])
        report = analyze(directory)
        self.assertEqual(report['version'], 2)
        self.assertFalse(report['exported_frame_window_complete'])
        self.assertEqual(report['frame_counts'], {'accepted': 2})
        self.assertEqual(report['duration']['max_ms'], 2000)
        self.assertEqual(report['duration']['p99_ms'], 1980.1)
        self.assertEqual(report['duration']['above_ms']['1000'], 1)
        self.assertEqual(report['phase_duration']['swap_to_completed']['max_ms'], 2000)
        self.assertEqual(report['frames'][1]['window_boundary'], 'crosses_end')
        self.assertEqual(report['frames'][1]['completion_status'], 'completed')
        self.assertIn('frame crosses capture window boundary', report['failures'])
        output = directory / 'analysis.json'
        result = subprocess.run([sys.executable, '-B', str(Path(__file__).with_name('analyze_interaction_profile.py')),
                                 str(directory), '--output', str(output)], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(json.loads(output.read_text())['duration']['max_ms'], 2000)
        self.assertTrue(json.loads(result.stdout)['failures'])

    def test_unavailable_completion_fails_with_other_accepted_frames(self):
        for end, status in ((0, 'unreported'), (2**63 - 1, 'unreported'), (1_299_999_999, 'invalid')):
            with self.subTest(end=end):
                report = analyze(self.make_run([(0, 1_200_000_000, 1_210_000_000),
                                                (0, 1_300_000_000, end)]))
                frame = report['frames'][1]
                self.assertEqual(frame['completion_status'], status)
                self.assertEqual(frame['window_boundary'], 'completion_unavailable')
                self.assertEqual(frame['elapsed_to_window_end_ms'], 1700)
                self.assertNotIn('duration_ms', frame)
                self.assertEqual(report['duration']['count'], 1)
                self.assertFalse(report['exported_frame_window_complete'])
                self.assertIn('frame starting in capture window has no valid completion timestamp', report['failures'])

    def test_half_open_start_window_preserves_exact_completion_boundary(self):
        report = analyze(self.make_run([(0, 1_000_000_000, 1_010_000_000),
                                        (0, 2_900_000_000, 3_000_000_000),
                                        (0, 3_000_000_000, 3_010_000_000),
                                        (0, 900_000_000, 1_000_000_000)]))
        self.assertEqual(report['failures'], [])
        self.assertTrue(report['exported_frame_window_complete'])
        self.assertEqual(report['frame_counts'], {'accepted': 2, 'outside_start_window': 2})
        self.assertEqual(report['duration']['max_ms'], 100)

    def test_frame_started_before_window_retains_overlap_and_fails(self):
        report = analyze(self.make_run([(0, 900_000_000, 1_200_000_000),
                                        (0, 1_300_000_000, 1_310_000_000)]))
        self.assertEqual(report['frames'][0]['window_boundary'], 'crosses_start')
        self.assertEqual(report['frames'][0]['duration_ms'], 300)
        self.assertEqual(report['duration']['max_ms'], 10)
        self.assertFalse(report['exported_frame_window_complete'])

    def test_unavailable_completion_before_window_has_unknown_overlap(self):
        for end in (0, 2**63 - 1, 899_999_999):
            with self.subTest(end=end):
                report = analyze(self.make_run([(0, 900_000_000, end),
                                                (0, 1_300_000_000, 1_310_000_000)]))
                self.assertEqual(report['frames'][0]['window_boundary'], 'overlap_unknown')
                self.assertNotIn('duration_ms', report['frames'][0])
                self.assertEqual(report['duration']['count'], 1)
                self.assertFalse(report['exported_frame_window_complete'])
                self.assertIn('frame starting before capture window has unknown overlap', report['failures'])

    def test_uncertain_start_is_excluded_but_uncertain_completion_keeps_duration(self):
        report = analyze(self.make_run([(0, 1_000_000_000, 1_020_000_000),
                                        (0, 1_300_000_000, 3_000_000_000)], uncertainty_ns=1_000_000))
        self.assertEqual(report['frame_counts'], {'clock_uncertainty_boundary': 1, 'accepted': 1})
        self.assertEqual(report['duration']['max_ms'], 1700)
        self.assertTrue(all(frame['clock_boundary_uncertain'] for frame in report['frames']))
        self.assertFalse(report['exported_frame_window_complete'])
        self.assertIn('frame capture window boundary is uncertain', report['failures'])

    def test_duplicate_and_flagged_frames_do_not_change_quantiles(self):
        report = analyze(self.make_run([(0, 1_200_000_000, 1_210_000_000),
                                        (0, 1_200_000_000, 1_210_000_000),
                                        (1, 1_300_000_000, 2_900_000_000)]))
        self.assertEqual(report['failures'], [])
        self.assertEqual(report['frame_counts'], {'accepted': 1, 'duplicate': 1, 'nonzero_flags': 1})
        self.assertEqual(report['duration']['max_ms'], 10)

    def test_outside_frame_with_uncertain_overlap_fails_coverage(self):
        report = analyze(self.make_run([(0, 900_000_000, 999_500_000),
                                        (0, 3_000_500_000, 3_010_000_000),
                                        (0, 1_300_000_000, 1_310_000_000)], uncertainty_ns=1_000_000))
        self.assertEqual(report['frame_counts'], {'outside_start_window': 2, 'accepted': 1})
        self.assertTrue(all(frame['clock_boundary_uncertain'] for frame in report['frames'][:2]))
        self.assertFalse(report['exported_frame_window_complete'])

    def test_guest_clock_offset_applies_to_start_and_completion_boundaries(self):
        report = analyze(self.make_run([(0, 6_200_000_000, 6_210_000_000),
                                        (0, 6_300_000_000, 8_300_000_000)], offset_ns=5_000_000_000))
        self.assertEqual(report['guest_window_ns'], [6_000_000_000, 8_000_000_000])
        self.assertEqual(report['duration']['max_ms'], 2000)
        self.assertEqual(report['frames'][1]['window_boundary'], 'crosses_end')
        self.assertFalse(report['exported_frame_window_complete'])


if __name__ == '__main__':
    unittest.main()
