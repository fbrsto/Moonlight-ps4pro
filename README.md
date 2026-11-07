# moonlight-ps4

Unofficial [Moonlight](https://moonlight-stream.org/) port (Sunshine/GameStream
game-streaming client) for jailbroken PS4 (GoldHEN), built on the
[OpenOrbis](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) toolchain.

Fork of [JaimeJimenezG/Moonlight-ps4](https://github.com/JaimeJimenezG/Moonlight-ps4), focused on
the PS4 Pro: rumble, motion, reconnect, in-stream menu, steadier frames, stable memory.

Full port plan: [PLAN.md](PLAN.md).

**Current version: 1.3.13**

## Status

Playable client for jailbroken PS4 (GoldHEN / FW 9.00 validated):

- [x] Pairing, applist, launch, and H.264 streaming against Sunshine
- [x] Audio (Opus + `sceAudioOut`)
- [x] Software video (FFmpeg) and hardware decode (`libSceVideodec2`, `prefer_hw`)
- [x] DualShock 4 input (`scePad`)
- [x] On-console UI (PCs / app grid / Settings + on-screen keyboard) and INI config
- [x] Installable `.pkg` (`CATEGORY=gd`)
- [x] Optional performance overlay and Videodec2 / BGRA tuning knobs

### TODO

- [ ] Keyboard and mouse support
- [ ] Native YCbCr/NV12 presentation on `sceVideoOut`

### Known issues

- **No rumble with Vibepollo's Automatic gamepad type (seen on 2.0.6):** Automatic presents a
  DualSense, and rumble did not reach the console in The Witcher 3. Set `Emulated Gamepad Type` to
  the DualShock 4 option of the Vibepollo Virtual Gamepad. Steam may then warn about the controller
  and show Xbox buttons in some games: The Witcher 3 did, 007 First Light kept PlayStation buttons.

- **No controller in games after the host PC wakes from sleep (Vibepollo 2.0.6):** the host's
  virtual gamepad driver can fail after a wake, and its log shows `VHF keyboard/mouse stopped
  after driver error [00000651]`, then `VHF controller allocation deferred while recovering a
  removed device`. Reconnecting does not help: the host still refused the controller 4 minutes
  later. Reading Vibepollo's source, its recovery thread sleeps until a controller exists, while
  controllers are refused until recovery ends. Restarting Vibepollo brings the controller back.
  Its issue is [Nonary/Vibepollo#462](https://github.com/Nonary/Vibepollo/issues/462).
  `keyboard_mouse_backend = sendinput` on the host may avoid this; it is untested.

Full port plan and longer backlog: [PLAN.md](PLAN.md). Validation notes: [docs/CONSOLE_VALIDATE.md](docs/CONSOLE_VALIDATE.md).

## Requirements (development)

- Linux x86_64 with `clang`, `ld.lld`, `cmake`, `ninja`, `git`, and `curl`
- OpenOrbis toolchain v0.5.4 (default: `~/ps4dev/OpenOrbis/PS4Toolchain`)
- Cross-built FFmpeg for PS4 (H.264 decoder only): `scripts/build_ffmpeg_ps4.sh`
- For `PkgTool.Core`: `libssl.so.1.1` / `libcrypto.so.1.1` in `~/ps4dev/hostlibs/usr/lib`
- PS4 with GoldHEN-compatible firmware (plan validated on **9.00**)
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

## Usage on PS4

1. Install the `.pkg` (Package Installer / GoldHEN / FTP). The package uses
  `CATEGORY=gd` (required so GoldHEN injects plugins).
2. On first launch choose **Add PC** and enter the PC's address. You can
  also create `/data/moonlight/moonlight.ini` over FTP (see
   `pkg/assets/misc/moonlight.ini`). Optional: `debug_host.txt` with your PC IP.
3. Start Sunshine on the PC. On first pair the app shows a PIN (also written to
  `/data/moonlight/pin.txt`); enter it at `https://<pc>:47990/pin`.
4. Logs on the PC: `nc -ulk 9999` (or enable file log in Settings →
  `/data/moonlight/debug.log`).
5. In the menu: **X** select or start, **O** back, **R1** Settings, **L1** Add PC,
  **Triangle** refresh, **OPTIONS** context menu. In a stream: **OPTIONS + Touchpad** (~1 s) opens a menu (stats, frame pacing, bitrate, disconnect, quit game);
  a finger on the touchpad makes the right stick move the mouse, with **R2** and **L2** as clicks.
  A dropped connection reconnects by itself (5 attempts); **O** goes back to the menu.



### Defaults


| Setting          | Default                                                        |
| ---------------- | -------------------------------------------------------------- |
| Resolution / FPS | 1920×1080 @ 60                                                 |
| Bitrate          | 20 Mbps, H.264                                                 |
| `prefer_hw`      | `true` (Videodec2)                                             |
| Title ID         | `MLNT00001`                                                    |


### Settings shared with the official client

The menu follows the official client's pages, sizes and Material dark theme (Computers,
app grid with box art, two-column Settings) and finds PCs by mDNS (`_nvstream._tcp`).
Settings: resolution (540p to 1080p; 1440p and 4K are experimental, decoded and scaled
down to the 1080p output), FPS (30, 60 or a custom value from 10 to 60),
bitrate (default per mode, up to 150 Mbps, or 500 Mbps unlocked), V-Sync, frame pacing, stereo/5.1/7.1
audio, mute host speakers, swap face/mouse buttons, one gamepad per
logged-in PS4 user, touchpad as mouse, rumble, motion controls, touchpad touches, light bar, optimize
game settings, quit app after stream, keep console awake, video decoder, performance
stats, and connection/configuration warnings.

`src/ui/ui_assets.{c,h}` are generated from `assets-src/` (Roboto, Apache 2.0; icons from
moonlight-qt, GPLv3) by `scripts/bake_ui_assets/bake.ps1`, which needs Windows.
`third_party/stb/stb_image.h` decodes box art.

Mean decode/convert/present times are printed every second over UDP.
Validation protocol: `[docs/CONSOLE_VALIDATE.md](docs/CONSOLE_VALIDATE.md)`.

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
