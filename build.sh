#!/usr/bin/env bash
set -euo pipefail

: "${NDK_DIR:?Set NDK_DIR to your Android NDK r27c root}"
: "${BINDER_NDK:?Set BINDER_NDK to ColorOS/Android 17 system/lib64/libbinder_ndk.so}"
: "${LIBMIFACE:?Set LIBMIFACE to stock vendor/lib64/libmiface.so}"

TC="$NDK_DIR/toolchains/llvm/prebuilt/linux-x86_64/bin"
CXX="$TC/aarch64-linux-android31-clang++"
STRIP="$TC/llvm-strip"
READELF="$TC/llvm-readelf"

OUT="vendor/bin/hw/android.hardware.biometrics.face-service.tanzanite"
mkdir -p "$(dirname "$OUT")"

"$CXX" -O2 -std=c++17 -Wall -Wextra -fPIE -pie -rdynamic -static-libstdc++ \
  -Wl,--allow-shlib-undefined \
  -Wl,--version-script=src/face_export.map \
  -Wl,-rpath,/vendor/lib64 \
  -Isrc src/face_bridge.cpp "$LIBMIFACE" "$BINDER_NDK" -llog -ldl -pthread \
  -o "$OUT"

"$STRIP" --strip-unneeded "$OUT"
chmod 0755 "$OUT"

if "$READELF" -d "$OUT" | grep -q 'libc++_shared'; then
  echo "ERROR: $OUT unexpectedly links libc++_shared.so!" >&2
  exit 1
fi

echo "Built $OUT successfully:"
"$READELF" -d "$OUT" | grep -E 'NEEDED|RUNPATH'
"$READELF" --dyn-syms "$OUT" | grep -E 'AServiceManager_addService|AIBinder_transact|AImage_getPlaneData'
