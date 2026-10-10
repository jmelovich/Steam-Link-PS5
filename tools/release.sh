#!/usr/bin/env bash
# Steam-Link-PS5 - Cut a release: bump contentVersion, commit, tag.
# SPDX-License-Identifier: GPL-3.0-or-later
#
#   tools/release.sh 1.1.0
#
# The order matters. ps5-homebrew-catalog reads contentVersion from the
# committed sce_sys/param.json *at the release tag*, and a console compares that
# value with the one in the copy it has installed. So the bump has to be in the
# commit the tag points at, or the listing shows a new release that no console
# ever offers as an update. This does the three steps in the right order and
# leaves the push to you.

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"

if [[ $# -ne 1 ]]; then
    echo "usage: tools/release.sh <version>      # e.g. tools/release.sh 1.1.0" >&2
    exit 2
fi

version=${1#v}
tag="v$version"

# Refuse to build a release out of a dirty or detached tree: the tag has to
# point at something reproducible.
branch=$(git rev-parse --abbrev-ref HEAD)
[[ $branch != HEAD ]] || { echo "detached HEAD; check out a branch first" >&2; exit 2; }
if [[ -n $(git status --porcelain) ]]; then
    echo "working tree is not clean; commit or set aside your changes first" >&2
    git status --short >&2
    exit 2
fi
if git rev-parse -q --verify "refs/tags/$tag" >/dev/null; then
    echo "tag $tag already exists" >&2
    exit 2
fi

# Validates the version and rejects anything NN.NNN.NNN cannot hold.
tools/set-content-version.sh "$version"
content_version=$(python3 -c \
    'import json;print(json.load(open("sce_sys/param.json"))["contentVersion"])')

if git diff --quiet -- sce_sys/param.json; then
    echo "contentVersion is already $content_version; nothing to commit."
    echo "Raise the version, or delete the tag and re-cut if you are redoing a release." >&2
    exit 2
fi

git add sce_sys/param.json
git commit -m "release: $tag (contentVersion $content_version)"
git tag -a "$tag" -m "$tag"

cat <<EOF

Tagged $tag at $(git rev-parse --short HEAD), contentVersion $content_version.

Push both - the tag alone would point at a commit GitHub does not have:

    git push origin $branch
    git push origin $tag

CI then builds and opens a DRAFT release. Publish it from the Releases page:
the catalog cannot download a draft.
EOF
