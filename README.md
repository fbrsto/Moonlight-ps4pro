# Moonlight PS4 Pro

Unofficial [Moonlight](https://moonlight-stream.org/) client for jailbroken PS4 (GoldHEN).
Fork of [JaimeJimenezG/Moonlight-ps4](https://github.com/JaimeJimenezG/Moonlight-ps4), improved for the PS4 Pro.

**Tested on PS4 Pro with GoldHEN 13.52, over Ethernet.**

## What's new

- Rumble, gyroscope, accelerometer, touchpad and light bar
- Automatic reconnect after a dropped stream
- In-stream menu: stats, frame pacing, bitrate, disconnect, quit game
- Steadier frame rate, corrected colors, stable memory
- Custom FPS, PC discovery and Wake PC

## Install

1. Install the `.pkg` from [Releases](https://github.com/fbrsto/Moonlight-ps4pro/releases).
2. Start Sunshine or Vibepollo on your PC.
3. Open the app and pick your PC, or choose **Add PC** and enter its address.
4. Enter the PIN at `https://<pc>:47990/pin`.

## Controls

| Where | Button | Action |
| --- | --- | --- |
| Menu | X / O | Select / back |
| Menu | R1 / L1 | Settings / Add PC |
| Menu | Triangle / OPTIONS | Refresh / context menu |
| Stream | OPTIONS + touchpad, about 1 s | In-stream menu |
| Stream | Finger on touchpad | Right stick moves the mouse, R2 and L2 click |

## Known issues

- **No controller after the PC wakes from sleep:** restart Vibepollo ([Nonary/Vibepollo#462](https://github.com/Nonary/Vibepollo/issues/462)).
- **No rumble with Vibepollo's Automatic gamepad type:** choose the DualShock 4 option of the Vibepollo Virtual Gamepad.
  Steam may then show Xbox buttons in some games.

## Build

See [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).
