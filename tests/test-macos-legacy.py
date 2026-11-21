#!/usr/bin/env python3
"""Portable checks for legacy capture selection and the packaged binary gate."""
import importlib.util
from pathlib import Path
import struct
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


audit = load('macos_target_audit', 'scripts/check-macos-target.py')
overlay = load('legacy_capture_overlay', 'host/overlays/patch-host-legacy-macos.py')


def macho(cpu=0x01000007, minimum=0x000b0000):
    return struct.pack('<IIIIIIII', 0xfeedfacf, cpu, 3, 2, 1, 24, 0, 0) + struct.pack('<IIIIII', 0x32, 24, 1, minimum, 0, 0)


class LegacyMacTests(unittest.TestCase):
    def test_native_big_sur_and_wrong_architecture(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'program'
            path.write_bytes(macho())
            self.assertEqual(audit.check(path, 'x86_64', (11, 7, 11)), '11.0.0')
            with self.assertRaisesRegex(ValueError, 'missing'):
                audit.check(path, 'arm64', (11, 7, 11))

    def test_reject_newer_runtime(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'dependency'
            path.write_bytes(macho(minimum=0x000c0000))
            with self.assertRaisesRegex(ValueError, 'requires macOS 12'):
                audit.check(path, 'x86_64', (11, 7, 11))

    def test_universal_selects_intel_not_arm_runtime(self):
        intel, arm = macho(), macho(0x0100000c, 0x001a0000)
        data = struct.pack('>II', 0xcafebabe, 2)
        data += struct.pack('>IIIII', 0x01000007, 3, 48, len(intel), 0)
        data += struct.pack('>IIIII', 0x0100000c, 0, 48 + len(intel), len(arm), 0)
        data += intel + arm
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'universal'
            path.write_bytes(data)
            self.assertEqual(audit.check(path, 'x86_64', (11, 7, 11)), '11.0.0')
            with self.assertRaisesRegex(ValueError, 'requires macOS 26'):
                audit.check(path, 'arm64', (11, 7, 11))

    def test_reject_truncated_load_commands(self):
        data = macho()[:-1]
        with self.assertRaisesRegex(ValueError, 'load commands'):
            audit.deployment(data, 0x01000007)

    def test_real_vendored_cmake_and_overlay_order(self):
        # Exercise the actual pinned host CMake file and actual DeskPort patch.
        import subprocess
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory)
            target = source / 'cmake/compile_definitions/macos.cmake'
            target.parent.mkdir(parents=True)
            with tarfile.open(ROOT / 'host/vendor/sunshine.tar.gz') as archive:
                target.write_bytes(archive.extractfile('cmake/compile_definitions/macos.cmake').read())
            patch = ROOT / 'host/macos/patches/sunshine-screen-capture-kit.patch'
            patched = subprocess.run(['git', '-c', 'core.autocrlf=false', 'apply', '--include=cmake/compile_definitions/macos.cmake', '-'], input=patch.read_text().encode(), cwd=source, capture_output=True)
            self.assertEqual(patched.returncode, 0, patched.stderr.decode())
            overlay.apply(source, ROOT)
            first = target.read_text()
            self.assertNotIn('framework ScreenCaptureKit', first)
            self.assertNotIn('/color-conversion.m', first)
            self.assertIn('/legacy-screen-video.m', first)
            self.assertIn('-fno-objc-arc', first)
            overlay.apply(source, ROOT)
            self.assertEqual(target.read_text(), first)
            self.assertTrue((source / 'src/deskport/macos/legacy-screen-video.m').is_file())


if __name__ == '__main__':
    unittest.main()
