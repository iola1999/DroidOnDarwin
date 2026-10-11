#!/usr/bin/env python3
"""检查设备状态查询的新旧响应与活动会话边界，不连接实际手机。"""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from urllib.parse import parse_qs, urlparse

import benchmark_dod


class TranslationParametersTests(unittest.TestCase):
    def test_partial_override_keeps_other_translation_choices(self):
        defaults = {'nterp': False, 'translation': {'codeWatch': True, 'crossPage': True, 'cacheMiB': 128}}
        result = benchmark_dod.merge_parameters(defaults, {'translation': {'codeWatch': False}})
        self.assertEqual(result['translation'], {'codeWatch': False, 'crossPage': True, 'cacheMiB': 128})
        self.assertTrue(defaults['translation']['codeWatch'])

    def test_unknown_and_wrong_type_do_not_reach_app(self):
        defaults = {'translation': {'cacheMiB': 128}}
        for requested in ({'translation': {'missing': True}}, {'translation': {'cacheMiB': True}}, {'translation': False}):
            with self.assertRaises(ValueError):
                benchmark_dod.merge_parameters(defaults, requested)
        self.assertFalse(benchmark_dod.parameter_values_valid({'translation': {'cacheMiB': '128'}}))
        self.assertFalse(benchmark_dod.parameter_values_valid({'nterp': {}}))
        self.assertTrue(benchmark_dod.parameter_values_valid({'nterp': True, 'translation': {'cacheMiB': 64}}))


class PhoneStatusTests(unittest.TestCase):
    def query(self, active=False, stale_only=False):
        request_id = None
        copies = 0

        def launch(args, url):
            nonlocal request_id
            request_id = parse_qs(urlparse(url).query)['requestID'][0]

        def copy(args, source, destination):
            nonlocal copies
            copies += 1
            value = {'requestID': 'old-request' if stale_only or copies == 1 else request_id,
                     'active': active, 'timestamp': 1, 'configuration_defaults': {'nterp': False}}
            destination.write_text(json.dumps(value))
            return True

        with tempfile.TemporaryDirectory() as directory, \
                patch.object(benchmark_dod, 'launch_phone', side_effect=launch) as launcher, \
                patch.object(benchmark_dod, 'copy_phone', side_effect=copy), \
                patch.object(benchmark_dod.time, 'sleep'):
            result = benchmark_dod.phone_idle(None, Path(directory))
            self.assertEqual(launcher.call_count, 1)
            self.assertEqual(copies, 2)
            return result

    def test_fresh_response_ignores_clock_difference_and_stale_file(self):
        self.assertEqual(self.query()['configuration_defaults'], {'nterp': False})

    def test_active_session_prevents_restart(self):
        with self.assertRaisesRegex(RuntimeError, '活动会话'):
            self.query(active=True)

    def test_stale_responses_do_not_authorize_restart(self):
        with self.assertRaisesRegex(RuntimeError, '无法读取新状态'):
            self.query(stale_only=True)


class MemoryMetricsTests(unittest.TestCase):
    def test_stage_and_process_peaks_remain_separate(self):
        stage = {'metrics': {'footprint': {'mean_bytes': 200, 'sampled_peak_bytes': 300, 'latest_bytes': 100}}}
        result = {'memory': {'enabled': True, 'overall': stage, 'phases': {'booting': stage},
                             'boot_steps': {'2': stage}, 'process_lifetime_peak_bytes': {'footprint': 900}}}
        metrics = benchmark_dod.measure(result)
        self.assertEqual(metrics['memory_phase_booting_footprint_mean_bytes'], 200)
        self.assertEqual(metrics['memory_boot_step_2_footprint_sampled_peak_bytes'], 300)
        self.assertEqual(metrics['memory_process_lifetime_footprint_peak_bytes'], 900)

    def test_disabled_or_missing_memory_does_not_create_zero_samples(self):
        self.assertEqual(benchmark_dod.measure({'memory': {'enabled': False}}), {})
        self.assertEqual(benchmark_dod.measure({}), {})


if __name__ == '__main__':
    unittest.main()
