#!/usr/bin/env bash
# Fetch the pinned linuxdeploy + Qt plugin used to build the AppImage
# (tools/package_appimage.sh). Both are themselves AppImages; they are not
# committed (tools/third-party/ is git-ignored). See docs/tools.md.
#
# Upstream's own "continuous" tag is re-pointed in place, so the pin here is a
# DATED release tag plus the SHA256 of what we fetched — a silently changed binary fails here
# instead of inside a release build.
#
# One pair of hashes PER ARCHITECTURE: an AppImage is native code, so an ARM64
# host (Raspberry Pi) needs the aarch64 tools to package an aarch64 AppImage.
# Both arches come from the same pinned tags.
set -euo pipefail

TOOLS_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$TOOLS_DIR/common.sh"

LINUXDEPLOY_TAG="1-alpha-20251107-1"      # linuxdeploy
PLUGIN_TAG="1-alpha-20250213-1"          # linuxdeploy-plugin-qt
DEST_DIR="$TOOLS_DIR/third-party"
mkdir -p "$DEST_DIR"

# SHA256 of the pinned assets: x86_64 recorded 2026-07-29, aarch64 2026-08-04.
# Upstream re-tags its "continuous" builds in place, which is exactly why the
# dated tag alone is not enough of a pin.
ARCH="$(host_arch)"
case "$ARCH" in
x86_64)
    LINUXDEPLOY_SHA256="c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d"
    PLUGIN_SHA256="15106be885c1c48a021198e7e1e9a48ce9d02a86dd0a1848f00bdbf3c1c92724"
    ;;
aarch64)
    LINUXDEPLOY_SHA256="620095110d693282b8ebeb244a95b5e911cf8f65f76c88b4b47d16ae6346fcff"
    PLUGIN_SHA256="bf1c24aff6d749b5cf423afad6f15abd4440f81dec1aab95706b25f6667cdcf1"
    ;;
*)
    # linuxdeploy also publishes i386 and armhf, but this project supports
    # 64-bit hosts only (see docs/platforms.md) — so say so instead of
    # downloading something that cannot run.
    echo "no pinned linuxdeploy for $ARCH — AppImage packaging supports x86_64 and aarch64" >&2
    exit 1
    ;;
esac

fetch() { # url, target, expected-sha256
    local url="$1" target="$2" want="$3"
    if [ -x "$target" ] && [ "$(sha256sum "$target" | cut -d' ' -f1)" = "$want" ]; then
        echo "$(basename "$target") already present (sha256 ok)"
        return 0
    fi
    echo "downloading $url"
    curl -fsSL -o "$target" "$url"
    local got
    got="$(sha256sum "$target" | cut -d' ' -f1)"
    if [ "$got" != "$want" ]; then
        echo "ERROR: $(basename "$target") sha256 mismatch" >&2
        echo "  expected $want" >&2
        echo "  got      $got" >&2
        echo "  Upstream changed the asset. Verify the release, then update the" >&2
        echo "  hash in this script." >&2
        rm -f "$target"
        exit 1
    fi
    chmod +x "$target"
}

fetch "https://github.com/linuxdeploy/linuxdeploy/releases/download/$LINUXDEPLOY_TAG/linuxdeploy-$ARCH.AppImage" \
    "$DEST_DIR/linuxdeploy-$ARCH.AppImage" "$LINUXDEPLOY_SHA256"
fetch "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/$PLUGIN_TAG/linuxdeploy-plugin-qt-$ARCH.AppImage" \
    "$DEST_DIR/linuxdeploy-plugin-qt-$ARCH.AppImage" "$PLUGIN_SHA256"

# Containers and CI runners have no FUSE, so the tools have to unpack themselves
# rather than mount; this is also how package_appimage.sh invokes them.
export APPIMAGE_EXTRACT_AND_RUN=1
"$DEST_DIR/linuxdeploy-$ARCH.AppImage" --version
