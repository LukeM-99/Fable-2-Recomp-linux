# Linux build (Vulkan, AppImage)

The Linux port runs the same recompiled game on the SDK's Vulkan GPU backend.
It ships as a single `Fable2-Recompiled-x86_64.AppImage` with a settings
launcher. As on Windows, **you must supply the game yourself** (Fable II GOTY
USA/Europe disc image, SHA-256 in the main README); nothing from the disc is
in this repo or the AppImage.

## Running the AppImage

```sh
chmod +x Fable2-Recompiled-x86_64.AppImage
./Fable2-Recompiled-x86_64.AppImage
```

The launcher opens first. On the first run it asks for your `.iso`, then
extracts `default.xex` and `data/` into the game folder. Press **Play** to start.

**Game folder** (content, saves, shader cache, config, logs). The AppImage is
read-only, so these live in a writable folder:

1. `$FABLE2_HOME`, if set.
2. The folder holding the `.AppImage`, if `default.xex` is next to it
   (portable install).
3. Otherwise `~/.local/share/fable2-recomp`.

The launcher's **Open game folder** button opens it. Back up `saves/` to keep
your progress.

**Command-line options:**

| Option | Effect |
|---|---|
| `--launcher` | Always show the launcher (undoes "Skip this launcher next time"). |
| `--no-launcher` | Start the game directly. |
| `--extract-iso <disc.iso>` | Extract the game without the launcher. |

**Requirements:** x86-64 CPU with SSE4.2 (x86-64-v2), a working Vulkan driver
(Mesa or NVIDIA), FUSE (`fusermount`), and `python3` for the ISO
extraction. Without FUSE, use
`--appimage-extract-and-run`.

## Launcher settings

The settings are grouped into tabs:

- **Display:** display mode, VSync, frame rate cap.
- **Graphics:** internal resolution (including sub-720p 16:9 modes), filtering,
  FXAA, motion blur.
- **Performance:** shader compilation, memexport readback, occlusion queries.
- **Game:** intro skip, audio.
- **Controls:** mouse look and keymap.

**Presets:**

- **Performance:** 800x448 with no MSAA, readback off, motion blur off,
  30 FPS cap.
- **Balanced** and **Quality** step up from there.

The launcher writes `fable_2.toml` and `fable2_config.toml` in the game folder.
Its own preferences go in `launcher.toml`.

The sub-720p modes (`main_scene_resolution` = `WxHxMSAA`) resize the game's own
main-scene render target. They take effect once the main menu has loaded.

## Linux-specific changes

### Recomp

- **Game folder:** `$FABLE2_HOME` is honoured for content, saves, cache and logs.
- **Idle loops:** the game's idle and frame-limiter loops sleep instead of
  spinning.
- **Function-call trace hook:** now optional (`FABLE2_FUNC_TRACE`, off on Linux).
  It cost a check at every guest function entry.
- **Diagnostics:** POSIX ports of the diagnostics.
- **Launcher:** the SDL3/ImGui launcher in `src/launcher/linux_launcher.cpp`.

### SDK (`thirdparty/rexglue-sdk-linux.patch`, applied by `build-linux.sh`)

**Memory:**

- `memory_posix`: the guest-memory shm object is unlinked after creation, so
  crashed runs no longer fill `/dev/shm` and cause SIGBUS.
- Page protections are kept in a shadow table rather than re-parsing
  `/proc/self/maps` on every fault.

**Threading:**

- The timer queue blocks instead of spinning.
- `WaitMultiple` sleeps with microsecond precision.
- Fixed the disruptorplus `blocking_wait_strategy` argument order.

**GPU:**

- `execute_unclipped_draw_vs_on_cpu` defaults to on. Draw bounds are estimated,
  which cuts EDRAM render-target transfer traffic about threefold. This was the
  biggest GPU win.
- Command processor fast paths: a known-register bitset, batched type-0
  register writes, and a memexport readback check.
- Vulkan: shared-memory uploads not used earlier in a submission are hoisted
  into one prologue copy (`vulkan_hoist_shared_memory_uploads`). This means
  about 25% fewer barriers and render passes.

**Build:**

- SDL_RENDER is enabled for the launcher.
- Optional large code model.

**Performance (Intel UHD 620, Performance preset)** (full write-up in the README section "Linux performance notes"): gameplay went from about
3.4 FPS to about 11 FPS. Menus run at 22–24 FPS. On this GPU the remaining
limit is GPU time on the game's own draws plus EDRAM render-target transfers.
Faster GPUs will do much better.

## Building from source

Debian/Ubuntu/Devuan, no root needed:

```sh
git clone --recursive -b linux <this repo> && cd Fable-2-Recomp
tools/setup-linux-toolchain.sh     # LLVM 20.1.8, libc++, ninja, appimagetool, -dev headers
cp /path/to/default.xex .          # codegen input (from your own disc)
./build-linux.sh                   # Release build of fable_2 (+ fable2_launcher)
./make-appimage.sh                 # -> out/appimage/Fable2-Recompiled-x86_64.AppImage
```

Host needs: `cmake` 3.25 or newer, `python3`, `curl`, `git`, plus the usual
X11/Wayland/Vulkan runtime libraries. On other distros, install the `-devel`
packages for alsa, pulseaudio, pipewire, xcb, xss, udev and libdecor, then run
`tools/setup-linux-toolchain.sh --no-sysroot`.

libc++ is rebuilt with `LIBCXXABI_USE_LLVM_UNWINDER=OFF`. The LLVM release's
libc++abi uses LLVM libunwind, glibc's `pthread_exit` unwinds with `libgcc_s`,
and the mix crashes on every thread exit.

**Other build commands:**

- `./build-linux.sh -debug`: Debug build.
- `./build-linux.sh <target>`: build any other CMake target.
- `./build-linux.sh -clean`: wipe the build directory.
- `NINJA_ARGS=-j4`: limit parallel jobs.

**Test builds:** `CMAKE_EXTRA_ARGS=-DFABLE2_FORCE_REMOTE_CONTROL=ON ./build-linux.sh`
compiles in the remote-control server (localhost:8791, see the main README) for
automated testing. A plain `./build-linux.sh` always turns it off again. Never
ship a test build.

## Diagnostics (environment variables)

| Variable | Output |
|---|---|
| `FABLE2_FPS_LOG=1` | `[fps]` guest frame-time lines every 2 s. |
| `FABLE2_PROFILE=1` | Per-thread CPU sampling profile in `profile.txt`. Symbolize it with `tools/symbolize_profile.py profile.txt out/build/linux-release`. |
| `FABLE2_GPU_WAIT_LOG=1` | Per frame: GPU fence waits, render-target transfers, IssueDraw phase costs, Vulkan command counts. |
| `FABLE2_F5_LOG=1` | Mirror F5 Lua output into the log. |

Logs go to `<game folder>/logs/`.

**Known issues:**

- Do not toggle `clock_no_scaling` while the game is running: it blacks out
  the game.
- A symlinked `data/` folder makes the game fault-loop at boot. Use real files
  or hardlinks.
