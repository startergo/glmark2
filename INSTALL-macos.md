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

If you already have a Homebrew-installed `glmark2` (GLX/XQuartz-based), you can run it directly instead of building.

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
