#!/usr/bin/env python3
"""Select the pinned host's AVFoundation adapter for the Big Sur build only."""
from pathlib import Path
import shutil
import sys


def apply(source, repo):
    path = source / 'cmake/compile_definitions/macos.cmake'
    text = path.read_text(encoding='utf-8')
    old = '"${CMAKE_SOURCE_DIR}/src/deskport/macos/screen-video.m"'
    new = '"${CMAKE_SOURCE_DIR}/src/deskport/macos/legacy-screen-video.m"'
    if old not in text and new not in text:
        raise SystemExit('Legacy capture: pinned ScreenCaptureKit source anchor missing')
    if old in text:
        if text.count(old) != 2:
            raise SystemExit('Legacy capture: unexpected ScreenCaptureKit source count')
        text = text.replace(old, new)
        text = text.replace('        "-framework ScreenCaptureKit"\n', '')
        text = text.replace('    "${CMAKE_SOURCE_DIR}/src/deskport/macos/color-conversion.m"', '')
        # AVVideo uses manual retain/release; the small subclass has no ARC ownership.
        text = text.replace('PROPERTIES COMPILE_FLAGS "-fobjc-arc"',
                            'PROPERTIES COMPILE_FLAGS "-fno-objc-arc"')
        path.write_text(text, encoding='utf-8')
    target = source / 'src/deskport/macos/legacy-screen-video.m'
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(repo / 'host/macos/legacy-screen-video.m', target)


if __name__ == '__main__':
    apply(Path(sys.argv[1]).resolve(), Path(__file__).resolve().parents[2])
