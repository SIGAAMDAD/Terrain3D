#!/usr/bin/env python3
"""Deterministic fixture for the F00/T00 capture reporter (not measured performance)."""
import unittest
from streaming_baseline_report import summarize, percentile


class BaselineTests(unittest.TestCase):
    def test_percentiles(self):
        self.assertEqual(percentile([30, 10, 20], .5), 20)
        self.assertEqual(percentile([30, 10], .95), 29.0)
        self.assertIsNone(percentile([], .5))

    def test_real_capture_schema_and_missing_metrics(self):
        captured = {'schema': 't3d_streaming_baseline', 'scene_id': 'res://fixture.tscn',
                    'scenario': 'flythrough', 'settings': {'viewport': [1920, 1080], 'vt_upload_budget': 8},
                    'samples': [{'sequence': 1, 'gpu_missing_required': 3, 'clipmap_usec': 100},
                                {'sequence': 2, 'gpu_missing_required': 1, 'clipmap_usec': 200}]}
        report = summarize(captured)
        self.assertEqual(report['metrics']['gpu_missing_required']['total'], 4)
        self.assertEqual(report['metrics']['clipmap_usec']['p95'], 195)
        self.assertIsNone(report['metrics']['instance_load_bytes'])
        self.assertIn('gpu_frame_time_usec', report['missing_external_metrics'])
        captured['samples'][1]['sequence'] = 2
        captured['vt_samples'] = [
            {'sequence': 1, 'vt_page_misses': 10, 'vt_physical_resident': 90},
            {'sequence': 2, 'vt_page_misses': 13, 'vt_physical_resident': 95},
        ]
        report = summarize(captured)
        self.assertEqual(report['vt_metrics']['vt_page_misses']['delta'], 3)
        self.assertIsNone(report['vt_metrics']['vt_physical_resident']['delta'])
        captured['samples'][1]['sequence'] = 1
        with self.assertRaises(ValueError):
            summarize(captured)


if __name__ == '__main__':
    unittest.main()
