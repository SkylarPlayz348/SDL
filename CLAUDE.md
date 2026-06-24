AI must not be used to generate code for contributions to this project.

"AI" in this case means a Large Language Model ("LLM"), such as ChatGPT,
Claude, Copilot, Grok, etc.

AI-generated code is based upon sources of unknown origins and may not be
compatible with the Zlib license, or may introduce conflicting license terms
if they include code from other projects.

AI can be used to identify issues with contributions to this project, but the
solutions to those issues should be authored by humans.

We have found that AI will frequently hallucinate issues that are not actually
problems in practice, report incorrect information, and describe problems that
are actually not issues at all. If AI identifies a problem with this codebase,
please make sure you understand what it is saying and have independently
confirmed that the issue exists before submitting a bug report or pull request.

Any pull request to this project will ask you to confirm that you are the
author and that you are contributing your changes under the Zlib license.

---

# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

> The policy above is from the SDL maintainers and takes precedence. The notes
> below are a local navigation aid only — do not author code contributions to
> SDL with AI. The same text is mirrored in `AGENTS.md`.

## What this fork is

This is **SDL-EP** (SDL2 E-Ink Patch): a fork of upstream `libsdl-org/SDL` (the
`SDL2` branch) that adds grayscale e-ink support for jailbroken Kindles (tested
on PW6 / KT / K5). It tracks upstream via periodic `Merge branch
'libsdl-org:SDL2'` commits; fork-specific work is authored on top. When working
here, keep changes minimal and upstream-compatible — most files are vanilla SDL2.

## Build

SDL2 has two parallel build systems; CMake is the primary one used here.

```sh
# Host build (native)
cmake -S . -B build -DSDL_TEST=ON
cmake --build build -j

# Cross-compile for Kindle (armv7hf). Requires a crosstool-NG toolchain at
# $HOME/x-tools/arm-kindlehf-linux-gnueabihf (see the TARGET/TOOLCHAIN vars in
# the toolchain files). Two variants:
cmake -S . -B build-kindle \
  -DCMAKE_TOOLCHAIN_FILE=armhf-toolchain-glx.cmake       # X11 + GLX/OpenGL
cmake -S . -B build-kindle \
  -DCMAKE_TOOLCHAIN_FILE=armhf-toolchain-software.cmake  # X11 software only (no GL)
cmake --build build-kindle -j
```

The toolchain files (`armhf-toolchain-*.cmake`) pin the cross sysroot,
pkg-config dirs, and force a curated set of `SDL_*` feature flags (X11 on; ALSA
on; PulseAudio/OSS/Vulkan off). The `-glx` variant enables OpenGL/GLX; the
`-software` variant disables all GL for pure framebuffer rendering.

The Autotools path (`./configure && make`, regenerate with `./autogen.sh`) also
exists upstream but is not the fork's focus. Per-platform build projects live in
`VisualC*/`, `Xcode*/`, `android-project/`, and the `Makefile.*` variants.

## Tests

There is no unit-test suite in the SDL2 sense; `test/` holds standalone manual
programs (`testsprite2`, `testgl2`, `testaudio`, etc.) built when `SDL_TEST=ON`.
Run an individual one after building, e.g. `./build/test/testsprite2`. The
`visualtest/` dir is an automated screenshot-diff harness driven separately.
`build-scripts/test-versioning.sh` checks ABI/version-number consistency.

## Architecture (relevant subsystems)

SDL2 is organized as a thin public API (`include/SDL_*.h`) over swappable
backend drivers selected at build/runtime. The pieces that matter for this fork:

- **`src/video/`** — the video subsystem. `SDL_sysvideo.h` defines the
  `SDL_VideoDevice` driver vtable; each subdirectory (`x11/`, `wayland/`,
  `kmsdrm/`, `cocoa/`, `windows/`, `dummy/`, `offscreen/`, …) is one backend.
  Kindle uses the **X11 backend** (`src/video/x11/`). Software framebuffer
  rendering goes through `SDL_x11framebuffer.c`.
- **`src/render/`** — 2D renderer backends (software, opengl, opengles2, …),
  separate from the low-level video framebuffer path above.
- **`src/audio/`** — driver-per-backend like video; Kindle uses **ALSA**
  (`src/audio/alsa/`).
- **`src/dynapi/`** — the dynamic API jump table. Every public symbol is routed
  through `SDL_dynapi.c` / `SDL_dynapi_overrides.h`; **adding or changing a
  public function requires updating the dynapi tables** (see
  `docs/README-dynapi.md`). Don't skip this when touching the public API.
- **`src/events/`**, `src/joystick/`, `src/haptic/`, `src/thread/` — same
  backend-vtable pattern.

## Fork-specific e-ink patches (where the custom logic lives)

The grayscale support is concentrated in the X11 backend. When debugging Kindle
display issues, start here:

- **`src/video/x11/SDL_x11framebuffer.c`** — the core hack. On `StaticGray`/
  `GrayScale` 8bpp visuals it keeps a private 8bpp Y8 buffer for the `XImage`
  while exposing an ARGB8888 surface to the app, then converts dirty rects to
  grayscale on every present using BT.601 luma (`(77*R + 150*G + 29*B) >> 8`).
- **`src/video/x11/SDL_x11window.h`** — adds `grayscale_buf` (Y8 XImage data,
  freed by `XDestroyImage`) and `argb_buf` (the app-facing ARGB surface) to the
  per-window struct.
- **`src/video/x11/SDL_x11modes.c`**, **`SDL_egl.c`**, **`src/audio/alsa/SDL_alsa_audio.c`**
  — smaller Kindle-targeted adjustments from the same patch series.

Memory ownership is subtle in the framebuffer path: `XDestroyImage` frees
`grayscale_buf` (it owns `ximage->data`), while `argb_buf` is freed separately.
Preserve that split when editing allocation/teardown.

A2 fast-refresh: the X11 framebuffer path supports optional A2/DU e-ink
waveforms via the `SDL_X11_EINK_WAVEFORM` hint (`a2`/`du`/`off`, default off).
When active, the per-rect grayscale conversion ordered-dithers to 1-bit and
`X11_eink_update()` drives the panel with `mxcfb` ioctls on `/dev/fb0`, with a
periodic GC16 de-ghost (`EINK_DEGHOST_EVERY`). The mxcfb struct/ioctl/waveform
constants in `SDL_x11framebuffer.c` are common i.MX/Kindle defaults marked
`TODO` — **verify against the target firmware's kernel `mxcfb.h` before
trusting on-device** (A2 index and the ioctl number are the likely mismatches).
Design notes: `EINK-A2-IMPLEMENTATION-PLAN.md`.

## Conventions

- C89-compatible C, 4-space indent, no tabs; formatting is enforced by
  `.clang-format` — run `build-scripts/clang-format-src.sh` before submitting.
- Internal source files take `_THIS` as the first parameter (expands to the
  driver/device pointer) — follow the surrounding signatures.
- `WhatsNew.txt`, `docs/README-*.md`, and `include/SDL_*.h` are the canonical
  references for platform behavior and the public API.

