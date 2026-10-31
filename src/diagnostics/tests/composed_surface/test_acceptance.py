#!/usr/bin/env python3
"""Synthetic parser tests; these do not represent GPU measurements."""
import csv
import tempfile
import unittest
from pathlib import Path
from analyze_composed_surface_acceptance import compare, read_frames


class AcceptanceTests(unittest.TestCase):
    def test_structural_requirements_and_gates(self):
        headings = ['case', 'resolution', 'warm_state', 'shader_path',
                    'frame_gpu_usec', 'terrain_depth_usec', 'terrain_dnr_usec',
                    'terrain_color_usec', 'visibility_usec', 'transfer_usec',
                    'upload_bytes', 'vt_misses']
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'fixture.csv'
            with path.open('w', newline='', encoding='utf-8') as file:
                writer = csv.DictWriter(file, fieldnames=headings)
                writer.writeheader()
                for mode, cost in [('canonical_vt_v1', 100), ('composed_surface', 60)]:
                    for _ in range(31):
                        writer.writerow(dict(case='stationary_horizon', resolution='full',
                                             warm_state='warm', shader_path=mode,
                                             frame_gpu_usec=cost + 10,
                                             terrain_color_usec=cost // 2,
                                             terrain_dnr_usec=cost // 2,
                                             terrain_depth_usec=1, visibility_usec=2,
                                             transfer_usec=1, upload_bytes=0, vt_misses=0))
            data = read_frames(path)
            reports = compare(data)
            self.assertEqual(reports[0]['decision'], 'PASS_TIMING_GATES_ONLY')
            self.assertEqual(reports[0]['candidate_material']['p50'], 60)
            reports = compare(data, min_frames=33)
            self.assertEqual(reports[0]['decision'], 'INSUFFICIENT_MEASUREMENTS')
            path.write_text(path.read_text().replace(',30,', ',-30,'), encoding='utf-8')
            with self.assertRaises(ValueError):
                read_frames(path)

    def test_unmatched_and_missing_timing(self):
        groups = {(('horizon', 'full', 'warm'), 'canonical_vt_v1'):
                  [{'frame_gpu_usec': 10.0}]}
        self.assertEqual(compare(groups)[0]['decision'], 'NO_MATCHED_CAPTURE')
        groups[(('horizon', 'full', 'warm'), 'composed_surface')] = [{'frame_gpu_usec': 5.0}]
        self.assertEqual(compare(groups)[0]['decision'], 'INSUFFICIENT_MEASUREMENTS')


if __name__ == '__main__':
    unittest.main()
