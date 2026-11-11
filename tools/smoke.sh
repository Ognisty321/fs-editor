#!/usr/bin/env bash
# Starts the built editor through its own smoke hooks (FSED_SMOKE*, see the README) and fails when
# a window or a dialog does not come up: the checks a build gets that no unit test gives.
#
#   tools/smoke.sh <path to fsed> [output folder]
#
# The runs use the smoke test's own settings, never the user's, and ask nothing of the network.
# On Linux without a display: xvfb-run -a tools/smoke.sh ...
set -u

exe=${1:?usage: tools/smoke.sh <path to fsed> [output folder]}
root=$(cd "$(dirname "$0")/.." && pwd)
out=${2:-$(mktemp -d)}
mkdir -p "$out"
# the program takes its paths with forward slashes, also on Windows
if command -v cygpath >/dev/null 2>&1; then
    out=$(cygpath -m "$out")
    root=$(cygpath -m "$root")
fi
subs="$root/tests/corpus/aegisub/ass-format-tests.ass"
failed=0

fail() {
    echo "FAIL: $*"
    failed=1
}

# run <name> [VAR=value ...] [-- file ...]: one start of the program, its screenshot in
# $out/<name>.png; a crash, a hang or a missing screenshot fails the run
run() {
    local name=$1
    shift
    local vars=() files=()
    while [ $# -gt 0 ] && [ "$1" != "--" ]; do
        vars+=("$1")
        shift
    done
    [ $# -gt 0 ] && shift
    files=("$@")
    rm -f "$out/$name.png"
    env FSED_SMOKE="$out/$name.png" FSED_SMOKE_MS=2500 "${vars[@]}" \
        timeout 120 "$exe" "${files[@]}" >"$out/$name.log" 2>&1
    local rc=$?
    if [ $rc -ne 0 ]; then
        fail "$name: exit code $rc"
        tail -n 20 "$out/$name.log"
        return 1
    fi
    if [ ! -s "$out/$name.png" ]; then
        fail "$name: no screenshot"
        return 1
    fi
    echo "ok: $name"
}

# the window with a file: its menus as a tree, the timings of the grid, the stalls of its thread
if run window FSED_SMOKE_MENUS="$out/menus.txt" FSED_SMOKE_BENCH="$out/bench.txt" \
    FSED_SMOKE_STALLS="$out/stalls.txt" FSED_SMOKE_MS=6000 -- "$subs"; then
    for id in openSubs save undo findReplace openVideo openAudio styleManager checkUpdates; do
        grep -q "<$id>" "$out/menus.txt" 2>/dev/null || fail "window: the menus lack <$id>"
    done
    grep -q "^rows: [1-9]" "$out/bench.txt" 2>/dev/null || fail "window: no lines in the grid"
    cat "$out/bench.txt" 2>/dev/null
fi

# a video and an audio that are no one's files, with a line picked: the video and audio panels draw
run media FSED_SMOKE_ACTION="openDummy;blankAudio" FSED_SMOKE_CLICK="OK" FSED_SMOKE_ROW=1 FSED_SMOKE_MS=5000 -- "$subs"

# each window a menu command opens
for id in styleManager properties fontCollector findReplace selectLines settings hotkeys resample attachments \
    spellChecker shiftTimes history automationManager kanjiTimer stylingAssistant translationAssistant; do
    run "dialog-$id" FSED_SMOKE_ACTION="$id" -- "$subs"
done

# the update check against an answer in a file: a newer version is offered, in a window of its own
printf '{"tag_name": "v999.0.0", "html_url": "https://example.invalid/", "body": "notes"}' >"$out/latest.json"
run update FSED_UPDATE_URL="file:///${out#/}/latest.json" FSED_SMOKE_ACTION="checkUpdates" -- "$subs"

if [ $failed -ne 0 ]; then
    echo "smoke test failed; screenshots and logs in $out"
    exit 1
fi
echo "smoke test passed; screenshots in $out"
