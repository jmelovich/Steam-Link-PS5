#!/usr/bin/env bash
# Steam-Link-PS5 - Stamp sce_sys/param.json's contentVersion from a release tag.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The PS5 wants contentVersion as NN.NNN.NNN (tools/build.sh and tools/lint.sh
# both enforce it), while releases are tagged as semver. This maps one to the
# other so they cannot drift:
#
#   v1.2.3        -> 01.002.003
#   v0.1.0        -> 00.001.000
#   v2.10.0-rc1   -> 02.010.000   (a pre-release suffix is ignored)
#
# contentVersion is how a console decides an installed copy is out of date, and
# the ps5-homebrew-catalog reads it from the committed sce_sys/param.json *at
# the release tag* - not from the built artifact. So the value has to be
# committed before tagging, which is what tools/release.sh does. CI then runs
# this with --check to prove the committed value matches the tag, and refuses
# the release if it does not.
#
#   tools/release.sh 1.2.3                     bump, commit, tag
#   tools/set-content-version.sh --check v1.2.3   verify, change nothing
#   tools/set-content-version.sh v1.2.3           write the value

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
param=${PARAM_JSON:-$root/sce_sys/param.json}

check_only=false
if [[ ${1:-} == --check ]]; then
    check_only=true
    shift
fi

if [[ $# -ne 1 ]]; then
    echo "usage: tools/set-content-version.sh [--check] <version>   # e.g. v1.2.3" >&2
    exit 2
fi

version=${1#v} # tolerate a leading v
version=${version%%[-+]*} # drop any -rc1 / +build suffix

if [[ ! $version =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)$ ]]; then
    echo "not a MAJOR.MINOR.PATCH version: $1" >&2
    exit 2
fi
major=${BASH_REMATCH[1]}
minor=${BASH_REMATCH[2]}
patch=${BASH_REMATCH[3]}

# NN.NNN.NNN has no room for more, and silently truncating would ship a version
# that moves backwards.
if ((major > 99)); then
    echo "major $major exceeds 99, which NN.NNN.NNN cannot represent" >&2
    exit 2
fi
if ((minor > 999 || patch > 999)); then
    echo "minor/patch $minor.$patch exceeds 999, which NN.NNN.NNN cannot represent" >&2
    exit 2
fi

content_version=$(printf '%02d.%03d.%03d' "$major" "$minor" "$patch")

[[ -f $param ]] || { echo "no such file: $param" >&2; exit 2; }

CONTENT_VERSION="$content_version" PARAM="$param" CHECK_ONLY="$check_only" \
    TAG="$1" python3 - <<'PY'
import os, re, sys

path = os.environ["PARAM"]
want = os.environ["CONTENT_VERSION"]
check_only = os.environ["CHECK_ONLY"] == "true"
tag = os.environ["TAG"]
text = open(path, encoding="utf-8").read()

pattern = re.compile(r'("contentVersion"\s*:\s*")(\d{2}\.\d{3}\.\d{3})(")')
match = pattern.search(text)
if not match:
    sys.exit(f"{path}: no NN.NNN.NNN contentVersion to read")
have = match.group(2)

if check_only:
    if have != want:
        sys.exit(
            f"contentVersion mismatch: {path} has {have}, but tag {tag} means {want}.\n"
            "The catalog reads contentVersion from the committed param.json at the\n"
            "release tag, so it must be bumped and committed before tagging. Use:\n"
            f"    tools/release.sh {tag.lstrip('v')}"
        )
    print(f"contentVersion {have} matches tag {tag}")
    sys.exit(0)

# Rewrite just the one value, so the file's formatting is untouched.
new, count = pattern.subn(rf"\g<1>{want}\g<3>", text)
if count != 1:
    sys.exit(f"{path}: expected one contentVersion, found {count}")
open(path, "w", encoding="utf-8").write(new)
print(f"contentVersion {have} -> {want}")
PY
