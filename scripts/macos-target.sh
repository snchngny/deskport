#!/usr/bin/env bash
# Source from build recipes; defaults preserve the upstream release target.
deskport_macos_target() {
    export DESKPORT_MACOS_PROFILE=${DESKPORT_MACOS_PROFILE:-modern}
    case "$DESKPORT_MACOS_PROFILE" in
        modern)
            export DESKPORT_MACOS_ARCH=arm64 DESKPORT_MACOS_MIN=26.0
            export DESKPORT_LEGACY_MACOS=0
            ;;
        intel-bigsur)
            export DESKPORT_MACOS_ARCH=x86_64 DESKPORT_MACOS_MIN=11.7
            export DESKPORT_LEGACY_MACOS=1
            ;;
        *) echo "Unknown macOS profile: $DESKPORT_MACOS_PROFILE" >&2; return 2 ;;
    esac
}
deskport_macos_target
