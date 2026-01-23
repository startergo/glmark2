# INSTALL (macOS)

This document describes how to build and run **glmark2** on macOS.

It covers:
- Native macOS/Cocoa backend: `glmark2-macos` (no XQuartz required)
- Optional XQuartz/GLX backend (for comparison/testing)

## Prerequisites

- macOS 12+ recommended
- Xcode Command Line Tools:
  - `xcode-select --install`
- Homebrew (recommended for dependencies)

Install build tools and common deps:

```sh
brew install meson ninja pkg-config libpng jpeg
```

Notes:
- This repo vendors several libraries under `src/` (glad, libpng, zlib, etc.), but the macOS build commonly links to system/brew libraries for image loading.

## Install from prebuilt archives (GitHub Releases / CI artifacts)

If you don’t want to build from source, you can install a prebuilt tarball produced by this repo’s CI.

### Keep the archive from auto-extracting

Some browsers (notably Safari) can automatically “open” downloaded archives.

- Safari: disable **Safari → Settings… → General → “Open ‘safe’ files after downloading”**.
- If your browser still auto-extracts, prefer downloading via the command line (examples below).

### 1) Download the right archive

First, determine your macOS architecture:

```sh
uname -m
```

- `arm64` → Apple Silicon (M1/M2/M3/M4)
- `x86_64` → Intel

Then download either:

- A published **GitHub Release** asset, or
- A workflow-run **Actions artifact** (downloaded from the run page)

You should end up with a file like one of these:

- Native Cocoa backend (no XQuartz):
  - `glmark2-macos-native-arm64.tar.gz`
  - `glmark2-macos-native-x86_64.tar.gz`
- XQuartz/GLX backend:
  - `glmark2-macos-arm64.tar.gz`
  - `glmark2-macos-x86_64.tar.gz`

#### Download via CLI (recommended)

Using `curl` will keep the archive intact:

```sh
curl -L -o glmark2.tar.gz "https://.../glmark2-macos-native-arm64.tar.gz"
```

If you have the GitHub CLI installed, you can also download from a release without using the browser:

```sh
gh release download --repo <OWNER>/<REPO> --pattern "glmark2-macos-native-*.tar.gz"
```

### 2) Install the archive (system-wide)

These archives contain a prefix like `opt/homebrew/...` (Apple Silicon) or `usr/local/...` (Intel).

For safety, avoid extracting an unverified tarball directly into `/` as root.

#### Verify integrity (recommended)

If the release/run provides a SHA-256 checksum file, verify it before installing:

```sh
shasum -a 256 /path/to/the-archive.tar.gz
# compare the output to the published checksum
```

#### Safer install via staging directory (recommended)

1) Extract to a temporary staging directory as your user:

```sh
STAGE="$(mktemp -d)"
tar -xzf /path/to/the-archive.tar.gz -C "$STAGE"
```

2) Inspect the contents (make sure it only contains the expected prefix):

```sh
find "$STAGE" -maxdepth 3 -type d | head
```

3) Copy only the expected prefix into place:

```sh
# Apple Silicon archives (typically use /opt/homebrew)
if [ -d "$STAGE/opt/homebrew" ]; then
  sudo rsync -a --delete "$STAGE/opt/homebrew/" /opt/homebrew/
fi

# Intel archives (typically use /usr/local)
if [ -d "$STAGE/usr/local" ]; then
  sudo rsync -a --delete "$STAGE/usr/local/" /usr/local/
fi
```

4) Clean up:

```sh
rm -rf "$STAGE"
```

### 3) Remove Gatekeeper quarantine

macOS often marks downloaded binaries as quarantined. Remove that attribute from the installed binary:

```sh
# Native Cocoa backend
sudo /usr/bin/xattr -cr /opt/homebrew/bin/glmark2-macos 2>/dev/null || true
sudo /usr/bin/xattr -cr /usr/local/bin/glmark2-macos 2>/dev/null || true

# XQuartz/GLX backend
sudo /usr/bin/xattr -cr /opt/homebrew/bin/glmark2 2>/dev/null || true
sudo /usr/bin/xattr -cr /usr/local/bin/glmark2 2>/dev/null || true
```

### 4) Run

- Native Cocoa backend:

```sh
glmark2-macos --debug
```

- XQuartz/GLX backend (requires XQuartz + DISPLAY):

```sh
export DISPLAY=:0
glmark2 --debug
```

Note: Homebrew does not currently ship a `glmark2` formula, so Releases/CI artifacts (or building from source) are the intended installation paths.

## Build: native macOS backend (Cocoa + NSOpenGL)

This produces `glmark2-macos`.

From the repository root:

```sh
meson setup build-macos --prefix="$HOME/.local" -Dflavors=macos-gl
ninja -C build-macos
ninja -C build-macos install
```

The `install` step places assets under:
- `$HOME/.local/share/glmark2`

And the executable typically at:
- `$HOME/.local/bin/glmark2-macos`

If `$HOME/.local/bin` is not on your `PATH`, you can run it via:

```sh
"$HOME/.local/bin/glmark2-macos" --help
```

## Run: native macOS backend

### Default (core profile)

The native backend requests an OpenGL **3.2 core** profile by default.

```sh
glmark2-macos
```

### Select OpenGL profile (macOS-only)

You can choose between:
- `core` (default): requests `NSOpenGLProfileVersion3_2Core`
- `legacy`: requests `NSOpenGLProfileVersionLegacy` (OpenGL 2.1)

Examples:

```sh
# 3.2 core profile (default)
glmark2-macos --macos-gl-profile core

# 2.1 legacy profile
glmark2-macos --macos-gl-profile legacy
```

### Useful run flags

```sh
# Off-screen rendering (reduces compositor/present effects)
glmark2-macos --off-screen

# Force a specific window size
glmark2-macos --size 1920x1080

# End each frame with glFinish (useful for timing comparisons)
glmark2-macos --frame-end finish

# Quick output validation
glmark2-macos --validate
```

### macOS-only GL4.x feature scenes

When you build the native backend (`-Dflavors=macos-gl`) these extra scenes are included by default on core profile and automatically skipped on legacy profile.

Scenes:
- `gl41-instancing` (instancing + UBO)
  - Options: `instances` (default 4096)
- `gl41-pipeline` (separate shader objects + program pipeline, 3D instanced grid)
  - Options: `grid` (default 10, clamps 1..16)
- `gl41-mrt` (MRT + per-target blend state)
  - Options: `targets` (default 4, clamps 1..4), `size` (default 1024, clamps 64..4096)
- `gl41-texarray` (texture arrays + LOD/bias)
  - Options: `instances` (default 4096), `layers` (default 64), `tex-size` (default 64), `bias` (default 0.75)
- `gl41-streaming-sync` (streaming buffer updates + sync objects)
  - Options: `bytes` (default 4194304)
- `gl41-geometry` (geometry shader amplification)
  - Options: `points` (default 65536), `size` (default 0.012)

Examples:

```sh
glmark2-macos --benchmark gl41-pipeline:grid=12
glmark2-macos --benchmark gl41-mrt:targets=4:size=2048
glmark2-macos --benchmark gl41-texarray:layers=128:tex-size=128:bias=1.0
glmark2-macos --benchmark gl41-streaming-sync:bytes=16777216
```

## Build: XQuartz/GLX backend (optional)

This path uses X11/GLX and requires XQuartz.

### Install XQuartz

- Install from: https://www.xquartz.org/
- Launch **XQuartz** at least once.

In your terminal:

```sh
export DISPLAY=:0
```

### Build

On macOS, GLX builds typically need XQuartz/X11 headers and libraries.

Install XQuartz and (optionally) pkg-config helpers:

```sh
brew install pkg-config
```

Then configure/build a GLX flavor with Meson (exact flavor names depend on the repo configuration):

```sh
meson setup build-x11 --prefix="$HOME/.local" -Dflavors=x11-gl
ninja -C build-x11
ninja -C build-x11 install
```

If you want a prebuilt binary instead of building from source, see **Install from prebuilt archives** above.

### Run

```sh
export DISPLAY=:0
glmark2 --debug
```

If you run the GLX binary directly from the build directory (without installing),
it may look for assets under `/usr/local/share/glmark2` and fail to find shaders.
In that case, point it at the repo's `data/` directory:

```sh
export DISPLAY=:0
/path/to/build-x11/src/glmark2 --data-path /path/to/glmark2/data
```

To print the OpenGL information block:

```sh
glmark2 --debug 2>&1 | sed -n '/OpenGL Information/,+15p'
```

## Notes on performance comparisons

Even when both paths report the same `GL_VENDOR/GL_RENDERER/GL_VERSION`, scores can differ due to window-system and present/swap behavior.

To tighten comparisons:

```sh
# Reduce present/compositor influence
... --off-screen

# Force GPU completion per frame
... --frame-end finish
```

## Troubleshooting

### Assets not found (shaders/textures/models)

If you run a freshly built binary from the build tree without installing, the default data path may not match.

Recommended fix:

```sh
ninja -C build-macos install
```

Or override at runtime:

```sh
glmark2-macos --data-path /path/to/glmark2/data
```

### White window when using `--frame-end none`

`--frame-end none` disables the usual end-of-frame operations (including presenting/swapping), so the window may not update.

Use `--frame-end swap` (default) or `--frame-end finish` instead.

### XQuartz: “Could not initialize canvas” / no display

Make sure:

- XQuartz is running
- `DISPLAY` is set:

```sh
export DISPLAY=:0
```

### Swap interval warning on GLX

If you see messages like:

- `GLX does not support GLX_EXT_swap_control ... Failed to set swap interval`

that indicates the GLX/XQuartz stack can’t control vsync via those extensions. Use `--frame-end finish` or `--off-screen` for more consistent comparisons.
