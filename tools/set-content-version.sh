#!/usr/bin/env bash
# Steam-Link-PS5 - Stamp sce_sys/param.json's contentVersion from a release tag.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The PS5 wants contentVersion as NN.NNN.NNN (tools/build.sh and tools/lint.sh
# both enforce it), while releases are tagged as semver. Stores and updaters
# compare contentVersion, not the git tag, so a release whose contentVersion
# never moves looks like "no new version" no matter what it is tagged. CI calls
# this on a tag build so the two can never drift:
#
#   v1.2.3        -> 01.002.003
#   v0.1.0        -> 00.001.000
#   v2.10.0-rc1   -> 02.010.000   (a pre-release suffix is ignored)
#
# The repository keeps a fixed baseline in param.json; only the built artifact
# is stamped. Nothing is committed, so a local `make` is unaffected.

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
param=${PARAM_JSON:-$root/sce_sys/param.json}

if [[ $# -ne 1 ]]; then
    echo "usage: tools/set-content-version.sh <version>   # e.g. v1.2.3 or 1.2.3" >&2
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

CONTENT_VERSION="$content_version" PARAM="$param" python3 - <<'PY'
import os, re, sys

path = os.environ["PARAM"]
want = os.environ["CONTENT_VERSION"]
text = open(path, encoding="utf-8").read()

# Rewrite just the one value, so the file's formatting is untouched.
pattern = re.compile(r'("contentVersion"\s*:\s*")\d{2}\.\d{3}\.\d{3}(")')
if not pattern.search(text):
    sys.exit(f"{path}: no NN.NNN.NNN contentVersion to replace")
new, count = pattern.subn(rf"\g<1>{want}\g<2>", text)
if count != 1:
    sys.exit(f"{path}: expected one contentVersion, found {count}")
open(path, "w", encoding="utf-8").write(new)
print(f"contentVersion -> {want}")
PY
