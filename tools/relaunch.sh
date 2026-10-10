#!/usr/bin/env bash
# Steam-Link-PS5 - The edit-deploy-test loop in one command.
# SPDX-License-Identifier: GPL-3.0-or-later
#
#   PS5_HOST=192.168.x.x tools/relaunch.sh [--no-build]
#
# Stops the title if it is running (a running title's files cannot be replaced),
# uploads dist/<TITLE_ID> over FTP, launches the title through shsrv and prints the
# first lines of its log. Needs ftpsrv (2121) and shsrv (2323) on the console.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
host=${PS5_HOST:?set PS5_HOST to the console IP address}
title_id=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' "$root/sce_sys/param.json")
sh() { timeout 30 python3 "$root/tools/ps5sh.py" "$host" "$@"; }

[[ ${1:-} == --no-build ]] || make -C "$root" --no-print-directory app
pid=$(sh ps 2>/dev/null | awk -v t="$title_id" '$8 == t {print $1}' || true)
if [[ -n $pid ]]; then
    echo "==> [relaunch] stopping $title_id (pid $pid)"
    sh "kill $pid" >/dev/null 2>&1 || true
    sleep 4
fi
PS5_HOST=$host bash "$root/tools/deploy.sh" | tail -1
echo "==> [relaunch] launching $title_id"
sh "launch $title_id" >/dev/null 2>&1 || true
sleep "${RELAUNCH_WAIT:-12}"
echo "==> [relaunch] log:"
curl -s --max-time 10 "ftp://$host:2121/data/steamlink-ps5/log.txt" | tail -n "${RELAUNCH_LINES:-25}"
