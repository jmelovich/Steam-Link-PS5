# steamlink-ps5: plan

A native Steam Remote Play client for a jailbroken PS5, as a homebrew title. Target
experience: the TV Steam Link app. Turn it on, it finds the PC running Steam, pair once
with a PIN, Big Picture Mode streams to the TV, the DualSense controllers just work.

This document records the research (October 2026), the decision, the architecture, the
build plan and the milestones. Nothing is built yet.

## 1. Decision in one paragraph

There is no open-source Steam Link client to port: Valve's client is proprietary, and its
Linux binary cannot run on the PS5 (a title may make no system call and the console is
FreeBSD-derived, not Linux). The protocol, however, has been reverse engineered:
**[ihslib](https://github.com/mariotaku/ihslib)** (C, LGPL-3.0) implements discovery,
pairing and streaming of Steam Remote Play, is actively maintained (last commit
2026-10-05, Ghidra-driven work against the real Steam client), and already powers a
Switch homebrew client ([nsteamlink](https://github.com/scaronni/nsteamlink), hardware
NVDEC) and a Raspberry Pi 5 client ([plume](https://github.com/beudbeud/plume)). On the
PS5 side, **[ProsperoLight](https://github.com/blackbearreloaded/ProsperoLight)**
(GPL-3.0-or-later, a Moonlight client for PS5, last commit 2026-10-07) has already solved
everything hardware-specific: VideoDec2 hardware H.264/HEVC decode presented through AGC
without CPU copies, Opus audio through AudioOut, up to four DualSense controllers through
ScePad, an OpenGL launcher UI, and a Make-based build with FTP deploy built on
[ps5-native-app-boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate).

**So: do not port ihsplay (the SDL2/LVGL webOS front end, stale since mid-2024). Write a
new, thin PS5 application that uses ihslib for the protocol and ProsperoLight's
platform code for the console.** The protocol is 100 % done elsewhere, the PS5 media
path is about 70 % done elsewhere, and our code is the glue: a ScePad HID provider for
ihslib, a pthread backend, a VideoDec2 video sink, an Opus-to-AudioOut sink, and the
screens.

## 2. What exists (research findings)

| Project | What it is | Verdict for us |
| --- | --- | --- |
| [ValveSoftware/steamlink-sdk](https://github.com/ValveSoftware/steamlink-sdk) | SDK for apps *on* the Steam Link box; the streaming client is a closed binary | Not usable. Confirms no official source. |
| [mariotaku/ihslib](https://github.com/mariotaku/ihslib) | Steam Remote Play protocol library: UDP discovery (port 27036), PIN authorization, streaming request, session with control/stats/data channels, AES-CBC control crypto, H.264 and HEVC frame reassembly, Opus audio, mouse/keyboard/touch input, HID gamepad channel (incl. `IHS_CONTROLLER_TYPE_PS5`), microphone, frame stats. Deps: protobuf-c runtime, mbedtls (or OpenSSL), a threads backend (SDL2 upstream). LGPL-3.0, active. | **The protocol core.** |
| [mariotaku/ihsplay](https://github.com/mariotaku/ihsplay) | SDL2 + LVGL + `ss4s` media-sink client for webOS/RPi, GPL-3.0 | Reference for UI flow and stream lifecycle only; last commit June 2024, SDL-bound. Not ported. |
| [beudbeud/ihslib `plume`](https://github.com/beudbeud/ihslib/tree/plume) | Fork: +25 fixes (retransmit deadlocks, control-channel serialization, HID bugs), **pure POSIX thread backend** (`ihs_thread_posix.c`), no SDL | Source of the pthread backend; cherry-pick what upstream lacks. |
| [scaronni/nsteamlink](https://github.com/scaronni/nsteamlink) | Switch client on ihslib (kxn fork, +72 over upstream incl. a cumulative-ACK fix that cured 20 s input stalls), FFmpeg/NVDEC, SDL2. GPL-3.0. Excellent RE docs (`docs/STEAMLINK_PROTOCOL_RE.md`, `OFFICIAL_INPUT_RE.md`, `STEAM_REMOTE_PLAY_AUTH.md`) | **The closest precedent**: console + hardware decoder + ihslib. Its docs are our protocol reference; its author now upstreams to mariotaku. |
| [blackbearreloaded/ProsperoLight](https://github.com/blackbearreloaded/ProsperoLight) | Native Moonlight (Sunshine) client for PS5: VideoDec2 + AGC zero-copy, 1080p–2160p at 30–120 FPS, HEVC Main10 HDR, Opus → AudioOut stereo/5.1, 4 DualSense + USB keyboard/mouse, OpenGL launcher (ps5-homebrew-ui), self-update, `make deploy PS5_HOST=`. GPL-3.0-or-later, active | **The PS5 platform donor**: VideoDec2 structs and link stubs, AGC presenter, AudioOut, ScePad, sockets→sceNet adapter, mbedtls PS5 config and entropy, build/deploy tooling. |
| [blackbearreloaded/ps5-native-app-boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate) | Template ProsperoLight is built on: Make + `tools/build.sh`, downloads the public ps5-payload-dev SDK v0.42 and zlib, clean-room `libc.prx`, ELF→FSELF converter, `make init/deploy/undeploy`, param.json validation | **Our project skeleton.** |
| [blackbearreloaded/ps5-hardware-video-decoding-research](https://github.com/blackbearreloaded/ps5-hardware-video-decoding-research) | Measurements: HEVC 4K60 decode 5.5 ms, 16.7 ms access-unit-to-flip; H.264 High and HEVC Main at 1080p/1440p/2160p; AV1 not available | Sets expectations for latency and bitrate. |
| [blackbearreloaded/ps5-homebrew-ui](https://github.com/blackbearreloaded/ps5-homebrew-ui) + [ps5-opengl](https://github.com/blackbearreloaded/ps5-opengl) | OpenGL UI kit (~100 widgets, themes, finished screens, PC preview) on a Mesa port; prebuilt SDK downloaded by script | Launcher UI (milestone 5). |
| `../rpcs3-PS5` (local) | RPCS3 as a PS5 title: lavavex PS5_PayloadSDK fork + PS5_Vulkan RADV link recipe, `tools/deploy.sh` (curl FTP), `ps5/ps5_pad_handler.cpp`, `ps5_audio_backend.cpp` | Deploy-script shape and the libc-gap lessons. Its Vulkan/RADV toolchain is far heavier than we need: a streaming client needs no Vulkan. |
| Thalium, [Bug hunting in Steam Remote Play](https://blog.thalium.re/posts/achieving-remote-code-execution-in-steam-remote-play/) | Protocol write-up: 32 channels, AES-CBC with IV = HMAC-MD5, codec negotiation | Background reading. |

Protocol facts that shape the design (from ihslib's headers and nsteamlink's RE docs):

- Pairing is Steam-Link-style: **the client shows a PIN and the user types it into Steam
  on the PC** (Settings → Remote Play → Pair Steam Link). No keyboard needed on the console.
  The client identity (`deviceId` + `secretKey` → device token) must persist, or Steam
  asks to pair again.
- The host sends complete video frames; ihslib reassembles fragments and emits one
  Annex-B access unit per `submit` callback (keyframe flag included, SPS/PPS in
  `codecData`). That is exactly what VideoDec2 consumes, and what ProsperoLight already
  feeds it from Moonlight.
- Audio is Opus 48 kHz (channel count requested by the client).
- Gamepads go through the HID channel: the client enumerates "devices" with vid/pid and
  streams 48-byte input reports (6 axes, 16 buttons, gyro, accel, touch), optionally as
  deltas; the host sends rumble back as HID writes. ihslib's SDL provider is the template
  for a ScePad provider (~1,500 lines of SDL code, most of it enumeration plumbing).
- Timestamps on the wire are 16.16 fixed-point seconds; the session handles it.

## 3. Alternatives considered

- **Moonlight/Sunshine instead of Steam Remote Play.** ProsperoLight already exists and
  streams Steam Big Picture fine *if* the PC runs Sunshine. It is the fallback if the
  Steam protocol turns out to be a moving target, but it is not what was asked for:
  Remote Play needs nothing installed on the PC beyond Steam.
- **Port ihsplay wholesale.** It would drag in SDL2 (no PS5 port), LVGL and the `ss4s`
  sink layer, all for a UI we would replace anyway. Its `app/backend/stream/*` files are
  worth reading for lifecycle ordering, nothing more.
- **Port nsteamlink.** Same SDL2 problem, plus Switch-specific NVDEC and libnx code. Its
  protocol-level fixes matter; its application does not.
- **rpcs3-PS5's toolchain (lavavex SDK fork + PS5_Vulkan).** Needed there because RPCS3
  renders with Vulkan. A streaming client presents decoded surfaces through AGC directly,
  which the public ps5-payload-dev SDK plus ProsperoLight's link stubs already cover.
  Avoiding the RADV build keeps a first build at minutes rather than tens of minutes.

## 4. Architecture

```
          Steam on the PC (Remote Play enabled)
                  |  UDP 27036 discovery / auth         |  UDP session: control, stats, data channels
   ───────────────┼─────────────────────────────────────┼──────────────────────────────────
                  v                                     v
             ihslib client                         ihslib session
     (discover, PIN authorize,             (crypto, reassembly, retransmit, stats)
      streaming request)                 callbacks:  video     audio     input/HID
                  |                                   |         |            ^
                  v                                   v         v            |
        launcher (host list,                 VideoDec2 sink   Opus →     ScePad HID provider
        PIN screen, settings)                H.264/HEVC AU    AudioOut   (4 users, rumble,
                  |                          → GPU surface               touchpad = mouse)
                  v                                   |                      |
        ps5-homebrew-ui (OpenGL)              AGC presenter  → VideoOut      scePadReadState
        or CPU text UI until M5               (zero-copy, flip on vblank)    (4 ms poll thread)
```

Components and where each comes from:

| Component | Source | Our work |
| --- | --- | --- |
| Protocol (`ihslib`) | submodule, pinned to upstream `master` | A `src/platform/ihs_thread_posix.c` (pthread; 75 lines, or lifted from the `plume` fork), a small patch set under `patches/ihslib/` for anything PS5-specific, cherry-picked fixes from the nsteamlink fork that upstream lacks. Build it from a file list, not its CMake (pkg-config is a liability on the cross toolchain). |
| protobuf-c runtime | submodule, one `.c` | None. |
| mbedtls | submodule | ProsperoLight's `mbedtls_ps5_config.h` and `ps5_entropy.c` (`sysctl kern.rng_pseudo`; `/dev/urandom` is also available). ihslib needs AES-CBC/ECB, RSA-OAEP (authorization), CTR-DRBG. |
| Opus | submodule | None (decoder only). |
| Video sink | ProsperoLight `moonlight_stream.cpp` (VideoDec2 structs, create/decode/flush/reset, decoder memory, compute queue) and `native_agc_present.cpp` (1,840 lines) | Extract into `src/video/videodec2.cpp` + `src/video/agc_present.cpp` with a codec-agnostic `submit(access_unit, keyframe)` interface; keep decode and present on separate threads as ProsperoLight does. Report `IHS_SessionReportVideoFrameStage/Complete` so Steam's quality control sees decode time. |
| Audio sink | ProsperoLight's AudioOut thread; `../rpcs3-PS5` `ps5_audio_backend.cpp` (same port API) | `src/audio/opus_audioout.cpp`: Opus → S16 stereo → `sceAudioOutOutput`, bounded backlog, drop on overrun. 5.1 later via `audioChannelCount`. |
| Input | ihslib `src/hid/sdl/*` as the template; ProsperoLight `ps5_dualsense.cpp` for ScePad (users, slots, rumble, motion, touch) | `src/hid/ps5/*`: an `IHS_HIDProvider` whose devices are the signed-in users' pads, producing the 48-byte report from `ScePadData`; `write` → `scePadSetVibration`. Touchpad-as-mouse and PS+Options overlay chord via `IHS_SessionSendMouse*`. Text entry via `sceImeDialog` → `IHS_SessionSendText`. |
| Launcher UI | ps5-homebrew-ui + ps5-opengl (prebuilt SDK download) | Screens: hosts (auto-discovered, state), pairing (PIN display with countdown), settings, in-stream overlay (stats, disconnect, quit game), errors. Until M5, a CPU-rendered text screen from the boilerplate skeleton is enough to drive the stream. |
| Network | ProsperoLight `platform/ps5/ps5_sockets.c` (BSD API over `sceNet*`, epoll) | None expected. No DNS is needed (IPs only), which sidesteps the console's broken `getaddrinfo`. |
| Storage, logging | ProsperoLight `app_storage.cpp` shape; rpcs3's `/data/<app>` convention | `/data/steamlink-ps5/` (ShadowMount+ 1.7beta4+ mounts `/data` into the sandbox; the dev console has it) with `/app0/` fallback (writable nullfs of the title folder). Log file + `sceKernelDebugOutText` so klog shows it. Pairing identity, hosts and settings as a small text/JSON file. |
| Build, package, deploy | ps5-native-app-boilerplate `tools/`, `tooling/native/`, `runtime/` | Import as-is; add `tools/build-deps.sh`. |

Threads at run time: ihslib client thread, session receive thread (+ its timers), video
decode thread, present thread (vblank), audio output thread, pad poll thread (4 ms),
UI/main thread. The console gives a title 13 usable cores; the SDK reports 16, so size
nothing from `sysconf`. Give every thread an explicit stack (256 KB+); the default may
not match what the library expects (rpcs3-PS5 wraps `pthread_create` for this).

Lifecycle, following ProsperoLight: the launcher and the stream never own the display at
the same time. Launcher tears down (EGL display, its pad handle, its audio port) →
stream opens VideoOut, pads, AudioOut → stream ends → launcher is rebuilt. Returning
from HFR/HDR modes waits for the TV to settle.

## 5. Build system (deliberately plain)

One `make`, from a fresh clone, on Arch or Debian/Ubuntu/WSL:

```bash
git clone --recursive <repo>
cd steamlink-ps5
make doctor                   # checks clang/lld 18, python3, curl, wget, unzip
make                          # → dist/<TITLE_ID>/
PS5_HOST=192.168.0.192 make deploy
```

- `Makefile` and `tools/build.sh` come from the boilerplate: they fetch and verify the
  pinned public ps5-payload-dev SDK and zlib into `.deps/`, compile every file under
  `src/` with the SDK's clang, link with `prospero-lld` plus `--wrap`/`--defsym` bindings,
  convert the ELF to a development FSELF with the repository-owned converter, generate the
  clean-room `runtime/libc.prx`, and assemble `dist/<TITLE_ID>/{eboot.bin,sce_sys,sce_module}`.
- `tools/build-deps.sh` (modelled on ProsperoLight's `build-stream-deps.sh`) builds
  `build/deps/lib{ihslib,protobuf-c,mbedcrypto,mbedtls,mbedx509,opus}.a` once with the
  same cross compiler. ihslib and protobuf-c are compiled from explicit file lists;
  mbedtls and opus through their own CMake with the SDK's `toolchain/prospero.cmake`,
  with pkg-config disabled (`PKG_CONFIG_LIBDIR=/nonexistent`).
- `tools/deploy.sh` (already in this repo, adapted from rpcs3-PS5): curl FTP upload of
  `dist/<TITLE_ID>` to `/data/homebrew/`, `eboot.bin` and `param.json` last, size check
  afterwards. `tools/ps5sh.py` for `launch`/`kill`.
- Third-party code is git submodules pinned by commit, listed in `PINS.md` with the
  reason for each pin, as rpcs3-PS5 does. Patches live in `patches/<project>/` and are
  applied by `tools/build-deps.sh`, never committed into the submodule.
- No CMake at the top level, no host package discovery, nothing installed globally.
  Toolchain note: the boilerplate insists on LLVM 18 (`llvm-config-18`); on Arch that is
  the `llvm18`/`clang18`/`lld18` packages (to verify in M0), not the system clang.
- Optional later: `make ffpfsc` (compressed image) and a GitHub Actions build, both
  already written in the boilerplate.

Proposed layout:

```
Makefile  PINS.md  PLAN.md  README.md  LICENSE
sce_sys/            param.json, icon0.png (pic0/pic1/snd0 stay out of git)
src/
  main.cpp          storage, log, launcher ↔ stream handover
  app/              hosts registry, pairing identity, settings (plain files)
  ui/               screens (text UI first, ps5-homebrew-ui from M5)
  stream/           session owner: ihslib callbacks → sinks, stats, overlay chords
  video/            videodec2.cpp, agc_present.cpp (from ProsperoLight)
  audio/            opus_audioout.cpp
  hid/ps5/          IHS_HIDProvider for ScePad (template: ihslib/src/hid/sdl)
  platform/         ps5_sockets.c, ps5_entropy.c, ihs_thread_posix.c, runtime shims
patches/            ihslib/*.patch
third_party/        ihslib, protobuf-c, mbedtls, opus, ps5-homebrew-ui (submodules)
tooling/ runtime/   boilerplate's converter, CRT, libc.prx builder (imported)
tools/              build.sh, build-deps.sh, deploy.sh, ps5sh.py, doctor.sh
vendor/ps5/sdk/stubs/   link stubs for VideoDec2, VideoOut, AGC driver (from ProsperoLight)
docs/
```

## 6. Milestones

Each milestone ends with something that runs on the console; the dev console is the
PS5 at `192.168.0.192` (firmware 13.20, etaHEN, ftpsrv 2121, shsrv 2323, klogsrv 3232,
ShadowMount+ 1.7beta4). Pick a title ID now (`PPSA99350` unless homebrew.page wants
another; rpcs3 is `PPSA99300`, ProsperoLight `PPSA99002`).

**M0 — Skeleton that launches (½–1 day).** Import the boilerplate (`tools/`, `tooling/`,
`runtime/`, `Makefile`, `sce_sys/`), `make init TITLE_ID=… APP_NAME=…`, `make`, `make
deploy`, title on the home screen, a line in klog and a file under `/data/steamlink-ps5/`.
Decide the Arch LLVM 18 question. Done when the edit-deploy-launch-log loop takes under a
minute.

**M1 — Protocol on the console (2–4 days).** Cross-compile protobuf-c, mbedtls, opus,
ihslib with the pthread backend; port `ps5_sockets.c` and `ps5_entropy.c`. A headless
flow driven by the text UI: broadcast discovery finds the Steam PC, the screen shows a
PIN, the user pairs in Steam, the identity persists, a streaming request succeeds, the
session connects, and the log counts video and audio frames with their sizes and codec.
Validates the three protocol risks (broadcast in the sandbox, socket receive buffer size,
Steam version compatibility) before any media code exists.

**M2 — Video (3–5 days).** VideoDec2 sink for H.264 at 1080p60, AGC presentation,
keyframe wait on loss, `setCaptureSize` honoured, frame-stage reporting. Then HEVC
(`enableHevc`) and 1440p/2160p request sizes. Measure access-unit-to-flip with the
ProsperoLight method; expect ~17 ms.

**M3 — Audio (1–2 days).** Opus → AudioOut stereo, bounded backlog, clean start/stop with
the session. 5.1 is a setting later.

**M4 — Input (3–5 days).** ScePad HID provider for player 1 (buttons, sticks, triggers,
gyro/accel), then hot-plug and 4 users, rumble via HID writes, touchpad as mouse,
PS-button chord for the overlay, `sceImeDialog` for text. This is the milestone with the
most protocol uncertainty: nsteamlink's log shows several report-format revisions
(48-byte SDL layout vs host-requested lengths, "Generic Gamepad" announcement). Start from
upstream ihslib's SDL layout, verify against Steam's controller settings page, and adopt
nsteamlink's findings as needed. Done when a game plays end to end with two controllers.

**M5 — Launcher (3–5 days).** ps5-homebrew-ui screens: hosts list with live state,
pairing with countdown, settings (resolution cap, HEVC, audio channels, stream interface:
Big Picture / desktop / recent games), in-stream overlay (stats, disconnect, stop game),
error states and reconnect. Auto-connect to the last host on launch so the TV flow is one
button.

**M6 — Polish (ongoing).** Quality control feedback (`setTargetBitrate` etc. are host→
client in this protocol: honour them), stall recovery, HDR (Main10) if Steam offers it,
120 Hz, USB keyboard/mouse passthrough (ProsperoLight has it), microphone (ihslib has the
channel), self-update kit.

**M7 — Release.** README with a real build matrix, `PINS.md`, GitHub Actions build,
homebrew.page listing. Unlike rpcs3-PS5 (GPL-2-only RPCS3 + GPL-3 SDK), every component
here is GPL-3-compatible and the runtime shim is clean-room, so built titles can be
shared the way ProsperoLight's are.

Rough total to a playable client (M0–M4): two to three weeks of focused work; the
launcher adds one more.

## 7. Risks and unknowns

| Risk | Likelihood | Mitigation |
| --- | --- | --- |
| Steam changes the protocol; ihslib lags | Medium, ongoing | Pin ihslib; nsteamlink's author upstreams fixes; keep Moonlight (ProsperoLight) as the documented fallback for users. |
| UDP broadcast discovery blocked in the title sandbox | Low–medium | ProsperoLight does mDNS multicast from a title, so sockets are fine; verify `SO_BROADCAST` in M1; manual IP entry is a feature anyway. |
| sceNet caps `SO_RCVBUF` well below the 4 MB ihslib asks for (nsteamlink hit ~42 KB on libnx) | Medium | Measure in M1 (`ps5_sockets.c` already logs requested vs actual); if capped, drain the socket on a dedicated thread into a userspace ring. |
| Input report format mismatch (host ignores or misreads the pad) | Medium | See M4; two independent implementations to compare against (upstream SDL provider, nsteamlink's revisions). |
| VideoDec2 is not in the public SDK | None in practice | ProsperoLight's link stubs and struct definitions (GPL-3) are proven on 6.02 and 12.70; the dev console is 13.20, which runs ProsperoLight-era homebrew. |
| Decoder bitrate ceiling at 4K HEVC (~80 Mb/s measured) | Known | Steam's default quality tiers sit far below; expose a resolution cap. |
| Thread stacks and core count | Known | Explicit stack sizes; never size pools from `sysconf`. |
| `/data` absent on older ShadowMount+ | Known | `/app0` fallback; log the chosen path at startup. |
| Trademark | — | Name the app something that is not "Steam Link" (e.g. "Prospero Remote Play"); the repo name is fine. Unofficial, personal-use client. |

## 8. Day-to-day loop

```bash
make                                          # or: make deps once, then make
tools/ps5sh.py "$PS5_HOST" kill               # stop the running title
PS5_HOST=… make deploy                        # or tools/deploy.sh
tools/ps5sh.py "$PS5_HOST" "launch PPSA99350"
setsid nohup ~/.claude/skills/ps5-console/scripts/ps5klog.sh "$PS5_HOST" /tmp/klog.txt &
tail -F /tmp/klog.txt | grep -E "\[SLP5\]|signal:|proc name:|reason:"
curl -s "ftp://$PS5_HOST:2121/data/steamlink-ps5/log.txt"
```

Keep a Steam PC on the LAN with Remote Play enabled and a controller-friendly game; use
Steam's own `logs/streaming_log.txt` on the PC as the second half of every investigation,
as nsteamlink's docs do.

## 9. Open questions for the owner

1. Title ID and display name (`PPSA99350`, "Prospero Remote Play"?).
2. Text UI through M4 and the OpenGL kit from M5 (recommended), or the kit from M0?
3. Any interest in the Moonlight fallback being mentioned in the README, or Steam-only?
