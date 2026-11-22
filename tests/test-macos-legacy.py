#!/usr/bin/env python3
"""Portable checks for legacy capture selection and the packaged binary gate."""
import importlib.util
from pathlib import Path
import struct
import tarfile
import tempfile
import unittest
import ast
import os
import shutil
import socket
import re
from unittest.mock import patch

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
    @unittest.skipUnless(os.name == 'posix' and hasattr(socket, 'AF_UNIX'), 'Native Unix socket regression')
    def test_session_endpoints_bind_under_normal_macos_temp_path(self):
        # /private/var/folders/.../T is 56 bytes on the affected Big Sur host.
        # Exercise the kernel bind limit, including the old failing endpoint.
        text = (ROOT / 'app/backend/multisessions.cpp').read_text()
        prefixes = re.findall(r'm_(?:Video)?Server\.listen\(QStringLiteral\("([^"]+)"\)', text)
        self.assertEqual(len(prefixes), 2)
        with tempfile.TemporaryDirectory(prefix='dp-', dir='/tmp') as directory:
            root = Path(directory) / ('x' * (56 - len(os.fsencode(directory)) - 1))
            root.mkdir()
            with socket.socket(socket.AF_UNIX) as endpoint:
                with self.assertRaises((OSError, ValueError)):
                    endpoint.bind(str(root / ('deskport-sessions-' + '1' * 36)))
            for prefix in prefixes:
                with socket.socket(socket.AF_UNIX) as endpoint:
                    endpoint.bind(str(root / (prefix + '1' * 36)))

    def test_deployed_qml_plugin_resolves_selected_qt_sdk(self):
        tree = ast.parse((ROOT / 'scripts/fix-macos-dependencies.py').read_text())
        function = next(node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == 'resolve')
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            suffix = 'QtQuickShapes.framework/Versions/A/QtQuickShapes'
            sdk = root / 'selected-sdk/lib'
            source = sdk / suffix
            source.parent.mkdir(parents=True)
            source.write_bytes(b'qt-framework')
            namespace = dict(Path=Path, os=os, origins={}, app=root / 'DeskPort.app',
                             frameworks=root / 'Frameworks', loader_rpaths=lambda unused: [])
            exec(compile(ast.Module(body=[function], type_ignores=[]), '<production-resolver>', 'exec'), namespace)
            with patch.dict(os.environ, {'DESKPORT_QT_LIBRARY_PATH': str(sdk)}):
                self.assertEqual(namespace['resolve']('@rpath/' + suffix, root / 'plugin.dylib'), source.resolve())
            with patch.dict(os.environ, {'DESKPORT_QT_LIBRARY_PATH': ''}):
                with self.assertRaisesRegex(RuntimeError, 'Cannot resolve'):
                    namespace['resolve']('@rpath/' + suffix, root / 'plugin.dylib')

    def test_host_rpath_uses_verified_prefix_without_overriding_loader(self):
        tree = ast.parse((ROOT / 'scripts/fix-macos-dependencies.py').read_text())
        function = next(node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == 'resolve')
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prefix = root / 'verified/lib'
            prefix.mkdir(parents=True)
            dependency = prefix / 'libdatachannel.0.dylib'
            dependency.write_bytes(b'linked-library')
            namespace = dict(Path=Path, os=os, origins={}, app=root / 'DeskPort.app',
                             frameworks=root / 'Frameworks', loader_rpaths=lambda unused: [])
            exec(compile(ast.Module(body=[function], type_ignores=[]), '<production-resolver>', 'exec'), namespace)
            with patch.dict(os.environ, {'DESKPORT_DEPENDENCY_LIBRARY_PATH': str(prefix)}):
                self.assertEqual(namespace['resolve']('@rpath/' + dependency.name, root / 'Sunshine'), dependency.resolve())
                original = root / 'original'
                original.mkdir()
                (original / dependency.name).write_bytes(b'original-loader-library')
                namespace['loader_rpaths'] = lambda unused: [str(original)]
                self.assertEqual(namespace['resolve']('@rpath/' + dependency.name, root / 'Sunshine'), (original / dependency.name).resolve())

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
                (source / 'CMakeLists.txt').write_bytes(archive.extractfile('CMakeLists.txt').read())
                logging = source / 'src/logging.h'
                logging.parent.mkdir(parents=True)
                logging.write_bytes(archive.extractfile('src/logging.h').read())
                display_sources = []
                for name in ('common/types.cpp', 'common/logging.cpp', 'macos/mac_api_layer.cpp'):
                    relative = 'third-party/libdisplaydevice/src/' + name
                    display = source / relative
                    display.parent.mkdir(parents=True, exist_ok=True)
                    display.write_bytes(archive.extractfile(relative).read())
                    display_sources.append(display)
                opus_module = source / 'cmake/dependencies/FindOpus.cmake'
                opus_module.parent.mkdir(parents=True)
                opus_module.write_bytes(archive.extractfile('cmake/dependencies/FindOpus.cmake').read())
            patch = ROOT / 'host/macos/patches/sunshine-screen-capture-kit.patch'
            patched = subprocess.run(['git', '-c', 'core.autocrlf=false', 'apply', '--include=cmake/compile_definitions/macos.cmake', '-'], input=patch.read_text().encode(), cwd=source, capture_output=True)
            self.assertEqual(patched.returncode, 0, patched.stderr.decode())
            overlay.apply(source, ROOT)
            first = target.read_text()
            mac_api = source / 'third-party/libdisplaydevice/src/macos/mac_api_layer.cpp'
            self.assertNotIn('IOServiceGetMatchingServices(kIOMainPortDefault,', mac_api.read_text())
            self.assertIn('IOServiceGetMatchingServices(kIOMasterPortDefault,', mac_api.read_text())
            self.assertNotIn('framework ScreenCaptureKit', first)
            self.assertNotIn('/color-conversion.m', first)
            self.assertIn('/legacy-screen-video.m', first)
            self.assertIn('-fno-objc-arc', first)
            self.assertIn('fmt::format', logging.read_text())
            self.assertNotIn('std::format', logging.read_text())
            for display in display_sources:
                self.assertNotIn('#include <format>', display.read_text())
                self.assertNotIn('std::format', display.read_text())
                self.assertIn('fmt::', display.read_text())
            overlay.apply(source, ROOT)
            self.assertEqual(target.read_text(), first)
            self.assertEqual((source / 'CMakeLists.txt').read_text().count('# DeskPort Big Sur format runtime'), 1)
            self.assertTrue((source / 'src/deskport/macos/legacy-screen-video.m').is_file())
            cmake = shutil.which('cmake') or ('C:/msys64/ucrt64/bin/cmake.exe' if Path('C:/msys64/ucrt64/bin/cmake.exe').is_file() else None)
            ninja = shutil.which('ninja') or ('C:/msys64/ucrt64/bin/ninja.exe' if Path('C:/msys64/ucrt64/bin/ninja.exe').is_file() else None)
            if cmake and ninja:
                # Configure the actual patched FindOpus against flat headers.
                # This reproduces the native CI failure without a compiler.
                project = source / 'opus-test'
                project.mkdir()
                prefix = source / 'opus-prefix'
                (prefix / 'include').mkdir(parents=True)
                (prefix / 'lib').mkdir()
                (prefix / 'include/opus.h').write_text('#define DESKPORT_OPUS_LAYOUT_FIXTURE 1\n')
                (prefix / 'lib/libopus.a').write_bytes(b'fixture archive')
                (project / 'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.24)
project(OpusLayout LANGUAGES NONE)
set(Opus_ROOT_DIR "{prefix.as_posix()}")
set(OPUS_USE_STATIC ON)
include("{opus_module.as_posix()}")
if(NOT Opus_FOUND OR NOT EXISTS "${{Opus_INCLUDE_DIR}}/opus/opus.h")
    message(FATAL_ERROR "Legacy Opus header layout failed")
endif()
''')
                configured = subprocess.run([cmake, '-G', 'Ninja', '-DCMAKE_MAKE_PROGRAM=' + ninja,
                    '-S', str(project), '-B', str(project / 'build')], capture_output=True, text=True)
                self.assertEqual(configured.returncode, 0, configured.stdout + configured.stderr)


if __name__ == '__main__':
    unittest.main()
