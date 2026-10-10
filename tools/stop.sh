#!/usr/bin/env bash
# Steam-Link-PS5 - Stop the title on the console (shsrv), if it is running.
#   PS5_HOST=192.168.x.x tools/stop.sh
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
host=${PS5_HOST:?set PS5_HOST to the console IP address}
title_id=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' "$root/sce_sys/param.json")
pid=$(timeout 30 python3 "$root/tools/ps5sh.py" "$host" ps 2>/dev/null | awk -v t="$title_id" '$8 == t {print $1}' || true)
if [[ -z $pid ]]; then
    echo "==> [stop] $title_id is not running"
    exit 0
fi
echo "==> [stop] killing $title_id (pid $pid)"
timeout 30 python3 "$root/tools/ps5sh.py" "$host" "kill $pid" >/dev/null 2>&1 || true
sleep 3
if timeout 30 python3 "$root/tools/ps5sh.py" "$host" ps 2>/dev/null | awk -v t="$title_id" '$8 == t' | grep -q .; then
    echo "==> [stop] still running" >&2
    exit 1
fi
echo "==> [stop] stopped"
