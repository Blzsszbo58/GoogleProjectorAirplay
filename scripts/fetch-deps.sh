#!/usr/bin/env bash
# Downloads the open-source libraries MirrorTV is built on into third_party/.
# Pinned to known-good commits so builds are reproducible.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TP="$ROOT/third_party"
mkdir -p "$TP"

fetch() {  # name url commit
    local name="$1" url="$2" commit="$3" dir="$TP/$1"
    if [ -d "$dir/.git" ] && [ "$(git -C "$dir" rev-parse HEAD)" = "$commit" ]; then
        echo "$name: already at $commit"
        return
    fi
    rm -rf "$dir"
    git init -q "$dir"
    git -C "$dir" remote add origin "$url"
    git -C "$dir" fetch -q --depth 1 origin "$commit"
    git -C "$dir" checkout -q FETCH_HEAD
    echo "$name: fetched $commit"
}

# AirPlay protocol implementation (GPL-3.0)
fetch UxPlay   https://github.com/FDH2/UxPlay.git              534e2cc0e558d6c94bcc66964f8fc13ad73a6cdc
# Apple property lists (LGPL-2.1), tag 2.7.0
fetch libplist https://github.com/libimobiledevice/libplist.git cf5897a71ea412ea2aeb1e2f6b5ea74d4fabfd8c
# Apple Lossless decoder (Apache-2.0)
fetch alac     https://github.com/macosforge/alac.git           c38887c5c5e64a4b31108733bd79ca9b2496d987
