# glmark2 for macOS 10.6 (Snow Leopard) — x86_64 bundle

Self-contained benchmark bundle: the `glmark2-macos` binary plus the
shaders, textures and models it needs (`data/`). No installation step —
unzip and run.

## Running

From inside the bundle directory:

    ./glmark2-macos                 # full default suite

The compiled-in data path is the relative `data` directory, so run from
the bundle root; from anywhere else pass `--data-path /path/to/data`.

Useful invocations:

    ./glmark2-macos -b build:duration=5        # single scene
    ./glmark2-macos --list-scenes
    ./glmark2-macos --help

## Expected console notices on this stack

These are deliberate safeguards for the 10.6 software-GL stack
(GLEngine over a gld float whose buffer/surface entry points are
success-returning stubs), not errors:

- `Forcing --reuse-context` — per-scene GL context destroy frees
  engine-owned state that is still live; one context is kept for the
  whole run instead.
- `glMapBuffer disabled on this stack` — VBO updates use
  glBufferSubData; the `buffer:update-method=map` benchmark variants
  report Unsupported.
- `Skipping GL context teardown` — printed at exit; the context is
  leaked by design so the process ends with exit status 0 instead of
  aborting after the score is printed.

## Requirements

macOS 10.6.8 x86_64. The binary links libc++ statically and depends
only on frameworks that ship with 10.6 (AppKit, Foundation, libobjc,
libSystem, libz); OpenGL.framework is opened at runtime.
