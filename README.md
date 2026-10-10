# Steam Remote Play for PS5

A native Steam Remote Play ("Steam Link") client for jailbroken PS5 consoles. The console
finds your PC running Steam, pairs with a PIN, and streams Big Picture Mode with hardware
video decoding and the DualSense — the TV Steam Link experience, on the PS5.

**Status: working.** Streams 1080p60 with audio and controller input, and is playable.
Not affiliated with Valve or Sony.

| | |
| --- | --- |
| **Tested on** | PS5 CFI-2015, firmware **13.20**, etaHEN + ShadowMount+ 1.7beta4 |
| **Video** | H.264, hardware decode (VideoDec2), up to 1080p60, scaled to fit |
| **Audio** | Opus → stereo |
| **Controller** | DualSense over ScePad, seen by Steam as a real PS5 controller |
| **Build host** | Linux or WSL; the PS5 payload SDK is fetched for you |

## Features

| Feature | State |
| --- | --- |
| LAN host discovery, PIN pairing, persisted pairing key | Verified on hardware |
| H.264 hardware decode, any stream resolution scaled to fit the screen | Verified |
| Mid-stream resolution changes | Verified |
| Opus stereo audio | Verified |
| DualSense input, reported to Steam as a PS5 controller (VID 054C / PID 0CE6) | Verified |
| Options + touchpad click → Steam menu; hold 1.5 s → leave the stream | Verified |
| Auto-connect to the last PC, settings UI, smoothing (jitter) buffer | Verified |
| Rumble and lightbar | Implemented, **unverified** — Steam's Identify/LED controls don't reach a streamed pad, so it needs a game to exercise |
| Gyro / accelerometer | Implemented, unverified |
| HEVC, 5.1 audio, up to 4 controllers | Implemented, unverified (H.264 / stereo / one pad are what's been used) |
| 120 Hz output | Implemented, **untested** — no 120 Hz display available; see below |
| Keyboard, mouse, microphone, HDR | Not implemented |
| Touchpad as a trackpad | Not possible over Remote Play — the host exposes no touchpad for a streamed controller. The touchpad *click* works as a bindable button |

### About 120 Hz

The client measures its real display refresh rate and advertises it during negotiation, so
the host streams at the rate the console can actually show (nothing is hard-coded — Steam's
own Remote Play settings decide, same as for bitrate). It also asks the console for 120 Hz
output at startup, which needs `attribute3 = 0x80040` in `sce_sys/param.json` (set) plus a
120 Hz panel and the system's **Enable 120 Hz Output** setting. On a 60 Hz display the
console declines with `UNAVAILABLE_OUTPUT_MODE` and the app stays at 60 Hz — the intended
fallback. The 120 Hz path has never run on a 120 Hz display, so treat it as unproven.

## Requirements

**Console** — a jailbroken PS5 (developed on firmware 13.20) with:
- a payload loader (etaHEN or equivalent)
- `ftpsrv` on port 2121 — used to deploy
- `ShadowMount+` **1.7beta4 or newer** — it mounts `/data` into the title sandbox (the app
  needs it for its settings and log) and registers homebrew on the home screen
- `shsrv` on port 2323 — optional, only for the command-line dev loop

**PC** — Steam with Remote Play enabled, on the same network.

**Build host** — Linux or WSL with `git`, `make`, `python3`, `curl`.

## Building

```bash
git clone --recursive https://github.com/<you>/steamlink-ps5
cd steamlink-ps5
make doctor     # checks the host for what the build needs, changes nothing
make            # fetches the SDK on first run, then builds
```

Produces `dist/PPSA99350/` (`eboot.bin` + `sce_sys/`) and `dist/PPSA99350.zip`.

## Installing

```bash
PS5_HOST=192.168.x.x make deploy
```

Uploads the title to `/data/homebrew/PPSA99350/` over FTP. ShadowMount+ picks it up within
about 15 seconds and it appears on the home screen. Copying `dist/PPSA99350/` there by hand
works just as well.

For the edit-build-test loop (needs `shsrv`), `tools/relaunch.sh` stops the title, uploads,
relaunches it and prints the log:

```bash
PS5_HOST=192.168.x.x tools/relaunch.sh
```

## Using it

1. On the PC: Steam → Settings → Remote Play → enable it.
2. Launch **Steam Remote Play** on the console. It finds the PC by itself.
3. First time only: Steam shows a PIN — type it on the console.
4. It connects and streams Big Picture.

While streaming, **Options + touchpad click** opens the Steam menu (the PS button belongs to
the console, so it can't be forwarded). Holding both for 1.5 s leaves the stream.

Press Options on the launcher for settings:

| Setting | Default | Meaning |
| --- | --- | --- |
| Resolution | 1080p | cap requested of the host |
| Bitrate cap | AUTO | advertises 100 Mbit so Steam's own bandwidth setting is the limit |
| Quality | Fast | host encoder preset |
| HEVC | Off | offer HEVC instead of H.264 (unverified) |
| Auto connect | On | reconnect to the last PC without asking |
| Smoothing buffer | 2 frames | frames held before display to absorb network jitter, ~17 ms each. 0 = lowest latency, more stutter |

The console writes `/data/steamlink-ps5/log.txt` (readable over FTP) and mirrors it to the
kernel log.

## Known issues and host-side gotchas

- **Keep the PC's display awake.** When it sleeps, Steam stops capturing and the stream goes
  black or hitches badly. This looks exactly like a client bug and isn't one.
- **Linux/Wayland hosts need `steam -pipewire`.** Without it Steam captures a black frame.
  Start Steam with that flag (and click *Share* on the monitor picker).
- Games see the forwarded pad as an XInput device; only Steam Input sees a DualSense. That is
  how Remote Play forwards controllers, not a limitation of this client.
- On a weak Wi-Fi link, packet loss shows up as brief artifacts rather than freezes — the
  client deliberately keeps decoding through small losses instead of blanking.

## How it works

- **Protocol** — [ihslib](https://github.com/mariotaku/ihslib), built from a patched copy;
  `patches/ihslib/` holds the changes and `PINS.md` explains every pinned revision.
- **Video** — VideoDec2 hardware decode → CPU NV12→RGBA (4 threads) → tiled blit to VideoOut,
  behind a small frame-pacing buffer.
- **Audio** — Opus → `sceAudioOut`.
- **Input** — ScePad behind an ihslib HID provider that advertises the DualSense VID/PID.

`docs/` covers the native tooling and PS5 platform notes.

## Credits

[ihslib](https://github.com/mariotaku/ihslib) (protocol) and its
[`auth` branch](https://github.com/scaronni/ihslib) for current Steam pairing;
[ProsperoLight](https://github.com/blackbearreloaded/ProsperoLight) and
[ps5-native-app-boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate)
for the PS5 platform layer and build system; protobuf-c, Mbed-TLS, Opus and TLSF.

## Licence

GPL-3.0-or-later for this repository's own code; see [LICENSE](LICENSE) and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). No Sony SDK, firmware or keys, and no Valve
code, are included or required. Build it for your own console.
