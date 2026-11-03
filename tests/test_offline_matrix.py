"""Tests for tools/offline_matrix.py: matrix validation, run environment and validity, CSV reading, and the
aggregation (medians over repeats, statistics over frames, noise, deltas, image stability) on synthetic files."""
from paths import ROOT  # noqa: F401  (puts scripts/ on sys.path)
import csv
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(ROOT / 'tools'))
import offline_matrix as om  # noqa: E402

BLANK_HASH = '0' * 16


def write_csv(path, header, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, 'w', newline='', encoding='utf-8') as handle:
        writer = csv.writer(handle)
        writer.writerow(header)
        writer.writerows(rows)


def make_run(out, config, rnd, wall, gpu=5.0, hashes=None, warmup=False, valid=True, reason=''):
    """Writes one run folder (metrics.csv, hashes.csv) and returns its manifest record.
    wall: {frame: wall_ms}; hashes: {frame: (exact, phash)} or None for no hash file."""
    rel = Path('runs') / config / f'r{rnd}'
    folder = out / rel
    write_csv(folder / 'metrics.csv', ['frame', 'wall_ms', 'gpu_ms', 'draw_ops'],
              [[f, wall[f], gpu, 100] for f in sorted(wall)])
    if hashes is not None:
        write_csv(folder / 'hashes.csv', ['frame', 'exact', 'phash'],
                  [[f, hashes[f][0], hashes[f][1]] for f in sorted(hashes)])
    return {'config': config, 'round': rnd, 'warmup': warmup, 'dir': rel.as_posix(), 'valid': valid,
            'reason': reason, 'exit_code': 0, 'timed_out': False, 'seconds': 1.0}


def spec_for(frames=None, repeats=2, warmup=1):
    return om.parse_matrix({'configs': {'base': {}, 'flag': {'FLAG': '1'}}, 'repeats': repeats,
                            'warmup': warmup, **({'frames': frames} if frames else {})})


class MatrixFileTests(unittest.TestCase):
    def test_configs_keep_order_and_values_become_strings(self):
        spec = om.parse_matrix({'base': {'A': 1}, 'configs': {'first': {}, 'second': {'B': 2}}})
        self.assertEqual(list(spec['configs']), ['first', 'second'])
        self.assertEqual(spec['base'], {'A': '1'})
        self.assertEqual(spec['configs']['second'], {'B': '2'})
        self.assertEqual((spec['repeats'], spec['warmup']), (3, 1))

    def test_rejects_bad_files(self):
        bad = [
            {'configs': {}},
            {'configs': {'a': {}}, 'repeats': 0},
            {'configs': {'a': {}}, 'warmup': -1},
            {'configs': {'a': {}}, 'frames': [10, 5]},
            {'configs': {'a': {}}, 'frames': [1]},
            {'configs': {'a': {'SOTC_FRAME_HASH': 'x'}}},
            {'base': {'SOTC_DETERMINISTIC': '0'}, 'configs': {'a': {}}},
            {'configs': {'a': {}}, 'timeout_s': 0},
        ]
        for data in bad:
            with self.subTest(data=data), self.assertRaises(ValueError):
                om.parse_matrix(data)

    def test_frame_window_is_inclusive_tuple(self):
        self.assertEqual(om.parse_matrix({'configs': {'a': {}}, 'frames': [2, 4]})['frames'], (2, 4))


class RunEnvironmentTests(unittest.TestCase):
    def test_caller_sotc_and_bb_variables_are_dropped_and_forced_keys_set(self):
        inherited = {'PATH': 'p', 'SOTC_LAB': '0', 'BB_VBLANK_HZ': '60', 'KEEP_ME': 'yes'}
        with patch.dict(os.environ, inherited, clear=True):
            env = om.run_environment({'SOTC_LAB': '1', 'X': '2'}, Path('/run'))
        self.assertEqual(env['PATH'], 'p')
        self.assertEqual(env['KEEP_ME'], 'yes')
        self.assertEqual(env['SOTC_LAB'], '1')  # from the matrix, not the caller
        self.assertNotIn('BB_VBLANK_HZ', env)
        self.assertEqual(env['SOTC_DETERMINISTIC'], '1')
        self.assertTrue(env['SOTC_FRAME_METRICS'].endswith('metrics.csv'))
        self.assertTrue(env['SOTC_FRAME_HASH'].endswith('hash'))


class RunnerLabelTests(unittest.TestCase):
    def test_labels_count_warmup_and_repeats_and_strip_the_prefix(self):
        spec = om.parse_matrix({'configs': {'base': {}, 'flag': {'SOTC_FLAG': '1', 'BB_X': '2'}},
                                'repeats': 2, 'warmup': 1})
        self.assertEqual(om.run_labels(spec, 'flag', 0, 2, 6), {
            'SOTC_RUN_LABEL': 'flag warmup', 'SOTC_RUN_INDEX': '2/6', 'SOTC_RUN_FLAGS': 'FLAG=1 BB_X=2'})
        self.assertEqual(om.run_labels(spec, 'flag', 1, 4, 6)['SOTC_RUN_LABEL'], 'flag r1/2')
        self.assertEqual(om.run_labels(spec, 'base', 2, 5, 6)['SOTC_RUN_LABEL'], 'base r2/2')
        self.assertEqual(om.run_labels(spec, 'base', 2, 5, 6)['SOTC_RUN_FLAGS'], '')

    def test_flags_text_is_cut_to_120_characters(self):
        long_value = 'x' * 300
        spec = om.parse_matrix({'configs': {'a': {'SOTC_LONG': long_value}}})
        flags = om.run_labels(spec, 'a', 1, 1, 1)['SOTC_RUN_FLAGS']
        self.assertEqual(len(flags), 120)
        self.assertTrue(flags.startswith('LONG=xxx'))

    def test_exit_frames_from_window_or_explicit_value(self):
        self.assertEqual(om.exit_frames_for(spec_for(frames=[600, 1800])), 1800 + om.EXIT_MARGIN_FRAMES)
        self.assertIsNone(om.exit_frames_for(spec_for()))
        explicit = om.parse_matrix({'configs': {'a': {}}, 'frames': [600, 1800], 'exit_frames': 500})
        self.assertEqual(om.exit_frames_for(explicit), 500)

    def test_window_and_exit_defaults_yield_to_the_configuration(self):
        self.assertEqual(om.runner_defaults({}), {'SOTC_HEADLESS': '2'})
        self.assertEqual(om.runner_defaults({}, exit_frames=900, hidden=True),
                         {'SOTC_HEADLESS': '1', 'SOTC_DET_EXIT_FRAMES': '900'})
        self.assertEqual(om.runner_defaults({'SOTC_HEADLESS': '1'}, exit_frames=900),
                         {'SOTC_DET_EXIT_FRAMES': '900'})

    def test_run_environment_carries_labels_and_window(self):
        labels = {'SOTC_RUN_LABEL': 'flag r1/2', 'SOTC_RUN_INDEX': '3/6', 'SOTC_RUN_FLAGS': 'FLAG=1'}
        with patch.dict(os.environ, {'SOTC_RUN_LABEL': 'stale', 'PATH': 'p'}, clear=True):
            env = om.run_environment({'FLAG': '1'}, Path('/run'), labels, 1920, hidden=False)
        self.assertEqual(env['SOTC_RUN_LABEL'], 'flag r1/2')
        self.assertEqual(env['SOTC_RUN_INDEX'], '3/6')
        self.assertEqual(env['SOTC_HEADLESS'], '2')
        self.assertEqual(env['SOTC_DET_EXIT_FRAMES'], '1920')
        self.assertEqual(env['PATH'], 'p')

    def test_exit_frames_must_be_a_positive_integer(self):
        for value in (0, -5, 1.5, True):
            with self.subTest(value=value), self.assertRaises(ValueError):
                om.parse_matrix({'configs': {'a': {}}, 'exit_frames': value})


class RunValidityTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.folder = Path(self.tmp.name) / 'run'
        self.folder.mkdir()

    def tearDown(self):
        self.tmp.cleanup()

    def test_reasons(self):
        self.assertEqual(om.check_run(self.folder, {}, True), 'timeout')
        self.assertEqual(om.check_run(self.folder, {}, False), 'no metrics.csv')
        write_csv(self.folder / 'metrics.csv', ['frame', 'wall_ms'], [])
        self.assertEqual(om.check_run(self.folder, {}, False), 'metrics.csv has no frame rows')
        write_csv(self.folder / 'metrics.csv', ['frame', 'wall_ms'], [[0, 1.0]])
        self.assertIsNone(om.check_run(self.folder, {}, False))

    def test_route_must_have_finished(self):
        write_csv(self.folder / 'metrics.csv', ['frame', 'wall_ms'], [[0, 1.0]])
        merged = {'SOTC_LAB_ROUTE': 'user/routes/tests.json'}
        self.assertIn('route did not run', om.check_run(self.folder, merged, False))
        (self.folder / 'last_run.log').write_text('start\nSOTC LAB ROUTE done: 10 s\n', encoding='utf-8')
        self.assertIsNone(om.check_run(self.folder, merged, False))


class CsvReadingTests(unittest.TestCase):
    def test_metrics_skip_rows_without_frame_and_keep_numeric_cells(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'metrics.csv'
            write_csv(path, ['frame', 'wall_ms', 'gpu_ms'], [[0, 1.5, ''], ['', 2, 3], [2, 'n/a', 4]])
            rows = om.read_metrics(path)
        self.assertEqual(rows, {0: {'wall_ms': 1.5}, 2: {'gpu_ms': 4.0}})

    def test_phash_distance(self):
        self.assertEqual(om.phash_distance('ff', '00'), 8)
        self.assertEqual(om.phash_distance('0x0f', '0x00'), 4)
        self.assertIsNone(om.phash_distance('zz', '00'))
        self.assertIsNone(om.phash_distance('ff', '0000'))
        self.assertIsNone(om.phash_distance('', ''))

    def test_percentile99_nearest_rank(self):
        self.assertEqual(om.percentile99(list(range(1, 101))), 99)
        self.assertEqual(om.percentile99([3.0, 1.0, 2.0]), 3.0)
        self.assertEqual(om.percentile99([7.0]), 7.0)


class AggregationTests(unittest.TestCase):
    """Two configurations, two repeats each, frames 0..9.
    base: wall = 10 + f (repeat 0) and 11 + f (repeat 1); gpu 5 everywhere.
    flag: wall = 12 + f in both repeats (no spread).
    Images: base is nondeterministic at frame 3 (repeat 1 differs); flag equals base except frame 5 (a real
    change, pHash distance 2) and frame 3 (stable in flag, unstable in base)."""

    def build(self, frames=None, extra=()):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        out = Path(tmp.name)
        frame_ids = range(10)
        base_hashes0 = {f: (f'b{f}', BLANK_HASH) for f in frame_ids}
        base_hashes1 = dict(base_hashes0)
        base_hashes1[3] = ('other', BLANK_HASH)
        flag_hashes = {f: (f'b{f}', BLANK_HASH) for f in frame_ids}
        flag_hashes[5] = ('c5', '0' * 15 + '3')
        manifest = [
            make_run(out, 'base', 0, {f: 10.0 + f for f in frame_ids}, warmup=True, hashes=base_hashes0),
            make_run(out, 'flag', 0, {f: 99.0 for f in frame_ids}, warmup=True),
            make_run(out, 'base', 1, {f: 10.0 + f for f in frame_ids}, hashes=base_hashes0),
            make_run(out, 'flag', 1, {f: 12.0 + f for f in frame_ids}, hashes=flag_hashes),
            make_run(out, 'base', 2, {f: 11.0 + f for f in frame_ids}, hashes=base_hashes1),
            make_run(out, 'flag', 2, {f: 12.0 + f for f in frame_ids}, hashes=flag_hashes),
            make_run(out, 'flag', 3, {0: 1.0}, valid=False, reason='timeout'),
        ] + list(extra)
        by_config, invalid = om.load_runs(manifest, out)
        spec = spec_for(frames=frames)
        return om.aggregate(by_config, spec), invalid, by_config

    def test_warmup_and_invalid_runs_are_excluded(self):
        result, invalid, by_config = self.build()
        self.assertEqual(result['repeats'], {'base': 2, 'flag': 2})
        self.assertEqual([r['reason'] for r in invalid], ['timeout'])
        self.assertEqual(result['frames']['count'], 10)

    def test_median_over_repeats_then_mean_and_p99(self):
        result, _, _ = self.build()
        base, flag = result['timing']['base'], result['timing']['flag']
        # base medians per frame: 10.5 + f -> mean 15.0, p99 19.5
        self.assertAlmostEqual(base['wall_ms']['mean']['value'], 15.0)
        self.assertAlmostEqual(base['wall_ms']['p99']['value'], 19.5)
        # flag: 12 + f -> mean 16.5, p99 21.0
        self.assertAlmostEqual(flag['wall_ms']['mean']['value'], 16.5)
        self.assertAlmostEqual(flag['wall_ms']['p99']['value'], 21.0)

    def test_noise_is_min_max_spread_of_repeats(self):
        result, _, _ = self.build()
        # base repeats: means 14.5 and 15.5 -> spread 1.0; flag has identical repeats -> spread 0
        self.assertAlmostEqual(result['timing']['base']['wall_ms']['mean']['spread'], 1.0)
        self.assertAlmostEqual(result['timing']['flag']['wall_ms']['mean']['spread'], 0.0)

    def test_delta_against_reference_and_noise_flag(self):
        result, _, _ = self.build()
        rows = {(r['metric'], r['stat']): r for r in result['deltas']['flag']}
        wall = rows[('wall_ms', 'mean')]
        self.assertAlmostEqual(wall['delta'], 1.5)
        self.assertAlmostEqual(wall['delta_pct'], 10.0)
        self.assertAlmostEqual(wall['noise'], 1.0)
        self.assertTrue(wall['above_noise'])
        gpu = rows[('gpu_ms', 'mean')]
        self.assertAlmostEqual(gpu['delta'], 0.0)
        self.assertFalse(gpu['above_noise'])

    def test_image_nondeterminism_and_real_changes(self):
        result, _, _ = self.build()
        self.assertEqual(result['images']['base']['nondeterministic'], [3])
        self.assertEqual(result['images']['flag']['nondeterministic'], [])
        vs = result['image_vs_base']['flag']
        self.assertEqual(vs['changed'], [{'frame': 5, 'phash_distance': 2}])
        self.assertEqual(vs['unstable'], [3])
        self.assertEqual(vs['same'], 8)
        self.assertEqual(vs['max_phash_distance'], 2)

    def test_window_restricts_frames(self):
        result, _, _ = self.build(frames=[2, 4])
        self.assertEqual((result['frames']['first'], result['frames']['last'], result['frames']['count']),
                         (2, 4, 3))
        # flag = 12 + f over frames 2, 3, 4
        self.assertAlmostEqual(result['timing']['flag']['wall_ms']['mean']['value'], 15.0)

    def test_single_repeat_has_no_noise(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        out = Path(tmp.name)
        manifest = [make_run(out, 'base', 0, {0: 1.0, 1: 2.0}), make_run(out, 'flag', 0, {0: 2.0, 1: 2.0})]
        by_config, _ = om.load_runs(manifest, out)
        result = om.aggregate(by_config, spec_for(repeats=1, warmup=0))
        row = next(r for r in result['deltas']['flag'] if r['metric'] == 'wall_ms' and r['stat'] == 'mean')
        self.assertIsNone(row['noise'])
        self.assertIsNone(row['above_noise'])
        self.assertIsNone(result['images']['base'])

    def test_config_without_valid_run_is_an_error(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        out = Path(tmp.name)
        manifest = [make_run(out, 'base', 0, {0: 1.0})]
        by_config, _ = om.load_runs(manifest, out)
        with self.assertRaises(ValueError):
            om.aggregate(by_config, spec_for(repeats=1, warmup=0))

    def test_markdown_table_and_marks(self):
        result, invalid, _ = self.build()
        text = om.render_markdown(result, invalid)
        self.assertIn('| metric | stat | `base` (ref) | `flag` |', text)
        self.assertIn('| wall_ms | mean | 15.00 ±1.00 | 16.50 ±0.000 |', text)
        self.assertIn('| wall_ms | mean | +1.500 (+10.0%) ±1.00 * |', text)
        self.assertIn('## Images', text)
        self.assertIn('changed frames vs `base`: 5 (2)', text)
        self.assertIn('## Invalid runs', text)

    def test_json_round_trip(self):
        result, invalid, _ = self.build()
        loaded = json.loads(json.dumps({'result': result, 'invalid_runs': invalid}, default=str))
        self.assertEqual(loaded['result']['base'], 'base')


if __name__ == '__main__':
    unittest.main()
