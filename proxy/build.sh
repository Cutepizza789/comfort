#!/usr/bin/env bash
# Needs: Android NDK (set ANDROID_NDK_HOME), curl
set -e
cd "$(dirname "$0")"
mkdir -p include/openxr out
for h in openxr.h openxr_platform_defines.h; do
  [ -f include/openxr/$h ] || curl -fL -o include/openxr/$h \
    https://raw.githubusercontent.com/KhronosGroup/OpenXR-SDK/main/include/openxr/$h
done
CXX=$(ls -d "${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME}"/toolchains/llvm/prebuilt/*)/bin/aarch64-linux-android29-clang++
$CXX -shared -fPIC -O2 -std=c++17 -Iinclude proxy.cpp -o out/libopenxr_loader.so \
  -Wl,-soname,libopenxr_loader.so -Wl,-z,max-page-size=16384 -static-libstdc++ -llog -ldl
echo "built proxy/out/libopenxr_loader.so"
