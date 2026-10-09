# Pinned revisions

Third-party code is git submodules pinned by commit (`git submodule status`), plus the
SDK archive `tools/setup-native-dependencies.sh` fetches and verifies by SHA-256. To move
a pin: check out the new revision in the submodule, run `make`, test on the console, then
commit the submodule change here together with a note below.

| Component | Where | Revision | Why this one |
| --- | --- | --- | --- |
| [ihslib](https://github.com/scaronni/ihslib) branch `auth` | `third_party/ihslib` | `f68de5f0` (2026-10-04) | Upstream [mariotaku/ihslib](https://github.com/mariotaku/ihslib) master `1881b9a0` plus two commits implementing Steam's 2026 PIN-masked X25519 pairing (`crypto: add SHA-256, X25519 and Steam's key exchange`, `client: pair with the PIN-masked key exchange`). Without them current Steam answers `AuthorizationFailed` once the PIN is typed. Switch back to upstream once they are merged. `patches/ihslib/` carries two small robustness patches on top (see below). |
| [protobuf-c](https://github.com/protobuf-c/protobuf-c) | `third_party/protobuf-c` | v1.5.2 | ihslib's generated sources need ≥ 1.3.0; 1.5.2 is what they were regenerated with. |
| [mbedtls](https://github.com/Mbed-TLS/mbedtls) | `third_party/mbedtls` | v3.6.4 | The revision ProsperoLight proved on the console; 4.x drops APIs ihslib uses (`mbedtls_md_hmac`). Built with `platform/ps5/mbedtls_ps5_config.h`. |
| [opus](https://github.com/xiph/opus) | `third_party/opus` | v1.5.2 | Same as ProsperoLight. Intrinsics off (the SDK's toolchain has no runtime CPU dispatch we trust). |
| [TLSF](https://github.com/mattconte/tlsf) | `third_party/tlsf` | 3.1 (vendored, BSD) | The title's malloc: a pool over direct memory, grown on demand. |
| [ps5-payload-dev SDK](https://github.com/ps5-payload-dev/sdk) | `.deps/native/ps5-payload-sdk` (fetched) | v0.42, SHA-256 in `tools/setup-native-dependencies.sh` | What ps5-native-app-boilerplate pins. Builds with the host clang (tested with clang 22 on Arch, clang 18 upstream). |
| [ps5-native-app-boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate) | `tools/`, `tooling/`, `runtime/`, `Makefile` | `3ada439f` (2026-10-07), imported | Build, packaging, clean-room `libc.prx`, FTP deploy. Local changes: clang fallback, optional ccache, `APP_IMPORT_STUBS`, `-L` for libc++'s pthread note, `.eh_frame` bracket symbols in `ps5-pie.ld`. |
| [ProsperoLight](https://github.com/blackbearreloaded/ProsperoLight) | `platform/ps5/ps5_sockets.c`, `ps5_entropy.c`, `mbedtls_ps5_config.h`, `vendor/ps5/sdk/stubs/` | `c3b082bf` (2026-10-07), copied | The sceNet socket adapter, entropy source, mbedtls config and the VideoDec2 link stub; the VideoDec2 ABI structs in `src/video.cpp` are theirs too. |

## Patches on ihslib (`patches/ihslib/`, applied by `tools/build-deps.sh` to a copy)

- `0001-data-channel-drop-unexpected-packet-type.patch`: a data channel receiving a packet
  of an unexpected type logged and dropped instead of `assert`ing (an abort on Wi-Fi loss).
- `0002-data-channel-reset-window-on-overflow.patch`: a packet window full of a partial
  frame is reset instead of staying stuck for the rest of the session.
- `0003-session-config-bitrate-and-quality.patch`: `IHS_SessionConfig` gains
  `maxBitrateKbps` and `quality`, so the app can cap the bitrate and pick a preset instead
  of the hard-coded Balanced at 30000 kbit/s that floods a weak Wi-Fi link.
- `0004-session-last-submitted-video-frame-id.patch`: exposes the id of the video frame
  being submitted so the app can report decode stages and completion to the host.
- `0005-frame-stats-skip-unsynchronised-network-duration.patch`: drops the accumulated
  "network" stat that mixed the host clock with ours; its bogus multi-second value made
  the host believe the network was the bottleneck and drop nearly every frame.
- `0006-retransmission-resend-on-nack-and-back-off.patch`: a NACK now resends the named
  packet instead of being treated like an ACK (cancelling it), and reliable retries back
  off from 60 ms to 1 s instead of 21 sends in 200 ms. Also makes `IHS_QueuePollBy` detach
  the polled node so it can be re-queued without building a cycle.
- `0007-window-resync-when-host-races-beyond-the-window.patch`: when the host's packet id
  is more than a whole window ahead (a big loss burst), resync instead of silently ignoring
  every later packet, which used to freeze the channel for good.
- `0008-hid-report-clamp-instead-of-abort.patch`: the HID report holder clamps a report to
  the length the host asked for instead of `abort()`ing the whole title, which crashed the
  app a few seconds into every stream once the host requested a shorter controller report.
- `0009-session-config-max-resolution.patch`: `IHS_SessionConfig` gains `maxResolutionX/Y`
  so the app can ask the host to downscale (the negotiation hard-coded "unlimited"), which
  shrinks keyframes on a weak link.

All are candidates for upstream.
