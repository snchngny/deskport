#!/bin/bash
# Re-sign an already verified, staged app with one persistent local identity.
# This is local testing/deployment, not Developer ID distribution or notarization.
set -euo pipefail
if [ "$#" -ne 2 ] || [ "$(uname -s)" != Darwin ]; then
    echo 'Usage on macOS: sign-macos-local.sh IDENTITY_SHA1 STAGED_DeskPort.app' >&2
    exit 2
fi
identity=$1
app=$2
if ! [[ "$identity" =~ ^[[:xdigit:]]{40}$ ]] || [ ! -d "$app" ] || [ -L "$app" ]; then
    echo 'Expected an exact keychain identity SHA-1 and a staged app directory' >&2
    exit 2
fi
app=$(cd "$app" && pwd -P)
case "$app" in
    /Applications/*|/System/*|/Library/*)
        echo 'Sign a staged copy outside system application folders, never a live app' >&2
        exit 2
        ;;
esac
if [ "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$app/Contents/Info.plist")" != io.github.keithxc.DeskPort ]; then
    echo 'Unexpected application identifier; refusing to sign' >&2
    exit 1
fi
/usr/bin/codesign --verify --deep --strict "$app"
/usr/bin/codesign --force --deep --sign "$identity" --timestamp=none \
    --preserve-metadata=identifier,entitlements,flags "$app"
/usr/bin/codesign --verify --deep --strict "$app"
requirement="identifier \"io.github.keithxc.DeskPort\" and certificate leaf = H\"$identity\""
/usr/bin/codesign --verify --strict -R "=$requirement" "$app"
actual=$(/usr/bin/codesign -d -r- "$app" 2>&1)
if printf '%s\n' "$actual" | grep -q 'designated =>.*cdhash'; then
    echo 'The app still has a build-specific designated requirement' >&2
    exit 1
fi
printf '%s\n' "$actual"
