#!/bin/sh
# Build glmark2-macos for macOS 10.6 (Snow Leopard) x86_64.
#
# Cross-compiles from any Apple Silicon / Intel Mac with a modern Xcode
# against the 10.6 SDK, using the vendored libc++ 5.0.1 (leopard-webkit-build)
# and LLVM 5.0.1 libc++experimental sources. libc++ and libc++abi are linked
# statically into the executable, so the binary only depends on frameworks
# that ship with 10.6.
#
# Prerequisites:
#   - Xcode with the 10.6 SDK at
#     /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX10.6.sdk
#   - Homebrew LLVM for llvm-ar/llvm-ranlib (/opt/homebrew/opt/llvm/bin)
#   - meson >= 0.54, ninja
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX10.6.sdk
LLVMAR=/opt/homebrew/opt/llvm/bin/llvm-ar

cd "$ROOT"

# 1. Compile the clock_gettime/strndup/open_memstream shim (10.6 lacks them).
clang -target x86_64-apple-macos10.6 -arch x86_64 \
  -isysroot "$SDK" -fallow-unsupported -w \
  -c cross-compat/clock_gettime_compat.c -o cross-compat/clock_gettime_compat.o

# 2. Build libc++experimental (std::experimental::filesystem impl, LLVM 5.0.1).
#    libc++ 5 headers are mostly header-only; these out-of-line symbols are
#    normally provided by libc++experimental.a, which the bundle doesn't ship.
for f in directory_iterator operations path; do
  clang++ -target x86_64-apple-macos10.6 -arch x86_64 \
    -isysroot "$SDK" -fallow-unsupported -femulated-tls -std=c++17 -w \
    -nostdinc++ -isystem "$ROOT/cross-compat/libcxx/include" \
    -include "$ROOT/cross-compat/math_compat.h" \
    -c "cross-compat/libcxx-experimental/$f.cpp" \
    -o "cross-compat/libcxx-experimental/$f.o"
done
"$LLVMAR" rcs cross-compat/libcxx-experimental/libc++experimental.a \
  cross-compat/libcxx-experimental/directory_iterator.o \
  cross-compat/libcxx-experimental/operations.o \
  cross-compat/libcxx-experimental/path.o

# 3. Configure. NOTE: meson pins the cross file at first setup; if you edit
#    cross-compat/glmark2-cross-10.6.txt you must wipe build-106 first.
rm -rf build-106
meson setup build-106 \
  --cross-file cross-compat/glmark2-cross-10.6.txt \
  -Dflavors=macos-gl -Dcpp_std=c++17 --buildtype release

# 4. Build.
ninja -C build-106

echo
echo "Built: build-106/src/glmark2-macos ($(file build-106/src/glmark2-macos | cut -d: -f2))"
echo "Verify: otool -l build-106/src/glmark2-macos | grep -A2 LC_VERSION_MIN"
echo
echo "To run it needs the data files; e.g.:"
echo "  ./build-106/src/glmark2-macos --data-path data -b build:duration=5"
