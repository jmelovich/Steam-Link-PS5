#!/usr/bin/env bash
# Upload the title folder (dist/<TITLE_ID>) to the console's /data/homebrew over FTP.
# ShadowMount+ registers a new folder within ~15 s. eboot.bin and param.json go last so a
# half-uploaded title is never launchable.
#   PS5_HOST=<console ip> (required), FTP_PORT (default 2121)
# Stop the title first (a running title's files cannot be replaced):
#   tools/ps5sh.py "$PS5_HOST" kill   # or close it on the console
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
host=${PS5_HOST:?set PS5_HOST to the console IP address}
port=${FTP_PORT:-2121}
title_id=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' "$root/sce_sys/param.json")
app="$root/dist/$title_id"
[[ -f $app/eboot.bin ]] || { echo "deploy.sh: no $app/eboot.bin; run make first" >&2; exit 2; }

upload() {
    local rel=$1
    if curl -s --ftp-create-dirs -T "$app/$rel" "ftp://$host:$port/data/homebrew/$title_id/$rel"; then
        count=$((count + 1))
    else
        echo "deploy.sh: failed: $rel" >&2
        failed=$((failed + 1))
    fi
}

count=0 failed=0
while IFS= read -r -d '' file; do
    rel=${file#"$app/"}
    [[ $rel == eboot.bin || $rel == sce_sys/param.json ]] && continue
    upload "$rel"
done < <(find "$app" -type f -print0 | sort -z)
upload eboot.bin
upload sce_sys/param.json

# A truncated executable will not launch: compare sizes before trusting the upload.
remote=$(curl -s "ftp://$host:$port/data/homebrew/$title_id/" | awk '$NF == "eboot.bin" {print $5}')
local_size=$(stat -c %s "$app/eboot.bin")
[[ $remote == "$local_size" ]] || { echo "deploy.sh: eboot.bin size mismatch (console $remote, local $local_size)" >&2; failed=$((failed + 1)); }
echo "==> deployed $title_id to $host: $count files, $failed failed"
[[ $failed == 0 ]]
