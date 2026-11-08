"""Run provenance and failed-preparation handling, without launching native code."""
from paths import ROOT
import contextlib
import importlib.util
import io
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

spec = importlib.util.spec_from_file_location('sotc_run_metadata', ROOT / 'sotc.py')
sotc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sotc)


class RunMetadataTests(unittest.TestCase):
    def test_resolved_overrides_and_inputs_are_identified_without_unrelated_environment(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            probe = root / 'probe.exe'
            probe.write_bytes(b'synthetic test executable')
            shipped = root / 'manifest'
            shipped.mkdir()
            (shipped / 'sotc-pipelines.tsv').write_text('H\ttest\n')
            env = {'SOTC_DRIVER_CACHE': '1', 'SOTC_GPL_PREDICT': '0',
                   'SOTC_PIPE_MANIFEST_DIR': str(shipped), 'BB_PRESENT_MODE': 'fifo',
                   'PATH': 'unrelated path', 'PRIVATE_TOKEN': 'must not be recorded'}
            with patch.object(sotc, 'display_drivers', return_value='test driver'), contextlib.redirect_stdout(io.StringIO()):
                path = sotc.write_run_metadata(env, root / 'user', probe, 'test-image-key')
            data = json.loads(path.read_text(encoding='utf-8'))
            self.assertEqual(data['settings']['SOTC_GPL_PREDICT'], '0')
            self.assertNotIn('PATH', data['settings'])
            self.assertNotIn('PRIVATE_TOKEN', path.read_text())
            self.assertEqual(data['probe']['size'], probe.stat().st_size)
            self.assertEqual(data['image_fingerprint'], 'test-image-key')
            self.assertEqual(len(data['inputs']['shipped_manifest']['sha256']), 64)
            self.assertEqual(data['inputs']['user_manifest']['unavailable'], 'FileNotFoundError')

    def test_metadata_io_failure_does_not_stop_launch(self):
        with tempfile.TemporaryDirectory() as tmp:
            with patch.object(sotc, 'display_drivers', return_value='test'), \
                 patch.object(Path, 'write_text', side_effect=PermissionError('test')), \
                 contextlib.redirect_stderr(io.StringIO()):
                self.assertIsNone(sotc.write_run_metadata({}, Path(tmp), Path(tmp) / 'missing', 'image'))

    def test_native_preparation_failure_cannot_publish_stamp_even_with_done_line(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            probe = root / 'probe.exe'
            probe.write_bytes(b'test')
            process = Mock(stdout=io.BytesIO(b'SOTC PRECOMPILE done: test\n'))
            process.wait.return_value = 1
            with patch.object(sotc, 'display_drivers', return_value='test'), \
                 patch.object(sotc.subprocess, 'Popen', return_value=process), \
                 contextlib.redirect_stdout(io.StringIO()):
                ok = sotc.prepare_shaders({}, root / 'GAME', root / 'user', 'image', probe)
            self.assertFalse(ok)
            self.assertFalse((root / 'user' / 'cache' / sotc.PREPARE_STAMP).exists())


if __name__ == '__main__':
    unittest.main()
