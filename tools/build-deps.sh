#!/usr/bin/env bash
# steamlink-ps5 - Cross-build the streaming dependencies once, into build/deps.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Everything the app links besides its own sources: protobuf-c's runtime, mbedtls
# (the crypto ihslib's authorization and session need), opus (the audio decoder),
# ihslib itself (built from an explicit file list: its CMake wants pkg-config and
# SDL2, neither of which exists for the console) and the PS5 platform layer under
# platform/ps5 (sceNet socket adapter, entropy, pthread backend). Each archive is
# rebuilt only when its inputs change.
#
#   tools/build-deps.sh            build what is missing or stale
#   tools/build-deps.sh --clean    remove build/deps first
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
bash "$root/tools/setup-native-dependencies.sh" >/dev/null
sdk=${PS5_PAYLOAD_SDK:-$root/.deps/native/ps5-payload-sdk}
cc="$sdk/bin/prospero-clang"
ar="$sdk/bin/llvm-ar"
ranlib="$sdk/bin/llvm-ranlib"
out="$root/build/deps"
jobs=${BUILD_JOBS:-$(nproc 2>/dev/null || echo 2)}

ihslib="$root/third_party/ihslib"
protobuf_c="$root/third_party/protobuf-c"
mbedtls="$root/third_party/mbedtls"
opus="$root/third_party/opus"
platform="$root/platform/ps5"

[[ ${1:-} == --clean ]] && rm -rf -- "$out"
[[ -x $cc && -x $ar ]] || { echo "build-deps.sh: PS5 payload SDK not found at $sdk" >&2; exit 2; }
[[ -f $ihslib/include/ihslib.h && -f $protobuf_c/protobuf-c/protobuf-c.c && -f $mbedtls/Makefile && -f $opus/CMakeLists.txt ]] || {
    echo "build-deps.sh: submodules missing; run: git submodule update --init --recursive" >&2
    exit 2
}
mkdir -p "$out/obj"

# An archive is up to date when the hash of its inputs matches the one recorded
# beside it. Hash file contents, not timestamps: git checkouts reset the latter.
stamp_of() { sha256sum "$@" | sha256sum | cut -d' ' -f1; }
fresh() {
    local archive=$1 stamp=$2
    [[ -f $archive && -f $archive.stamp && $(<"$archive.stamp") == "$stamp" ]]
}
cflags=(-O2 -ffunction-sections -fdata-sections -fPIC)
mbedtls_config=(-DMBEDTLS_CONFIG_FILE='"mbedtls_ps5_config.h"' "-I$platform")

# ---- protobuf-c runtime ----------------------------------------------------
archive="$out/libprotobuf-c.a"
stamp=$(stamp_of "$protobuf_c/protobuf-c/protobuf-c.c" "$protobuf_c/protobuf-c/protobuf-c.h" "$0")
if ! fresh "$archive" "$stamp"; then
    echo "==> [deps] protobuf-c"
    "$cc" "${cflags[@]}" -std=c11 -w -I"$protobuf_c" -c "$protobuf_c/protobuf-c/protobuf-c.c" -o "$out/obj/protobuf-c.o"
    rm -f "$archive" && "$ar" rcs "$archive" "$out/obj/protobuf-c.o" && echo "$stamp" > "$archive.stamp"
fi

# ---- mbedtls ---------------------------------------------------------------
archive="$out/libmbedcrypto.a"
stamp=$(stamp_of "$platform/mbedtls_ps5_config.h" "$platform/psa_crypto_ps5_config.h" "$mbedtls/include/mbedtls/build_info.h" "$0" \
    $(find "$mbedtls/library" -type f -name '*.c' | sort))
if ! fresh "$archive" "$stamp"; then
    echo "==> [deps] mbedtls (library only)"
    work="$out/mbedtls-build"
    rm -rf "$work" && cp -a "$mbedtls" "$work"
    # The checkout already holds its generated sources; the generators would need perl
    # and python jsonschema on the host. A git checkout gives the templates arbitrary
    # mtimes, so mark the generated files newer than them and turn generation off.
    touch "$work/library/psa_crypto_driver_wrappers.h" "$work/library/psa_crypto_driver_wrappers_no_static.c" \
        "$work/library/error.c" "$work/library/version_features.c" "$work/library/ssl_debug_helpers_generated.c"
    make -C "$work" lib -j "$jobs" CC="$cc" AR="$ar" GEN_FILES= \
        CFLAGS="${cflags[*]} -DMBEDTLS_CONFIG_FILE=\\\"mbedtls_ps5_config.h\\\" -I$platform" \
        WARNING_CFLAGS='-w' >"$out/mbedtls.log" 2>&1 || { tail -30 "$out/mbedtls.log" >&2; exit 1; }
    cp "$work/library/libmbedcrypto.a" "$work/library/libmbedx509.a" "$work/library/libmbedtls.a" "$out/"
    rm -rf "$work"
    echo "$stamp" > "$archive.stamp"
fi

# ---- opus (decoder only is not an option upstream; the whole library is small) ----
archive="$out/libopus.a"
stamp=$(stamp_of "$opus/CMakeLists.txt" "$0" $(find "$opus/src" "$opus/celt" "$opus/silk" -maxdepth 1 -type f -name '*.c' | sort))
if ! fresh "$archive" "$stamp"; then
    echo "==> [deps] opus"
    work="$out/opus-build"
    cmake -S "$opus" -B "$work" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER="$cc" -DCMAKE_AR="$ar" -DCMAKE_RANLIB="$ranlib" \
        -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
        -DCMAKE_C_FLAGS_RELEASE="${cflags[*]} -DNDEBUG" \
        -DOPUS_BUILD_SHARED_LIBRARY=OFF -DOPUS_BUILD_TESTING=OFF -DOPUS_BUILD_PROGRAMS=OFF \
        -DOPUS_DISABLE_INTRINSICS=ON -DOPUS_X86_MAY_HAVE_AVX2=OFF -DOPUS_X86_PRESUME_AVX2=OFF \
        -DOPUS_X86_PRESUME_SSE4_1=OFF -DOPUS_STACK_PROTECTOR=OFF -DOPUS_FORTIFY_SOURCE=OFF \
        >"$out/opus.log" 2>&1 || { tail -30 "$out/opus.log" >&2; exit 1; }
    cmake --build "$work" --target opus --parallel "$jobs" >>"$out/opus.log" 2>&1 || { tail -30 "$out/opus.log" >&2; exit 1; }
    cp "$work/libopus.a" "$archive"
    rm -rf "$work"
    echo "$stamp" > "$archive.stamp"
fi

# ---- ihslib + PS5 platform layer ------------------------------------------
# Sources: everything under src/ except the SDL backends (threads, UDP, HID) and the
# OpenSSL crypto; the console gets the pthread backend from platform/ps5.
mapfile -t ihslib_sources < <(find "$ihslib/src" -type f -name '*.c' \
    ! -path '*/hid/sdl/*' ! -name 'ihs_thread_sdl.c' ! -name 'ihs_udp_sdl.c' ! -name 'impl_openssl.c' | sort)
platform_sources=("$platform/ihs_thread_posix.c" "$platform/ps5_sockets.c" "$platform/ps5_entropy.c" \
    "$platform/ps5_runtime_shims.c" "$platform/ps5_malloc.c" "$root/third_party/tlsf/tlsf.c")
archive="$out/libihslib.a"
stamp=$(stamp_of "${ihslib_sources[@]}" "${platform_sources[@]}" "$platform/ps5_compat.h" "$platform/mbedtls_ps5_config.h" "$0" \
    $(find "$ihslib/include" "$ihslib/src" -type f -name '*.h' | sort))
if ! fresh "$archive" "$stamp"; then
    echo "==> [deps] ihslib + platform (${#ihslib_sources[@]} + ${#platform_sources[@]} files)"
    objects=()
    # Socket descriptor calls inside ihslib route through the sceNet adapter (ps5_compat.h);
    # the adapter itself and the rest of the platform layer see the real libc names.
    common=("${cflags[@]}" -std=gnu11 -w "-I$ihslib/include" "-I$ihslib/src" "-I$protobuf_c" "-I$mbedtls/include" "-I$platform" "${mbedtls_config[@]}")
    i=0
    for source in "${ihslib_sources[@]}"; do
        object="$out/obj/ihs-$(printf '%03d' "$i")-$(basename "${source%.c}").o"
        "$cc" "${common[@]}" -include "$platform/ps5_compat.h" -c "$source" -o "$object" &
        objects+=("$object"); i=$((i + 1))
        (( i % jobs == 0 )) && wait
    done
    wait
    # set -e does not see a failed background compile: check the objects exist.
    for object in "${objects[@]}"; do
        [[ -f $object ]] || { echo "build-deps.sh: compile failed: $object" >&2; exit 1; }
    done
    for source in "${platform_sources[@]}"; do
        object="$out/obj/platform-$(basename "${source%.c}").o"
        extra=()
        [[ $(basename "$source") == ps5_sockets.c ]] && extra=(-DPS5_SOCKET_ADAPTER_IMPLEMENTATION)
        "$cc" "${common[@]}" "${extra[@]}" -c "$source" -o "$object"
        objects+=("$object")
    done
    rm -f "$archive" && "$ar" rcs "$archive" "${objects[@]}" && echo "$stamp" > "$archive.stamp"
fi

# ---- system import stubs ---------------------------------------------------
# VideoDec2 is a public system module the bundled SDK has no stub for. The stub
# only describes the imports to the linker; the console resolves the real module
# at run time. Its tiny bodies are never packaged or executed.
mkdir -p "$out/stubs"
stub_src="$root/vendor/ps5/sdk/stubs/videodec2_link_stub.c"
stub_so="$out/stubs/libSceVideodec2.so"
stamp=$(stamp_of "$stub_src" "$0")
if ! fresh "$stub_so" "$stamp"; then
    echo "==> [deps] libSceVideodec2 import stub"
    "$cc" -O2 -fPIC -ffunction-sections -fdata-sections -c "$stub_src" -o "$out/obj/videodec2_stub.o"
    "$sdk/bin/prospero-lld" --shared -soname libSceVideodec2.prx -o "$stub_so" "$out/obj/videodec2_stub.o"
    echo "$stamp" > "$stub_so.stamp"
fi

# ---- libc++ ---------------------------------------------------------------
# The app uses std::string, std::mutex and friends, whose non-inline parts live in
# the SDK's libc++. Copied under names the app build's path check accepts.
cp "$sdk/target/lib/libc++.a" "$out/libcxx.a"
cp "$sdk/target/lib/libc++abi.a" "$out/libcxxabi.a"
cp "$sdk/target/lib/libunwind.a" "$out/libunwind.a"

echo "==> [deps] ready: $(cd "$out" && ls *.a | tr '\n' ' ')"
