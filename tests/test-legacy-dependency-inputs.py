#!/usr/bin/env python3
"""Verify integrity checks for reusable legacy build inputs without network access."""
import hashlib
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('legacy_dependencies', ROOT / 'scripts/build-macos-legacy-deps.py')
deps = importlib.util.module_from_spec(spec)
spec.loader.exec_module(deps)


class DependencyInputs(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.data = b'upstream-source'
        self.entry = {'name': 'fixture', 'file': 'source.tar.gz', 'bytes': len(self.data),
                      'sha256': hashlib.sha256(self.data).hexdigest(), 'urls': ['https://example.invalid/source']}

    def tearDown(self):
        self.temp.cleanup()

    def test_verify_download_then_reuse_without_network(self):
        with patch.object(deps.urllib.request, 'urlopen', return_value=io.BytesIO(self.data)):
            path = deps.fetch(self.entry, self.root)
        with patch.object(deps.urllib.request, 'urlopen', side_effect=AssertionError('network access')):
            self.assertEqual(deps.fetch(self.entry, self.root), path)

    def test_reject_corrupt_download(self):
        with patch.object(deps.urllib.request, 'urlopen', return_value=io.BytesIO(b'corrupt')):
            with self.assertRaisesRegex(RuntimeError, 'checksum mismatch'):
                deps.fetch(self.entry, self.root)
        self.assertFalse((self.root / self.entry['file']).exists())
        self.assertEqual(list(self.root.iterdir()), [])

    def test_reject_existing_changed_cache_without_overwrite(self):
        path = self.root / self.entry['file']
        path.write_bytes(b'existing')
        with self.assertRaisesRegex(RuntimeError, 'cache checksum mismatch'):
            deps.fetch(self.entry, self.root)
        self.assertEqual(path.read_bytes(), b'existing')


if __name__ == '__main__':
    unittest.main()
