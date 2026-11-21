#!/bin/bash
# Build an arm64 application containing the viewer, host and virtual display.
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
source "$repo/scripts/macos-target.sh"
cd "$repo"
export DESKPORT_TARGET=macos
source "$repo/scripts/build-paths.sh"
DESKPORT_HOST_SOURCE_DIR=$(deskport_output_path "${DESKPORT_HOST_SOURCE_DIR:-$DESKPORT_MACOS_BUILD_DIR/sunshine-vendored-source}")
storage_prepare
export DESKPORT_HOST_SOURCE_DIR
version=$(cat app/version.txt)
: "${DESKPORT_SIGN_IDENTITY:?Set a stable local code-signing identity; use - only for disposable development builds}"
if [ "$DESKPORT_SIGN_IDENTITY" = - ] && [ "${DESKPORT_ALLOW_ADHOC:-0}" != 1 ]; then
    echo "Ad-hoc updates invalidate macOS privacy grants. Use a stable signing identity, or set DESKPORT_ALLOW_ADHOC=1 for a disposable build." >&2
    exit 1
fi
export DEVELOPER_DIR=${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}
default_qt=/opt/homebrew/bin
[ "$DESKPORT_MACOS_ARCH" != x86_64 ] || default_qt=/usr/local/bin
qt_bin=${DESKPORT_QT_BIN:-$default_qt}
# Checkout and dependency-provider paths keep qmake's SDK/tool caches isolated.
build_dir=$DESKPORT_MACOS_BUILD_DIR
dist_dir=$DESKPORT_MACOS_DIST_DIR
qml_args=()
if [ "$DESKPORT_LEGACY_MACOS" = 1 ]; then
    qml_args+=("CONFIG+=imac_bigsur")
fi
if [ -n "${DESKPORT_QML_CACHEGEN:-}" ]; then
    qml_args+=("QT_TOOL.qmlcachegen.binary=$DESKPORT_QML_CACHEGEN")
fi
mkdir -p "$build_dir" "$dist_dir"
# Check the actual signing session before spending time building and staging.
# Listing identities or signing in another terminal does not prove key access here.
probe_dir=$(mktemp -d "$build_dir/.signing-probe.XXXXXX")
cp /usr/bin/true "$probe_dir/probe"
if ! codesign --force --sign "$DESKPORT_SIGN_IDENTITY" --timestamp=none "$probe_dir/probe"; then
    rm -f "$probe_dir/probe"
    rmdir "$probe_dir"
    echo "Signing preflight failed in this execution session. See docs/MACOS_PACKAGE.md; do not retry the full build or switch identities." >&2
    exit 1
fi
rm -f "$probe_dir/probe"
rmdir "$probe_dir"
(
    mkdir -p "$build_dir/app" "$dist_dir"
    touch "$build_dir/.qmake.stash"
    cd "$build_dir"
    # Refresh subprojects too: app/Info.plist is generated during qmake.
    "$qt_bin/qmake" -r "$repo/moonlight-qt.pro" CONFIG+=release CONFIG-=debug_and_release \
        QMAKE_APPLE_DEVICE_ARCHS="$DESKPORT_MACOS_ARCH" QMAKE_MACOSX_DEPLOYMENT_TARGET="$DESKPORT_MACOS_MIN" \
        QMAKE_CC=/usr/bin/clang QMAKE_CXX=/usr/bin/clang++ \
        "QMAKE_XCODE_DEVELOPER_PATH=$DEVELOPER_DIR" ${qml_args[@]+"${qml_args[@]}"}
    make -j"${DESKPORT_JOBS:-4}"
)
bash scripts/build-macos-host.sh
xcrun clang -arch "$DESKPORT_MACOS_ARCH" -mmacosx-version-min="$DESKPORT_MACOS_MIN" -fobjc-arc -framework Foundation -framework CoreGraphics -framework ApplicationServices -framework AppKit \
    host/macos/display-helper.m -o "$build_dir/deskport-display"
xcrun clang++ -arch "$DESKPORT_MACOS_ARCH" -mmacosx-version-min="$DESKPORT_MACOS_MIN" -std=c++17 -Wall -Wextra -Werror host/macos/recovery-helper.cpp -o "$build_dir/deskport-recovery"
stage=$(mktemp -d "$dist_dir/.package.XXXXXX")
zip_check=
trap 'chmod -R u+w "$stage" 2>/dev/null || true; rm -rf "$stage"; if [ -n "${zip_check:-}" ]; then rm -rf "$zip_check"; fi' EXIT
app="$stage/DeskPort.app"
ditto "$build_dir/app/DeskPort.app" "$app"
chmod -R u+w "$app"
if [ -n "${DESKPORT_TEST_BUILD_ID:-}" ]; then
    /usr/libexec/PlistBuddy -c "Add :DeskPortTestBuildID string $DESKPORT_TEST_BUILD_ID" "$app/Contents/Info.plist"
    /usr/libexec/PlistBuddy -c "Add :DeskPortSourceRevision string $(git rev-parse HEAD)" "$app/Contents/Info.plist"
fi
PATH="$qt_bin:$PATH" python3 scripts/deploy-macos-runtime.py "$app" "$repo/app"
python3 scripts/fix-macos-dependencies.py "$app"
# Resource-directory QML plugins are not traversed by codesign --deep.
python3 - "$app" <<'PY'
import pathlib, subprocess, sys
magic = {b'\xfe\xed\xfa\xce', b'\xce\xfa\xed\xfe', b'\xfe\xed\xfa\xcf', b'\xcf\xfa\xed\xfe', b'\xca\xfe\xba\xbe', b'\xbe\xba\xfe\xca'}
for path in pathlib.Path(sys.argv[1]).rglob('*'):
    if path.parent == pathlib.Path(sys.argv[1]) / 'Contents/MacOS':
        continue  # Sign the enclosing application after all nested code.
    if path.is_file() and not path.is_symlink():
        with path.open('rb') as stream:
            is_macho = stream.read(4) in magic
        if is_macho:
            subprocess.run(['codesign', '--force', '--sign', '-', str(path)], check=True)
PY
codesign --force --deep --sign - "$app"
mkdir -p "$app/Contents/Helpers"
cp "$build_dir/deskport-display" "$app/Contents/Helpers/deskport-display"
cp "$build_dir/deskport-recovery" "$app/Contents/Helpers/deskport-recovery"
mkdir -p "$app/Contents/Library/LaunchDaemons"
cp app/deploy/macos/io.github.keithxc.DeskPort.Recovery.plist "$app/Contents/Library/LaunchDaemons/"
host_app="$app/Contents/Helpers/Sunshine.app"
ditto "$build_dir/sunshine-vendored-build/Sunshine.app" "$host_app"
python3 - "$repo" "$host_app/Contents" <<'ASSETS'
import hashlib, json, pathlib, sys, tarfile
root = pathlib.Path(sys.argv[1]) / 'host/vendor'
manifest = json.loads((root / 'sunshine-macos-resources.json').read_text())
archive = root / manifest['archive']
if hashlib.sha256(archive.read_bytes()).hexdigest() != manifest['sha256']:
    raise SystemExit('Vendored Sunshine resource checksum mismatch')
with tarfile.open(archive) as resources:
    resources.extractall(sys.argv[2], filter='data')
ASSETS
if [ "$DESKPORT_LEGACY_MACOS" = 1 ]; then
    # The verified Moonlight OpenSSL binaries use Frameworks-relative install
    # names. Sunshine's linker preserves those names, so seed its own bundle
    # from the exact prefix used at link time before dependency relocation.
    : "${DESKPORT_OPENSSL_ROOT:?Missing verified Intel OpenSSL prefix}"
    mkdir -p "$host_app/Contents/Frameworks"
    for library in libssl.3.dylib libcrypto.3.dylib; do
        cp -L "$DESKPORT_OPENSSL_ROOT/lib/$library" "$host_app/Contents/Frameworks/$library"
    done
fi
python3 scripts/fix-macos-dependencies.py "$host_app"
find "$host_app/Contents/Frameworks" -type f -name '*.dylib' -exec codesign --force --sign - {} \;
cp host/macos/patches/libvirtualhid-target-display.patch host/macos/patches/libvirtualhid-smooth-drag.patch host/macos/patches/libvirtualhid-paced-scroll.patch host/macos/patches/sunshine-capture-timeout.patch host/macos/patches/sunshine-pkgconfig-link.patch "$host_app/Contents/Resources/"
cp host/macos/patches/sunshine-smart-streaming.patch host/macos/patches/sunshine-screen-capture-kit.patch "$host_app/Contents/Resources/"
mkdir -p "$host_app/Contents/Resources/deskport-smart-source/common" "$host_app/Contents/Resources/deskport-smart-source/macos"
cp host/common/smartstream.h host/common/inputactivity.h host/common/framecadence.h host/common/encoderpolicy.h host/common/ratecontrol.h host/common/picturequality.h "$host_app/Contents/Resources/deskport-smart-source/common/"
cp host/macos/admitted-display.h host/macos/pixelmatch.h host/macos/screen-video.h host/macos/screen-video.m host/macos/vtbitrate.h host/macos/vtlatency.h host/macos/vtclock.h host/macos/scroll-pacer.h "$host_app/Contents/Resources/deskport-smart-source/macos/"
cp scripts/build-macos-host.sh scripts/build-paths.sh scripts/apply-host-patches.py host/series host/overlays/patch-host-smart-stream.py host/overlays/patch-host-sync-cadence.py host/overlays/patch-host-input-activity.py host/overlays/patch-host-encoder-policy.py host/overlays/patch-host-vt-drain.py host/overlays/patch-host-remote-rate.py host/overlays/patch-host-data-saver.py host/overlays/patch-host-rate-api.py "$host_app/Contents/Resources/"
codesign --force --sign "$DESKPORT_SIGN_IDENTITY" --timestamp=none --options runtime \
    --entitlements host/macos/entitlements.plist "$host_app"
codesign --force --sign "$DESKPORT_SIGN_IDENTITY" --timestamp=none --options runtime "$app/Contents/Helpers/deskport-recovery"
cp LICENSE "$app/Contents/Resources/DeskPort-LICENSE"
cp docs/BUNDLED_COMPONENTS.md "$app/Contents/Resources/"
if [ "$DESKPORT_LEGACY_MACOS" = 1 ]; then
    ditto "${DESKPORT_CMAKE_PREFIX_PATH:?}/share/deskport-legacy-notices" "$app/Contents/Resources/legacy-component-notices"
fi
codesign --force --sign "$DESKPORT_SIGN_IDENTITY" --timestamp=none "$app/Contents/Helpers/deskport-display"
codesign --force --sign "$DESKPORT_SIGN_IDENTITY" --timestamp=none \
    --options runtime --entitlements host/macos/entitlements.plist "$app"
codesign --verify --deep --strict "$app"
# Do not distribute a bundle with unresolved build-machine dependencies.
python3 scripts/check-macos-bundle.py "$app"
if [ "$DESKPORT_LEGACY_MACOS" = 1 ]; then
    python3 scripts/check-macos-target.py "$app" --arch x86_64 --max-version 11.7.11
fi
rm -rf "$dist_dir/DeskPort.app"
ditto "$app" "$dist_dir/DeskPort.app"
ln -s /Applications "$stage/Applications"
hdiutil create -volname DeskPort -srcfolder "$stage" -ov -format UDZO "$dist_dir/DeskPort-${version}-macos-${DESKPORT_MACOS_ARCH}.dmg"
shasum -a 256 "$dist_dir/DeskPort-${version}-macos-${DESKPORT_MACOS_ARCH}.dmg"
# Nix fetches this zip and extracts it with Info-ZIP unzip, which writes
# AppleDouble entries as literal "._" files inside the sealed bundle. Store no
# extended attributes, and verify the bundle the way Nix will unpack it.
zip="$dist_dir/DeskPort-${version}-macos-${DESKPORT_MACOS_ARCH}.zip"
rm -f "$zip"
ditto -c -k --norsrc --noextattr --noacl --keepParent "$dist_dir/DeskPort.app" "$zip"
zip_check=$(mktemp -d "$dist_dir/.zip-check.XXXXXX")
/usr/bin/unzip -q "$zip" -d "$zip_check"
codesign --verify --deep --strict "$zip_check/DeskPort.app"
python3 scripts/check-macos-bundle.py "$zip_check/DeskPort.app"
rm -rf "$zip_check"
zip_check=
shasum -a 256 "$zip"
