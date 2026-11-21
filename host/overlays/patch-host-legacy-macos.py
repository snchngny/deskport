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
    # The reused Moonlight Opus headers are flat; Sunshine includes opus/opus.h.
    # Adapt the include layout in this run's build directory, not the retained cache.
    opus = source / 'cmake/dependencies/FindOpus.cmake'
    if opus.is_file():
        text = opus.read_text(encoding='utf-8')
        marker = '# DeskPort flat Opus headers'
        if marker not in text:
            anchor = 'find_path(\n    Opus_INCLUDE_DIR'
            if text.count(anchor) != 1:
                raise SystemExit('Legacy Opus: pinned module anchor missing')
            compatibility = '''# DeskPort flat Opus headers
if(NOT Opus_INCLUDE_DIR AND EXISTS "${Opus_ROOT_DIR}/include/opus.h")
    set(DESKPORT_OPUS_INCLUDE "${CMAKE_BINARY_DIR}/deskport-opus/include")
    file(MAKE_DIRECTORY "${DESKPORT_OPUS_INCLUDE}/opus")
    file(GLOB DESKPORT_OPUS_HEADERS "${Opus_ROOT_DIR}/include/opus*.h")
    foreach(header IN LISTS DESKPORT_OPUS_HEADERS)
        get_filename_component(name "${header}" NAME)
        configure_file("${header}" "${DESKPORT_OPUS_INCLUDE}/opus/${name}" COPYONLY)
    endforeach()
    set(Opus_INCLUDE_DIR "${DESKPORT_OPUS_INCLUDE}" CACHE PATH "Legacy Opus include layout" FORCE)
endif()

'''
            opus.write_text(text.replace(anchor, compatibility + anchor), encoding='utf-8')
    # Apple's std::format runtime requires newer macOS. fmt supplies the same
    # formatting operations without introducing a newer system-library ABI.
    format_roots = (source / 'src', source / 'third-party/libdisplaydevice/src')
    for path in (path for root in format_roots for path in root.rglob('*')):
        if not path.is_file() or path.suffix not in ('.h', '.hpp', '.cpp', '.mm'):
            continue
        text = path.read_text(encoding='utf-8')
        updated = text.replace('#include <format>', '#include <fmt/format.h>\n#include <fmt/std.h>\n#include <fmt/chrono.h>\n#include <fmt/xchar.h>')
        updated = updated.replace('std::format', 'fmt::format')
        if updated != text:
            path.write_text(updated, encoding='utf-8')
    cmake = source / 'CMakeLists.txt'
    if cmake.is_file():
        text = cmake.read_text(encoding='utf-8')
        marker = '# DeskPort Big Sur format runtime'
        if marker not in text:
            cmake.write_text(text + '\n' + marker + '\nfind_package(fmt CONFIG REQUIRED)\ntarget_link_libraries(sunshine fmt::fmt-header-only)\n', encoding='utf-8')
        text = cmake.read_text(encoding='utf-8')
        marker = '# DeskPort Big Sur display-device format runtime'
        if marker not in text:
            text += '\n' + marker + '\nforeach(component libdisplaydevice_common libdisplaydevice_macos)\n    if(TARGET ${component})\n        target_link_libraries(${component} PRIVATE fmt::fmt-header-only)\n    endif()\nendforeach()\n'
            cmake.write_text(text, encoding='utf-8')
        marker = '# DeskPort host FFmpeg header priority'
        if marker not in text:
            text += '''
# DeskPort host FFmpeg header priority
# The reused client prefix also has FFmpeg headers. The host must use the
# headers shipped with its separately built static FFmpeg/CBS libraries.
target_include_directories(sunshine BEFORE PRIVATE "${FFMPEG_PREPARED_BINARIES}/include")
file(STRINGS "${FFMPEG_PREPARED_BINARIES}/include/libavcodec/version_major.h"
     DESKPORT_AVCODEC_VERSION_LINE REGEX "^#define LIBAVCODEC_VERSION_MAJOR ")
string(REGEX MATCH "[0-9]+" DESKPORT_AVCODEC_VERSION_MAJOR "${DESKPORT_AVCODEC_VERSION_LINE}")
if(NOT DESKPORT_AVCODEC_VERSION_MAJOR)
    message(FATAL_ERROR "Cannot determine the host FFmpeg ABI")
endif()
target_compile_definitions(sunshine PRIVATE DESKPORT_EXPECTED_AVCODEC_MAJOR=${DESKPORT_AVCODEC_VERSION_MAJOR})
target_sources(sunshine PRIVATE "${CMAKE_SOURCE_DIR}/src/deskport/macos/host-ffmpeg-abi.cpp")
'''
            cmake.write_text(text, encoding='utf-8')
        shutil.copyfile(repo / 'host/macos/host-ffmpeg-abi.cpp', source / 'src/deskport/macos/host-ffmpeg-abi.cpp')


if __name__ == '__main__':
    apply(Path(sys.argv[1]).resolve(), Path(__file__).resolve().parents[2])
