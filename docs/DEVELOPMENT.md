# Development

## Requirements (development)

- Linux x86_64 with `clang`, `ld.lld`, `cmake`, `ninja`, `git`, and `curl`
- OpenOrbis toolchain v0.5.4 (default: `~/ps4dev/OpenOrbis/PS4Toolchain`)
- Cross-built FFmpeg for PS4 (H.264 decoder only): `scripts/build_ffmpeg_ps4.sh`
- For `PkgTool.Core`: `libssl.so.1.1` / `libcrypto.so.1.1` in `~/ps4dev/hostlibs/usr/lib`
- PS4 with GoldHEN
- Host PC running [Sunshine](https://github.com/LizardByte/Sunshine)

## Dependencies (third_party)

Vendored libraries live as **git submodules** pinned in [`third_party/DEPS`](third_party/DEPS).
Git only stores the commit SHA for each submodule — not a full copy of upstream.
Orbis-specific fixes are **not** committed inside those repos; they live under
[`patches/`](patches/) and are applied after checkout.

```bash
git clone <this-repo>
scripts/setup_deps.sh    # checkout pins from third_party/DEPS + apply patches/
```

Equivalent manual flow: `git submodule update --init --recursive` then
`scripts/apply_patches.sh`. After patches, `third_party/*` working trees look
dirty — that is expected; commit patch files, not submodule dirt.

To bump a dependency on purpose: edit the pin in `third_party/DEPS`, update the
submodule to that commit, refresh patches if needed (`scripts/refresh_patches.sh`),
and commit the new gitlink + DEPS (+ patches).

## Build

```bash
git clone <this-repo>
cd moonlight-ps4
scripts/setup_deps.sh                # pinned third_party + Orbis patches
scripts/build_ffmpeg_ps4.sh          # once (→ ~/ps4dev/ffmpeg-ps4)

# PS4 app + .pkg (sources env.sh, configures, builds, packages)
scripts/build_pkg.sh
# scripts/build_pkg.sh --clean       # wipe build-ps4/ and reconfigure

# Optional: Linux development CLI (pairing / H.264 dump)
cmake -B build-host -G Ninja && cmake --build build-host
```

Installable artifact: `build-ps4/Moonlight-1.3.13.pkg`
(also `IV0000-MLNT00001_00-MOONLIGHTPS40000.pkg`).

`build_pkg.sh` runs `source scripts/env.sh` itself. Manual CMake equivalent:

```bash
source scripts/env.sh
cmake -B build-ps4 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/openorbis.cmake
cmake --build build-ps4
```

`src/ui/ui_assets.{c,h}` are generated from `assets-src/` (Roboto, Apache 2.0; icons from
moonlight-qt, GPLv3) by `scripts/bake_ui_assets/bake.ps1`, which needs Windows.
`third_party/stb/stb_image.h` decodes box art.

Mean decode/convert/present times are printed every second over UDP.
Validation protocol: [CONSOLE_VALIDATE.md](CONSOLE_VALIDATE.md).

Isolated Videodec2 spike: `videodec2_spike = true` in the INI.

## Host CLI (Linux)

```bash
./build-host/moonlight-cli pair 192.168.1.100
./build-host/moonlight-cli list 192.168.1.100
./build-host/moonlight-cli stream 192.168.1.100 --res 1280x720 --time 20
./build-host/moonlight-cli quit 192.168.1.100
```

## Layout

```
cmake/openorbis.cmake           CMake toolchain for PS4
pkg/                            package assets (icon0, sce modules)
scripts/                        env, cross FFmpeg, .pkg packaging
src/
  main.c stream.c config.c      orchestration + INI config
  ui/                           on-console menu (APPS / SETTINGS + OSK)
  gamestream/                   pairing/HTTP/XML over mbedTLS
  audio/audio_orbis.c           Opus + sceAudioOut
  video/renderer_videoout.c     YCbCr/NV12 or BGRA presentation (sceVideoOut)
  video/nv12_blit.c             1088→1080 copy (WB_GARLIC / Onion alias)
  video/decoder_ffmpeg.c        H.264 software (FFmpeg)
  video/decoder_orbis.c         H.264 hardware (libSceVideodec2)
  gamestream/sps.c              SPS fixup (num_ref_frames=1)
  input/input_pad.c             DualShock 4
  orbis/                        VideoOut / Videodec2 / net helpers
  host/main_cli.c               Linux development CLI
plugin/
  ycbcr_unlock/                 GoldHEN PRX (1.63) for YCbCr VideoOut
  ycbcr_kpatch/                 FW 9.00 kernel kpayload (BinLoader)
  kernel_dumper_900/            kernel dump helper for RE
docs/                           console validation + kernel YCbCr RE notes
third_party/
  DEPS                        pinned tags/SHAs (lockfile for setup_deps.sh)
  moonlight-common-c          protocol engine (submodule + Orbis patches)
  mbedtls                     crypto (v3.6.4)
  opus                        audio
  h264bitstream               SPS rewrite
patches/                      Orbis patches applied by scripts/apply_patches.sh
scripts/setup_deps.sh         checkout pins + apply patches
scripts/apply_patches.sh
scripts/refresh_patches.sh    regenerate patches after local dep edits
```
