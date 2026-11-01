#!/usr/bin/env bash
# sotc: rebuild bb-probe (MSYS2 CLANG64 under I:/msys64) and run Shadow of the Colossus once.
# usage: tools/sotc_run.sh [seconds] [log]   (env passes through, e.g. BB_LIST_IMPORTS=1)
set -uo pipefail
cd -- "$(dirname -- "$0")/.."
export MSYS2_ROOT=${MSYS2_ROOT:-/i/msys64}
export PATH="$MSYS2_ROOT/clang64/bin:$MSYS2_ROOT/usr/bin:$PATH"
game=${SOTC_GAME_DIR:-"$(dirname "$0")/../../CUSA08809"}
log=${2:-out/run.txt}
if ! ninja -C out/gpu bb-probe > out/probe-build.log 2>&1; then
    grep -E 'error|Error' out/probe-build.log | head -20; exit 1
fi
cp out/gpu/bb-probe.exe out/
# sotc: SOTC_PROFILE_OUT (src/probe.c) writes the sampled addresses as bb-probe.exe offsets, so the dump
# is only readable with the exe of this run. Keep a copy next to it: <dump>.exe (tools/profile_functions.py
# uses it by default). Copied before the run; the dump is written by the probe at the end of the run.
if [ -n "${SOTC_PROFILE_OUT:-}" ]; then
    cp out/bb-probe.exe "${SOTC_PROFILE_OUT}.exe"
    echo "profile exe archived: ${SOTC_PROFILE_OUT}.exe"
fi
timeout "${1:-60}" out/bb-probe.exe out/boot-linked.bin --content-profile out/content.bin --sotc-cpu out/sotc-cpu.bin --app0 "$game" --user out/user > "$log" 2>&1
echo "rc=$?"
grep -v '<Warning> vk_instance' "$log" | grep -v '^Runtime: .*=0' | tail -25
