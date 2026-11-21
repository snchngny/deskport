#!/usr/bin/env python3
"""Exercise draft-only uploads against the actual managed run record layout."""
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('draft_staging', ROOT / 'scripts/stage-draft-release.py')
stage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stage)


class DraftStagingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.base = self.root / 'checkout'
        self.dist = self.base / 'runs.noindex/test/windows/dist'
        self.dist.mkdir(parents=True)
        self.asset = self.dist / 'DeskPort-test.exe'
        self.asset.write_bytes(b'package')
        self.record = self.base / 'records/test/windows/run.json'
        self.record.parent.mkdir(parents=True)
        self.data = {'target': 'windows', 'run': 'test', 'status': 'built', 'source_unchanged': True,
                     'failed': False, 'assets': [{'path': self.asset.name,
                     'sha256': hashlib.sha256(self.asset.read_bytes()).hexdigest()}]}
        self.record.write_text(json.dumps(self.data))
        self.argv = ['stage', '--repo', 'snchngny/deskport', '--tag', 'v2.9.0-imac.1',
                     '--root', str(self.root), '--target', 'windows']

    def tearDown(self):
        self.temp.cleanup()

    def test_only_verified_draft_assets_are_uploaded(self):
        with patch.object(sys, 'argv', self.argv), patch.object(stage.subprocess, 'check_output', return_value=b'{"isDraft":true}'), patch.object(stage.subprocess, 'run') as upload:
            stage.main()
            self.assertEqual(upload.call_args.args[0][-1], str(self.asset))

    def test_refuse_published_release(self):
        with patch.object(sys, 'argv', self.argv), patch.object(stage.subprocess, 'check_output', return_value=b'{"isDraft":false}'), patch.object(stage.subprocess, 'run') as upload:
            with self.assertRaisesRegex(SystemExit, 'draft'):
                stage.main()
            upload.assert_not_called()

    def test_refuse_changed_package(self):
        self.asset.write_bytes(b'changed')
        with patch.object(sys, 'argv', self.argv), patch.object(stage.subprocess, 'check_output', return_value=b'{"isDraft":true}'):
            with self.assertRaisesRegex(SystemExit, 'checksum'):
                stage.main()

    def test_refuse_failed_build_record(self):
        self.data['failed'] = True
        self.record.write_text(json.dumps(self.data))
        with patch.object(sys, 'argv', self.argv), patch.object(stage.subprocess, 'check_output', return_value=b'{"isDraft":true}'):
            with self.assertRaisesRegex(SystemExit, 'No verified'):
                stage.main()


if __name__ == '__main__':
    unittest.main()
