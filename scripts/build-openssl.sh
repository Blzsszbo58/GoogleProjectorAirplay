#!/usr/bin/env bash
# Cross-compiles a static libcrypto for each Android ABI with the NDK.
# Output: third_party/openssl-android/<abi>/{include,lib/libcrypto.a}
set -euo pipefail

OPENSSL_VERSION="${OPENSSL_VERSION:-3.0.15}"
API="${ANDROID_API:-26}"
ABIS="${ABIS:-armeabi-v7a arm64-v8a}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/third_party/openssl-android"
WORK="$ROOT/third_party/openssl-build"

NDK="${ANDROID_NDK_ROOT:-${ANDROID_NDK_HOME:-}}"
if [ -z "$NDK" ] || [ ! -d "$NDK" ]; then
    echo "Set ANDROID_NDK_ROOT to your NDK directory" >&2
    exit 1
fi
export ANDROID_NDK_ROOT="$NDK"

case "$(uname -s)" in
    Linux*)  HOST_TAG=linux-x86_64 ;;
    Darwin*) HOST_TAG=darwin-x86_64 ;;
    *)       HOST_TAG=windows-x86_64 ;;
esac
export PATH="$NDK/toolchains/llvm/prebuilt/$HOST_TAG/bin:$PATH"

mkdir -p "$WORK"
TARBALL="$WORK/openssl-$OPENSSL_VERSION.tar.gz"
if [ ! -f "$TARBALL" ]; then
    curl -fsSL -o "$TARBALL" \
        "https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VERSION/openssl-$OPENSSL_VERSION.tar.gz"
fi

for ABI in $ABIS; do
    if [ -f "$OUT/$ABI/lib/libcrypto.a" ]; then
        echo "OpenSSL for $ABI already built"
        continue
    fi
    case "$ABI" in
        armeabi-v7a) TARGET=android-arm ;;
        arm64-v8a)   TARGET=android-arm64 ;;
        x86)         TARGET=android-x86 ;;
        x86_64)      TARGET=android-x86_64 ;;
        *) echo "unknown ABI $ABI" >&2; exit 1 ;;
    esac
    SRC="$WORK/src-$ABI"
    rm -rf "$SRC"
    mkdir -p "$SRC"
    tar -xzf "$TARBALL" -C "$SRC" --strip-components=1
    (
        cd "$SRC"
        ./Configure "$TARGET" -D__ANDROID_API__="$API" \
            no-shared no-tests -fPIC \
            --prefix="$OUT/$ABI" --libdir=lib
        make -j"$(nproc 2>/dev/null || echo 4)" build_libs
        make install_dev
    )
    rm -rf "$SRC"
    echo "OpenSSL $OPENSSL_VERSION built for $ABI"
done
