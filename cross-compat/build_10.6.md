# Building glmark2 for macOS 10.6 (Snow Leopard)

Cross-compile `glmark2-macos` (macos-gl flavor, native Cocoa/NSOpenGL) to
x86_64-apple-macos10.6 using the 10.6 SDK, vendored libc++ 5.0.1
(leopard-webkit-build) and LLVM 5.0.1 libc++experimental sources.

Run `cross-compat/build-10.6.sh` from the repo root; it performs every step
below. The resulting binary links libc++ statically and only depends on
frameworks that ship with 10.6 (AppKit, Foundation, libobjc, libSystem,
libz). OpenGL.framework is opened at runtime via dlopen.

## Prerequisites

- Xcode with the 10.6 SDK at
  `/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX10.6.sdk`
- Homebrew LLVM (`/opt/homebrew/opt/llvm/bin/llvm-ar`) — Apple's `ar`
  mangles member names in these archives
- meson >= 0.54, ninja

Adjust the SDK/llvm paths in `cross-compat/glmark2-cross-10.6.txt` if they
differ. **Meson pins the cross file at first setup** — after editing it,
delete `build-106` before reconfiguring.

## Patches applied to glmark2

Mechanical portability fixes; no behavioural change on modern macOS builds.

| File | Change | Reason |
|------|--------|--------|
| `src/native-state-macos.mm` | 10.6/10.12 constant fallbacks (`NSWindowStyleMask*`, `NSEventMaskAny`, `NSEventTypeKeyDown`); `@autoreleasepool` → `NSAutoreleasePool` | The rename constants are 10.12+; `objc_autoreleasePoolPush/Pop` (emitted for `@autoreleasepool`) are 10.7+ runtime symbols — a 10.6 binary would fail to load |
| `src/gl-state-macos.mm` | `#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 1070` around `NSOpenGLPFAOpenGLProfile`/`NSOpenGLProfileVersion*`; `NSMutableArray<NSNumber*>*` → `NSMutableArray*`; same `NSAutoreleasePool` conversion | Profile constants are 10.7+ SDK (10.6 has only the legacy profile); lightweight generics annotations are missing from the 10.6 SDK declarations |
| `src/libmatrix/util.cc` | `std::ifstream(path)` → `std::ifstream(path.string().c_str())` | libc++ 5 has no path-taking fstream ctor |
| `src/libpng/pngconf.h` | `TARGET_OS_MAC` MACOS detection excludes `__APPLE__` | Every Apple-toolchain build defines `TARGET_OS_MAC`, which made libpng 1.2 think it was classic Mac OS and include `<fp.h>` |
| `meson.build` | Vendored static zlib/libpng/libjpeg-turbo also used when cross-compiling to darwin (zlib from the SDK) | Homebrew jpeg/png dylibs are built for modern deployment targets |
| `src/meson.build` | `gl-state-macos.mm` built into a static lib instead of the executable sources | Executables keep a pure C++ link language; meson does not honor ObjC++ link args from machine files, so the target triple would be lost |

## Compat files (in `cross-compat/`)

| File | Purpose |
|------|---------|
| `libcxx/` | libc++ 5.0.1 headers + static/dynamic libs (leopard-webkit-build), copied from the Mesa-VirGL cross-compat bundle |
| `libcxx-experimental/` | LLVM 5.0.1 `src/experimental/filesystem/*` (path.cpp, operations.cpp, directory_iterator.cpp); built into `libc++experimental.a` — provides the out-of-line `std::experimental::filesystem` symbols libc++ 5 expects |
| `compat-headers/filesystem` | Redirects `<filesystem>` to `<experimental/filesystem>` and aliases `std::filesystem` (libc++ 5 predates C++17 `<filesystem>`); found first via `-I` |
| `compat-headers/Security/SecKeychain.h` | Shadow of the 10.6 SDK header with `AUTH_TYPE_FIX_` rewritten in unsigned arithmetic — the signed-shift version overflows `int` and is not a constant expression in C++, which modern clang rejects in ObjC++ (pulled in via `NSURLCredential.h`) |
| `clock_gettime_compat.c` | `clock_gettime` (mach_absolute_time/gettimeofday), `strndup`, `strnlen`, `open_memstream` — 10.7+/10.12+ APIs; `steady_clock` needs `clock_gettime` |
| `math_compat.h` | 36 C99 math declarations missing from the 10.6 SDK's `<math.h>`; force-included for C++ (libc++ 5 `<cmath>` requires them) |
| `glmark2-cross-10.6.txt` | Meson cross file: target triple, SDK, libc++ includes, static libc++/libc++abi/`-force_load libc++experimental` link args |

C++17 is kept (not downgraded): libc++ 5 headers support it (proven by the
Mesa-VirGL build) and `scene-gl41.cpp` needs the non-const `std::string::data()`.

## Packaging

The binary has no compiled-in resource fork — at runtime it reads the
shaders, textures and models under `data/` (9.4 MB), resolved through
`--data-path` (default baked in by the build script as the relative
`data` directory). No installer is needed; ship a self-contained zip
instead:

    ./cross-compat/package-10.6.sh
    # -> dist/glmark2-<commit>-macos-10.6-x86_64.zip (binary + data/ + README)

Users unzip anywhere and run `./glmark2-macos` from the bundle root with
no flags. The README bundled inside documents the stack safeguards
(forced `--reuse-context`, disabled `glMapBuffer`, skipped exit
teardown) and their console notices.

## Runtime notes

- 10.6.8 caps at OpenGL 2.1. The macos-gl flavor defaults to a 3.2 core
  profile request but falls back to a legacy context automatically; with the
  10.6 SDK the profile attribute is skipped entirely. Classic scenes are
  GL 2.0/GLSL 1.10-1.20 era and run on the legacy path; the `gl41-*` scenes
  report "requires a core profile context" and are skipped.
- `-femulated-tls` is used (10.6 has no native TLS runtime); glmark2 itself
  is single-threaded and uses no `thread_local`.
- The binary was smoke-tested on Apple Silicon via Rosetta 2 (default
  benchmark suite renders correctly on a "GL 2.1 Metal" context). That does
  **not** prove 10.6.8 behaviour — test on real hardware.
