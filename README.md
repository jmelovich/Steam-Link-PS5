# Steam Remote Play for PS5

Stream your PC's Steam library to a jailbroken PS5. The console finds your PC, pairs with a
PIN, and streams Big Picture Mode with hardware video decoding and the DualSense — the TV
Steam Link experience, running natively on the PS5.

Unofficial and not affiliated with Valve or Sony.

```
PC running Steam  ──►  LAN  ──►  PS5 homebrew app  ──►  your TV
```

## What works

Tested on a PS5 **CFI-2015, firmware 13.20**, with etaHEN and ShadowMount+ 1.7beta4.

| | |
| --- | --- |
| **Video** | H.264, hardware decoded, up to 1080p60. Any stream resolution is scaled to fit, including when it changes mid-stream. |
| **Audio** | Opus, stereo. |
| **Controller** | DualSense over ScePad. Steam sees a genuine PS5 controller (VID `054C` / PID `0CE6`), so you get PlayStation glyphs and correct bindings. |
| **Pairing** | LAN discovery, PIN pairing, and the key is remembered. Auto-connects to the last PC. |
| **In-stream** | Options + touchpad click opens the Steam menu. Hold both 1.5 s to quit the stream. |
| **Smoothness** | A configurable frame buffer absorbs network jitter. |

Implemented but **not verified** — they are wired up and should work, but nothing has
confirmed them on hardware: rumble, lightbar, gyro/accelerometer, HEVC, 5.1 audio, more than
one controller, and 120 Hz output (never run on a 120 Hz display; on a 60 Hz panel the console
declines and the app stays at 60, which is the intended fallback).

Not implemented: keyboard, mouse, microphone, HDR.

Not possible: using the touchpad as a trackpad. Remote Play gives the host no touchpad for a
streamed controller. The touchpad *click* works as a normal bindable button.

## What you need

**On the PS5** — a jailbroken console (developed on firmware 13.20) running:

- a payload loader such as etaHEN
- **ShadowMount+ 1.7beta4 or newer** — required; it puts `/data` inside the app's sandbox and
  registers homebrew on the home screen
- `ftpsrv` on port 2121 — to copy the app across

**On the PC** — Steam with Remote Play enabled, on the same network.

## Install

Download the latest `PPSA99350.zip` from [Releases](../../releases) and unpack it into
`/data/homebrew/` on the console, so you end up with:

```
/data/homebrew/PPSA99350/
├── eboot.bin
├── sce_module/libc.prx
├── sce_sys/
└── assets/
```

Any FTP client works (`ftpsrv`, port 2121). ShadowMount+ notices the new title within about
15 seconds and it appears on the home screen. There is no firmware requirement baked into the
app, so the same build works on any firmware your jailbreak supports.

Building it yourself instead? See [Building](#building).

## Using it

1. On the PC: **Steam → Settings → Remote Play** → enable it.
2. Launch **Steam Remote Play** on the console. It finds the PC on its own.
3. First time only: Steam shows a PIN — type it on the console.
4. It connects and streams Big Picture.

While streaming, **Options + touchpad click** opens the Steam menu (the PS button belongs to
the console and can't be forwarded). Hold both for 1.5 seconds to leave the stream.

Press Options on the launcher for settings:

| Setting | Default | Meaning |
| --- | --- | --- |
| Resolution | 1080p | cap requested of the host |
| Bitrate cap | AUTO | advertises 100 Mbit so Steam's own bandwidth setting decides |
| Quality | Fast | host encoder preset |
| HEVC | Off | offer HEVC instead of H.264 (unverified) |
| Auto connect | On | reconnect to the last PC without asking |
| Smoothing buffer | 2 frames | frames held back to absorb network jitter, ~17 ms each. 0 = lowest latency, more stutter |

## Known issues

- **Keep the PC's display awake.** When it sleeps, Steam stops capturing and the stream goes
  black or hitches badly. This looks exactly like a bug in the app and is not one.
- **Linux/Wayland hosts need `steam -pipewire`**, or Steam captures a black frame. Start Steam
  with that flag and click *Share* on the monitor picker.
- Games see the forwarded pad as an XInput device; only Steam Input sees a DualSense. That is
  how Remote Play forwards controllers everywhere, not a limitation of this app.
- On a weak Wi-Fi link, loss shows up as brief artifacts rather than freezes — the client
  deliberately decodes through small losses instead of blanking the picture.
- Every stream logs a few decoder resets in its first seconds while the first keyframes land.
  Harmless and self-correcting.

## Building

Linux or WSL, with `git`, `make`, `python3` and `curl`.

```bash
git clone --recursive https://github.com/<you>/steamlink-ps5
cd steamlink-ps5
make doctor     # checks the host for what the build needs; changes nothing
make            # fetches the PS5 payload SDK on first run, then builds
```

Output is `dist/PPSA99350/` plus `dist/PPSA99350.zip` — the same layout as a release.

Deploy straight to a console over FTP:

```bash
PS5_HOST=192.168.x.x make deploy
```

For the edit-build-test loop (needs `shsrv` on port 2323), `tools/relaunch.sh` stops the title,
uploads, relaunches and tails the log:

```bash
PS5_HOST=192.168.x.x tools/relaunch.sh
```

The console writes `/data/steamlink-ps5/log.txt`, readable over FTP, and mirrors it to the
kernel log.

## How it works

- **Protocol** — [ihslib](https://github.com/mariotaku/ihslib), built from a patched copy.
  `patches/ihslib/` holds every change and [PINS.md](PINS.md) explains each one, plus why each
  dependency is pinned where it is.
- **Video** — VideoDec2 hardware decode → CPU NV12→RGBA (4 threads) → tiled blit to VideoOut,
  behind a small frame-pacing buffer.
- **Audio** — Opus → `sceAudioOut`.
- **Input** — ScePad behind an ihslib HID provider advertising the DualSense VID/PID.

`docs/` covers the native tooling and PS5 platform notes.

## Credits

[ihslib](https://github.com/mariotaku/ihslib) for the protocol, and its
[`auth` branch](https://github.com/scaronni/ihslib) for current Steam pairing;
[ProsperoLight](https://github.com/blackbearreloaded/ProsperoLight) and
[ps5-native-app-boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate)
for the PS5 platform layer and build system; protobuf-c, Mbed-TLS, Opus and TLSF.

## Licence

GPL-3.0-or-later for this repository's own code — see [LICENSE](LICENSE) and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). The `sce_module/libc.prx` shipped with the
app is generated from the clean-room source in `runtime/`.

No Sony SDK, firmware or keys, and no Valve code, are included or required.
