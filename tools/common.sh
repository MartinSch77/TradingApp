#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

# Shared host detection for the Linux (bash) entry points — the counterpart of
# tools/common.ps1, limited to the two things that genuinely differ between the
# machines this project is built on:
#
#   * The Qt kit directory. An x86-64 host builds against ~/Qt/<ver>/gcc_64, an
#     ARM64 host (Raspberry Pi 4/5/Zero 2 W on a 64-bit OS, ARM servers) against
#     ~/Qt/<ver>/gcc_arm64 — Qt ships official Linux ARM64 desktop binaries from
#     6.7 on (aqt host linux_arm64, arch linux_gcc_arm64, installed into
#     ~/Qt/<ver>/gcc_arm64; verified against Qt's own Updates.xml). Hardcoding
#     gcc_64 is what made every script here x86-only.
#   * The LLVM toolset. The MC/DC coverage build and the TSan build need
#     clang >= 18 (-fcoverage-mcdc landed in 18), and clang++, llvm-profdata,
#     llvm-cov and llvm-symbolizer must all come from the SAME installation:
#     a profile written by one version and merged by another is rejected. Debian
#     and Raspberry Pi OS ship ONE clang, and its version is whatever the
#     release froze — so the version is resolved, not assumed.
#
# Dot-source it, never run it:   . "$(dirname "$0")/common.sh"
# Run it directly to see what it resolves on this host (a probe, no side effects).

# x86_64 | aarch64 | <whatever uname says>: the spelling Qt, linuxdeploy and
# AppImage file names all share, so one answer serves all three.
host_arch() {
    case "$(uname -m)" in
    x86_64 | amd64) echo x86_64 ;;
    aarch64 | arm64) echo aarch64 ;;
    *) uname -m ;;
    esac
}

# The kit directory name under ~/Qt/<version>/ for this host — empty when Qt
# publishes no desktop binaries for it (32-bit ARM, i686: those need the
# distribution's own Qt 6, see qt_prefix below and docs/platforms.md).
qt_kit_dir() {
    case "$(host_arch)" in
    x86_64) echo gcc_64 ;;
    aarch64) echo gcc_arm64 ;;
    *) echo "" ;;
    esac
}

# aqt coordinates for this host: `aqt install-qt $(qt_aqt_host) desktop <ver> $(qt_aqt_arch)`.
# ARM64 lives under its own host name; the arch id is "linux_" + the kit dir.
qt_aqt_host() {
    case "$(host_arch)" in
    aarch64) echo linux_arm64 ;;
    *) echo linux ;;
    esac
}

qt_aqt_arch() {
    local kit
    kit="$(qt_kit_dir)"
    [ -n "$kit" ] && echo "linux_$kit"
}

# The Qt kit this host should build against:
#   1. $HOME/Qt/<wanted version>/<kit>   (the layout setup.sh installs)
#   2. the newest $HOME/Qt/*/<kit>       (so a machine one Qt release ahead or
#                                        behind still builds without QT_PREFIX)
#   3. nothing — prints an empty string, and the callers then configure with an
#      empty CMAKE_PREFIX_PATH so CMake finds a DISTRIBUTION Qt 6 instead. That
#      is the second supported Raspberry Pi route (apt's qt6-base-dev +
#      qt6-charts-dev), and the only route on architectures Qt ships no binaries
#      for.
# QT_PREFIX in the environment always wins over all of this.
qt_prefix() {
    local want="${1:-${QT_VERSION:-6.11.1}}" kit newest
    kit="$(qt_kit_dir)"
    if [ -n "$kit" ]; then
        if [ -d "$HOME/Qt/$want/$kit" ]; then
            echo "$HOME/Qt/$want/$kit"
            return 0
        fi
        newest="$(ls -d "$HOME"/Qt/*/"$kit" 2>/dev/null | sort -V | tail -1)"
        if [ -n "$newest" ]; then
            echo "$newest"
            return 0
        fi
    fi
    echo ""
}

# Suffix of one matched LLVM installation whose clang is at least $1 (default
# 18): "-18" for clang++-18/llvm-cov-18/…, or "" for unsuffixed binaries.
# Versioned candidates come first and newest first, and every tool of the set
# has to exist under the SAME suffix — mixing llvm-profdata and llvm-cov across
# installations is the documented way to break a merged profile.
#
# Returns 1 (printing nothing) when the host has no clang new enough. Callers
# report that as `skipped`, never as a failure: clang >= 18 is a requirement of
# the MC/DC and TSan EVIDENCE, not of the product.
llvm_suffix() {
    local min="${1:-18}" name suffix major tool ok
    for name in $( { compgen -c clang++ || true; } | sort -u -t- -k2,2Vr); do
        case "$name" in
        clang++ | clang++-[0-9]*) ;;
        *) continue ;;
        esac
        suffix="${name#clang++}"
        major="$("$name" --version 2>/dev/null | grep -oE 'version [0-9]+' | head -1 | awk '{print $2}')"
        [ -n "$major" ] || continue
        [ "$major" -ge "$min" ] || continue
        ok=1
        for tool in llvm-profdata llvm-cov llvm-symbolizer; do
            command -v "$tool$suffix" >/dev/null 2>&1 || ok=0
        done
        [ "$ok" -eq 1 ] || continue
        echo "$suffix"
        return 0
    done
    return 1
}

# Probe mode: `tools/common.sh` with no dot-source prints what this host resolves
# to. Handy on a new machine (and it is how the ARM64 path was checked from an
# x86-64 one, with a uname stub ahead of PATH).
if [ "${BASH_SOURCE[0]}" = "${0}" ]; then
    printf '%-14s %s\n' "uname -m" "$(uname -m)"
    printf '%-14s %s\n' "host arch" "$(host_arch)"
    printf '%-14s %s\n' "Qt kit dir" "$(qt_kit_dir)"
    printf '%-14s %s\n' "aqt host" "$(qt_aqt_host)"
    printf '%-14s %s\n' "aqt arch" "$(qt_aqt_arch)"
    printf '%-14s %s\n' "Qt prefix" "$(qt_prefix)${QT_PREFIX:+  (QT_PREFIX overrides: $QT_PREFIX)}"
    if suffix="$(llvm_suffix)"; then
        printf '%-14s %s\n' "LLVM toolset" "clang++$suffix (MC/DC + TSan available)"
    else
        printf '%-14s %s\n' "LLVM toolset" "none >= 18 — MC/DC and TSan report skipped"
    fi
fi
