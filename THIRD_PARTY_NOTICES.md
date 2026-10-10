# Third-party notices

## Credits and acknowledgements

| Project | Role |
| --- | --- |
| [ps5-payload-dev/sdk](https://github.com/ps5-payload-dev/sdk) | Public PS5 headers, libc++ headers, sysroot, and Clang target support |
| [ps5-payload-dev/pacbrew-repo](https://github.com/ps5-payload-dev/pacbrew-repo) | Optional prebuilt PS5 ports and static libraries |
| [SvenGDK/SharpProspero](https://github.com/SvenGDK/SharpProspero) | Source of the ELF converter and FSELF writer in `tooling/native/` (GPL-3.0) |
| [SvenGDK/UFS2Tool](https://github.com/SvenGDK/UFS2Tool) | Optional UFS2 `.ffpkg` generation |
| [PSBrew/MkPFS](https://github.com/PSBrew/MkPFS) | Optional compressed `.ffpfsc` generation |
| [sinajet/PSFFPKG](https://github.com/sinajet/PSFFPKG) | Public `.ffpkg` procedure used as a format reference |
| [LLVM/Clang](https://github.com/llvm/llvm-project) | Native compiler |
| [GoogleTest](https://github.com/google/googletest) | Pinned host-only C++ unit-test framework |
| [zlib](https://zlib.net/) | Pinned source-built compression library used by the host FSELF tool |
| [Microsoft DirectXTex](https://github.com/microsoft/DirectXTex) | `texconv` presentation-image preparation |
| [FFmpeg](https://ffmpeg.org/) | Developer-supplied selection-audio preparation |
| [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus) | Directory-style deployment and hardware validation |
| [ArkSama/PS5-Lapy-JB-Daemon](https://github.com/ArkSama/PS5-Lapy-JB-Daemon) | Original Lapy project and owned-root design |
| [mpereiraesaa/PS5-Lapy-JB-Daemon](https://github.com/mpereiraesaa/PS5-Lapy-JB-Daemon) | Exact-title one-shot helper and cooperative elevation protocol |
| [mariotaku/ihslib](https://github.com/mariotaku/ihslib) | Clean-room Steam In-Home Streaming protocol implementation (LGPL-3.0) |
| [scaronni/ihslib](https://github.com/scaronni/ihslib) | `auth` branch, current Steam pairing and authentication (LGPL-3.0) |
| [protobuf-c](https://github.com/protobuf-c/protobuf-c) | Protocol Buffers runtime for the ihslib wire format (BSD-2-Clause) |
| [Mbed-TLS](https://github.com/Mbed-TLS/mbedtls) | AES-CBC, HMAC-SHA and RSA for the session crypto (Apache-2.0) |
| [Opus](https://opus-codec.org/) | Audio decoder (BSD-3-Clause) |
| [TLSF](http://tlsf.baisoku.org) | Two-Level Segregated Fit allocator behind the title's heap (BSD-3-Clause) |

## Streaming dependencies

The Steam Remote Play client is built on
[ihslib](https://github.com/mariotaku/ihslib) by Mariotaku, a clean-room
implementation of Steam's In-Home Streaming protocol, taken from the `auth`
branch of [scaronni's fork](https://github.com/scaronni/ihslib) for current
pairing support. ihslib is LGPL-3.0; it is built from source as a static
archive, and every change this project makes to it is published as a readable
patch in `patches/ihslib/`, documented one by one in [PINS.md](PINS.md), so the
library can be relinked or replaced independently. No Valve code, protocol
documentation, or asset is used, and none is included.

| Component | Licence | How it is used |
| --- | --- | --- |
| [ihslib](https://github.com/mariotaku/ihslib) | LGPL-3.0 | Discovery, pairing, session and channel protocol; built from a patched source copy |
| [protobuf-c](https://github.com/protobuf-c/protobuf-c) | BSD-2-Clause | Protocol Buffers runtime ihslib's messages require |
| [Mbed-TLS](https://github.com/Mbed-TLS/mbedtls) | Apache-2.0 (dual Apache-2.0 OR GPL-2.0-or-later; taken under Apache-2.0) | Session crypto: AES-CBC, HMAC-SHA, RSA |
| [Opus](https://opus-codec.org/) | BSD-3-Clause | Decoding the audio stream |
| [TLSF 3.1](http://tlsf.baisoku.org) | BSD-3-Clause | Copyright (c) 2006-2016 Matthew Conte; vendored unmodified in `third_party/tlsf/` and used by `platform/ps5/ps5_malloc.c` |

Each is fetched as a pinned git submodule (TLSF excepted, which is vendored),
cross-compiled from source, and statically linked. Each retains its own licence,
reproduced in its own source tree. Video decoding uses the console's own
VideoDec2 system module through the public SDK's stubs; no decoder library is
bundled.

## Native build dependencies

The application build uses LLVM/Clang/lld, zlib 1.3.2, and the public
[PS5 payload SDK](https://github.com/ps5-payload-dev/sdk). The bootstrapper
downloads SDK v0.42 after verifying SHA-256
`8cfbc7cd5811e719eb4f0c47eea668d3dc7b40bc8ab11c4a5031d40c23ec02da`.
It downloads zlib 1.3.2 from the upstream source archive after verifying
SHA-256 `bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16`
and compiles its static archive locally. Both dependencies remain under ignored
`.deps/native/`, retain their upstream licenses, and are not distributed by
this repository. No Sony SDK file is included.

Target C++ compilation uses the LLVM libc++ headers distributed by the public
SDK. Those headers retain the Apache-2.0 WITH LLVM-exception license recorded
upstream. The application does not redistribute or dynamically load the
complete libc++ or libc++abi archives.

The PS5 ELF converter and FSELF writer in `tooling/native/` are derived from
[SharpProspero](https://github.com/SvenGDK/SharpProspero), Copyright (C) 2026
SvenGDK, GPL-3.0, and were translated to C++ and modified by BlackBearReloaded.

## Host test dependency

The host unit-test target downloads
[GoogleTest](https://github.com/google/googletest) 1.17.0 after verifying
SHA-256 `65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c`.
It remains under ignored `.deps/test/`, retains its BSD-3-Clause license, and
is not linked into any PS5 application, runtime, or package artifact.

## Optional PS5-Lapy-JB-Daemon integration

The sandbox-elevation build fetches
[mpereiraesaa's PS5-Lapy-JB-Daemon](https://github.com/mpereiraesaa/PS5-Lapy-JB-Daemon)
at commit `54a095c0f19161825e845daa760a03b446e654fa`, invokes its unmodified
`owned-helper` target for the selected application's exact title and packages
the generated helper ELF with Lapy's MIT license. The shared protocol header
published upstream is LGPL-2.1-or-later; this repository's application-side
wire implementation is GPL-3.0-or-later.

Lapy was created by
[ArkSama / Team PHU](https://github.com/ArkSama/PS5-Lapy-JB-Daemon). Credit
belongs to ArkSama, mpereiraesaa and the Lapy contributors. No Lapy kernel
source is copied or modified here.

The helper build also uses the pinned `ps5log/1` header from
[mpereiraesaa/ps5-agc-gears](https://github.com/mpereiraesaa/ps5-agc-gears/tree/1ae1f9182abd2770c131b97419034fb85173c2dc/native/ps5log),
GPL-3.0-or-later, and the official PS5 Payload SDK v0.40. Those build inputs
remain under ignored `.deps/lapy/`; the normal application toolchain remains
the separately pinned Payload SDK v0.42.

## Optional PacBrew dependencies

When selected through `PACBREW_*` build variables, the build downloads the prebuilt ports image
from [ps5-payload-dev/pacbrew-repo](https://github.com/ps5-payload-dev/pacbrew-repo)
release `v0.40.2`, verifies its published SHA-256, and extracts only the
`target/user/homebrew` prefix under ignored `.deps/pacbrew/`. It does not
replace the pinned SDK or install files globally. PacBrew recipes and every
linked third-party library retain their upstream licenses; applications must
review those terms before redistribution.

The update-check example (`make update-check-example`) selects `libcurl`, which
statically links these PacBrew libraries into the example title. Its build
output stays under ignored `dist/` and is not distributed by this repository; an
application that ships the update check ships them and must carry their notices:

| Component | Version in PacBrew v0.40.2 | License |
| --- | --- | --- |
| [libcurl](https://curl.se/) | 8.18.0 | curl license (MIT/X derivative) |
| [OpenSSL](https://www.openssl.org/) | 3.5.2 | Apache License 2.0 |
| [zlib](https://zlib.net/) | 1.3.2 | zlib license |
| [zstd](https://github.com/facebook/zstd) | 1.5.6 | BSD-3-Clause (dual-licensed with GPL-2.0) |
| [libpsl](https://github.com/rockdaboot/libpsl) | 0.21.5 | MIT; built-in Public Suffix List data MPL-2.0 |

`examples/update-check/console_curl.c`, which makes these libraries run in a
native title, is original BlackBearReloaded code (GPL-3.0-or-later), taken from
the ProsperoRadio and ProsperoLichess projects. Its `gmtime_r` follows Howard
Hinnant's public-domain `civil_from_days` algorithm.

## Vendored miniz

`third_party/miniz/` holds [miniz](https://github.com/richgel999/miniz) 3.0.2
(commit `293d4db1b7d0ffee9756d035b9ac6f7431ef8492`), MIT, unmodified, with its
`LICENSE`; `SOURCE.json` records each file's SHA-256. Only the self-update
helper (`examples/self-update-helper`) and its host test link it; an
application that ships that helper ships miniz and must carry its notice. The
helper's archive validation and file helpers are original BlackBearReloaded
code (GPL-3.0-or-later), taken from the ProsperoStore project.

## Optional UFS2Tool dependency

When `.ffpkg` output is requested, the platform bootstrapper fetches
[SvenGDK/UFS2Tool](https://github.com/SvenGDK/UFS2Tool) at commit
`b5307a60d5b4e3a68ba680e0e33cfadf05017c77` into the ignored
`.deps/UFS2Tool` cache and builds it with the host .NET SDK. UFS2Tool is
BSD-2-Clause software and is not distributed by this repository.

## Optional MkPFS dependency

When `.ffpfsc` output is requested, the platform bootstrapper fetches
[PSBrew/MkPFS](https://github.com/PSBrew/MkPFS) at commit
`6cb8313dfe0c988ac52617794553f343243d3a56` into the ignored `.deps/MkPFS`
cache and installs its Python dependencies into an ignored virtual environment
there. MkPFS and its dependencies retain their own licenses and are not
distributed by this repository.

## Independently authored runtime shim

`tooling/native/libc_builder.cpp` and the manifests under
`tooling/native/runtime/` are independently authored for this project and
licensed under GPL-3.0-or-later. The generated `runtime/libc.prx` contains
project-authored compatibility stubs, startup code, and semantic loader
metadata. It contains no Sony runtime implementation.

Original ps5-native-app-boilerplate code is Copyright (C) 2026
BlackBearReloaded and licensed under GPL-3.0-or-later. Source and script files
carry matching SPDX identifiers.

## Assets

This project ships no boilerplate presentation assets: the BlackBear icon,
selection artwork, and the `Night Drive` selection track (`sce_sys/snd0.at9`)
supplied with ps5-native-app-boilerplate are not included here.

`assets/testpattern.h264` and `assets/banner.txt` are original to this project,
Copyright (C) 2026 the Steam-Link-PS5 contributors, GPL-3.0-or-later.

`sce_sys/icon0.png` is the Steam Link logo, a trademark of Valve Corporation. It
is **not** covered by this repository's licence; it is used only to identify, to
the person holding the controller, which service the app connects to. Valve
retains all rights to it. Anyone redistributing a modified build should replace
it with their own artwork.

No proprietary runtime module, encryption key, or game file is included.
