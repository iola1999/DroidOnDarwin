#!/usr/bin/env python3
"""检查帧记录缺失、格式错误和时钟采样误差的处理。"""
import unittest
from analyze_interaction_profile import read_frames, clock_alignment, distribution, read_frame_totals, read_thread_map


class InteractionProfileTests(unittest.TestCase):
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


if __name__ == '__main__':
    unittest.main()
