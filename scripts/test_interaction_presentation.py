#!/usr/bin/env python3
"""检查真实显示时间、晚到/缺失回调和不完整采集的解释。"""
from copy import deepcopy
import unittest
from interaction_presentation import analyze_presentation
from analyze_interaction_profile import distribution


def frame(start, identity, time=None, callback=None):
    row = {'layer': 'main', 'acquire_begin_ns': int(start * 1e9), 'acquire_end_ns': int((start + .01) * 1e9), 'drawable_id': identity}
    if callback is not None:
        row.update(callback_ns=int(callback * 1e9), presented_time_seconds=time)
    return row


class PresentationChecks(unittest.TestCase):
    def setUp(self):
        self.data = {'version': 1, 'capacity': 2048, 'overflow': 0, 'duplicate_callbacks': 0, 'clock_errors': 0,
                     'begin': {'host_before_ns': 1_000_000_000, 'media_seconds': 10, 'host_after_ns': 1_000_000_000},
                     'end': {'host_before_ns': 2_000_000_000, 'media_seconds': 11, 'host_after_ns': 2_000_000_000},
                     'frames': [frame(1.2, 1, 10.3, 1.9), frame(1.4, 2, 10.5, 1.6)]}

    def analyze(self, data=None):
        return analyze_presentation(data or self.data, 1_100_000_000, 1_900_000_000, distribution)

    def test_display_time_ignores_callback_delivery_order(self):
        result = self.analyze()
        self.assertEqual(result['failures'], [])
        self.assertEqual(result['counts']['presented_in_window'], 2)
        self.assertEqual(result['unique_presentation_interval_ms']['p50_ms'], 200)
        self.assertAlmostEqual(result['drawable_to_present_ms']['p50_ms'], 90)
        self.assertAlmostEqual(result['layers'][0]['first_present_offset_ms'], 200)
        self.assertAlmostEqual(result['layers'][0]['last_present_offset_ms'], 400)
        self.assertAlmostEqual(result['layers'][0]['trailing_without_present_ms'], 400)

    def test_pending_zero_nil_and_late_presentation_stay_separate(self):
        unavailable = frame(1.6, None)
        unavailable['drawable_unavailable'] = True
        self.data['frames'] += [frame(1.5, 3), frame(1.55, 4, 0, 1.7), unavailable, frame(1.7, 5, 10.95, 2.1)]
        result = self.analyze()
        self.assertEqual(result['failures'], [])
        for key in ('pending_callback', 'zero_presented_time', 'drawable_unavailable', 'outside_presentation_window'):
            self.assertEqual(result['counts'][key], 1)
        self.assertEqual(result['counts']['presented_in_window'], 2)

    def test_invalid_identity_and_time_fail(self):
        for change in ({'drawable_id': 1}, {'presented_time_seconds': 10.0}, {'presented_time_seconds': 11.0}, {'presented_time_seconds': float('nan')}):
            data = deepcopy(self.data)
            data['frames'][1].update(change)
            self.assertTrue(self.analyze(data)['failures'], change)

    def test_capture_errors_are_reported(self):
        for name in ('overflow', 'duplicate_callbacks', 'clock_errors'):
            data = deepcopy(self.data)
            data[name] = 1
            self.assertTrue(self.analyze(data)['failures'])
        data = deepcopy(self.data)
        data['end'] = None
        self.assertTrue(self.analyze(data)['failures'])


if __name__ == '__main__':
    unittest.main()
