#!/usr/bin/env python3
"""Build the pinned host dependencies for Intel macOS 11.7, outside the checkout."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tarfile
import tempfile
import urllib.request

REPO = Path(__file__).resolve().parents[1]
FMT = {'name': 'fmt', 'file': 'fmt-11.2.0.tar.gz', 'bytes': 705751,
       'urls': ['https://github.com/fmtlib/fmt/archive/40626af88bd7df9a5fb80be7b25ac85b122d6c21.tar.gz'],
       'sha256': '992216a0605e03f0373984ecad11158f42d8569782f1b2c84035272da10f0ff1'}


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def fetch(entry, directory):
    directory.mkdir(parents=True, exist_ok=True)
    target = directory / entry['file']
    if target.exists():
        if target.stat().st_size != entry['bytes'] or digest(target) != entry['sha256']:
            raise RuntimeError('Dependency cache checksum mismatch: ' + entry['name'])
        return target
    for url in entry['urls']:
        with tempfile.NamedTemporaryFile(dir=directory, delete=False) as stream:
            temporary = Path(stream.name)
            try:
                with urllib.request.urlopen(urllib.request.Request(url, headers={'User-Agent': 'DeskPort-build'}), timeout=180) as response:
                    shutil.copyfileobj(response, stream)
                stream.close()
                if temporary.stat().st_size != entry['bytes'] or digest(temporary) != entry['sha256']:
                    raise RuntimeError('Downloaded dependency checksum mismatch: ' + entry['name'])
                temporary.replace(target)
                return target
            finally:
                temporary.unlink(missing_ok=True)
    raise RuntimeError('Could not obtain dependency: ' + entry['name'])


def unpack(archive, destination):
    marker = destination / '.deskport-archive-sha256'
    sha = digest(archive)
    if destination.exists():
        if not marker.is_file() or marker.read_text().strip() != sha:
            raise RuntimeError('Unmanaged or changed dependency source: ' + destination.name)
    else:
        destination.mkdir(parents=True)
        with tarfile.open(archive) as source:
            source.extractall(destination, filter='data')
        marker.write_text(sha + '\n')
    children = [p for p in destination.iterdir() if p.is_dir()]
    if len(children) != 1:
        raise RuntimeError('Expected one source archive root')
    return children[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--jobs', type=int, default=3)
    args = parser.parse_args()
    if platform.system() != 'Darwin' or platform.machine() != 'x86_64':
        raise SystemExit('Intel macOS build host required')
    import build_storage
    root = build_storage.output(args.root.resolve())
    root.mkdir(parents=True, exist_ok=True)
    marker = root / '.deskport-legacy-deps.json'
    owner = {'owner': 'deskport-legacy-deps-v1', 'recipe': digest(Path(__file__))}
    if marker.exists():
        if json.loads(marker.read_text()) != owner:
            raise SystemExit('Dependency recipe changed: select a fresh cache')
    elif any(root.iterdir()):
        raise SystemExit('Refusing unmanaged dependency cache')
    else:
        marker.write_text(json.dumps(owner))
    prefix = root / 'prefix'
    ready = root / 'ready.json'
    if ready.is_file():
        proof = json.loads(ready.read_text())
        for name, expected in proof['files'].items():
            if digest(prefix / name) != expected:
                raise SystemExit('Compiled dependency checksum mismatch')
        print('Verified retained Intel Big Sur dependencies')
        return
    prefix.mkdir(exist_ok=True)
    include = prefix / 'include'
    include.mkdir(exist_ok=True)
    shutil.copytree(REPO / 'libs/mac/include/openssl', include / 'openssl', dirs_exist_ok=True)
    for header in (REPO / 'libs/mac/include').glob('opus*.h'):
        shutil.copyfile(header, include / header.name)
    (prefix / 'lib').mkdir(exist_ok=True)
    for name in ('libssl.3.dylib', 'libcrypto.3.dylib', 'libopus.a'):
        shutil.copyfile(REPO / 'libs/mac/lib' / name, prefix / 'lib' / name)
    for name in ('ssl', 'crypto'):
        link = prefix / 'lib' / f'lib{name}.dylib'
        if not link.exists():
            link.symlink_to(f'lib{name}.3.dylib')
    env = dict(os.environ, MACOSX_DEPLOYMENT_TARGET='11.7',
               PKG_CONFIG_PATH=str(prefix / 'lib/pkgconfig'),
               DESKPORT_CMAKE_PREFIX_PATH=str(prefix), DESKPORT_OPENSSL_ROOT=str(prefix))
    jobs = str(max(1, min(args.jobs, 4)))

    def run(command, cwd=None):
        print('Legacy dependency:', command[0], flush=True)
        subprocess.run(list(map(str, command)), cwd=cwd, env=env, check=True, timeout=7200)

    common = ['-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_OSX_ARCHITECTURES=x86_64',
              '-DCMAKE_OSX_DEPLOYMENT_TARGET=11.7', '-DCMAKE_POLICY_VERSION_MINIMUM=3.5',
              f'-DCMAKE_INSTALL_PREFIX={prefix}']
    entries = {item['name']: item for item in json.loads((REPO / 'winbuild/inputs.json').read_text())['inputs']}
    sources = {name: unpack(fetch(entry, root / 'downloads'), root / 'sources' / name)
               for name, entry in [('fmt', FMT)] + [(name, entries[name]) for name in ('miniupnpc', 'media-FFmpeg', 'media-x264')]}
    for name, extra in [('fmt', ['-DFMT_TEST=OFF', '-DFMT_DOC=OFF', '-DFMT_INSTALL=ON']),
                        ('miniupnpc', ['-DUPNPC_BUILD_SHARED=OFF', '-DUPNPC_BUILD_STATIC=ON', '-DUPNPC_BUILD_TESTS=OFF', '-DUPNPC_BUILD_SAMPLE=OFF'])]:
        build = root / (name + '-build')
        run(['cmake', '-S', sources[name], '-B', build, *common, *extra])
        run(['cmake', '--build', build, '-j', jobs])
        run(['cmake', '--install', build])
    vendor_manifest = json.loads((REPO / 'host/vendor/sunshine.json').read_text())
    vendor_archive = REPO / 'host/vendor' / vendor_manifest['archive']
    if digest(vendor_archive) != vendor_manifest['sha256']:
        raise SystemExit('Pinned host archive checksum mismatch')
    vendor = root / 'host-source'
    if not vendor.exists():
        vendor.mkdir()
        with tarfile.open(vendor_archive) as archive:
            archive.extractall(vendor, filter='data')
    build_deps = vendor / 'third-party/build-deps'
    for name, target in [('media-FFmpeg', 'FFmpeg'), ('media-x264', 'x264')]:
        shutil.copytree(sources[name], build_deps / 'third-party/FFmpeg' / target, dirs_exist_ok=True)
    media_build = root / 'media-build'
    run(['cmake', '-S', build_deps, '-B', media_build, *common,
         '-DBUILD_ALL=OFF', '-DBUILD_FFMPEG=ON', '-DBUILD_FFMPEG_SVT_AV1=OFF', '-DBUILD_FFMPEG_X265=OFF',
         '-DBUILD_FFMPEG_CUDA_LLVM=OFF', '-DBUILD_FFMPEG_VULKAN=OFF', '-DPARALLEL_BUILDS=' + jobs,
         '-DCMAKE_C_FLAGS=-mmacosx-version-min=11.7', '-DCMAKE_CXX_FLAGS=-mmacosx-version-min=11.7'])
    run(['cmake', '--build', media_build, '-j', jobs])
    run(['cmake', '--install', media_build])
    env.update(DESKPORT_SOURCE=str(REPO), DESKPORT_WORK=str(root / 'rtc'),
               DESKPORT_DATACHANNEL_PREFIX=str(prefix), DESKPORT_MACOS_ARCH='x86_64', DESKPORT_MACOS_MIN='11.7', DESKPORT_JOBS=jobs)
    run(['bash', REPO / 'scripts/build-linux-libdatachannel.sh'])
    pc = prefix / 'lib/pkgconfig'
    pc.mkdir(parents=True, exist_ok=True)
    if not (pc / 'miniupnpc.pc').exists():
        (pc / 'miniupnpc.pc').write_text(f'prefix={prefix}\nlibdir=${{prefix}}/lib\nincludedir=${{prefix}}/include\nName: miniupnpc\nDescription: MiniUPnP client\nVersion: 2.3.3\nLibs: -L${{libdir}} -lminiupnpc\nCflags: -I${{includedir}}\n')
    notices = prefix / 'share/deskport-legacy-notices'
    notices.mkdir(parents=True, exist_ok=True)
    for name, source in sources.items():
        for file in source.glob('*'):
            if file.is_file() and file.name.upper().startswith(('LICENSE', 'COPYING')):
                shutil.copyfile(file, notices / (name + '-' + file.name))
    shutil.copytree(root / 'rtc/cache/libdatachannel-notices', notices / 'libdatachannel', dirs_exist_ok=True)
    files = {str(p.relative_to(prefix)): digest(p) for p in prefix.rglob('*') if p.is_file() and not p.is_symlink()}
    ready.write_text(json.dumps({'target': 'x86_64-macos11.7', 'files': files}, indent=2))
    print('Built Intel Big Sur dependency prefix:', prefix)


if __name__ == '__main__':
    main()
