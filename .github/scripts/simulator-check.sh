#!/bin/bash
# Runs a simulator build of GoWPad.app in a fresh iPhone simulator for a few minutes. There is no disc image (it never
# leaves my devices), so this checks what can run without it: start-up, the renderer's OpenGL ES context and
# framebuffer fetch, its shaders, the IOP modules and the game's boot up to its first disc read. Prints the app's
# log.txt and keeps screenshots in OUT_DIR.
#
# Usage: simulator-check.sh GoWPad.app OUT_DIR
# Environment: GOWPAD_BUNDLE_ID
set -euo pipefail

app=$1
mkdir -p "$2"
out=$(cd "$2" && pwd)
bundle=${GOWPAD_BUNDLE_ID:-com.lasmith1689.gowpad}

runtime=$(xcrun simctl list runtimes -j | python3 -c '
import json, sys
r = [x for x in json.load(sys.stdin)["runtimes"] if x.get("isAvailable") and x.get("platform") == "iOS"]
print(r[-1]["identifier"] if r else "")')
devtype=$(xcrun simctl list devicetypes -j | python3 -c '
import json, sys
d = [x["identifier"] for x in json.load(sys.stdin)["devicetypes"] if x["name"].startswith("iPhone") and "Pro" in x["name"]]
print(d[-1] if d else "")')
[ -n "$runtime" ] && [ -n "$devtype" ] || { echo "::error::no iOS simulator runtime/device type"; exit 1; }
echo "▶ Simulator: $devtype on $runtime"
udid=$(xcrun simctl create gowpad-check "$devtype" "$runtime")
trap 'xcrun simctl shutdown "$udid" >/dev/null 2>&1 || true; xcrun simctl delete "$udid" >/dev/null 2>&1 || true' EXIT
xcrun simctl boot "$udid"
xcrun simctl bootstatus "$udid" -b > /dev/null
xcrun simctl install "$udid" "$app"
data=$(xcrun simctl get_app_container "$udid" "$bundle" data)
echo "▶ Launching $bundle"
xcrun simctl launch "$udid" "$bundle"
elapsed=0
for wait in 15 30 60 75; do
  sleep "$wait"
  elapsed=$((elapsed + wait))
  xcrun simctl io "$udid" screenshot "$out/shot-${elapsed}s.png" > /dev/null 2>&1 || true
  if ! xcrun simctl spawn "$udid" launchctl list 2>/dev/null | grep -q "$bundle"; then
    echo "::warning::the app is no longer running after ${elapsed}s"
    break
  fi
done
# where every thread is (a stalled boot shows the game thread's recompiled function); kept with the screenshots
pid=$(pgrep -f "GoWPad.app/GoWPad" | head -1 || true)
if [ -n "$pid" ]; then
  sample "$pid" 5 -file "$out/sample.txt" > /dev/null 2>&1 || true
  echo "▶ sample of the running app (pid $pid), first 400 lines"
  head -n 400 "$out/sample.txt" 2>/dev/null || true
fi
log="$data/Documents/log.txt"
cp "$log" "$out/log.txt" 2>/dev/null || true
echo "▶ log.txt ($(wc -l < "$log" 2>/dev/null || echo 0) lines): start-up, GL and GS lines"
grep -E "^\[(ios|gs-gl|gs-hw|gs-present|gs-worker|gs-stage|gow-cd)\]|shader error|link error|FAILED|ERROR|terminate|fatal|unavailable" "$log" | head -120 || true
echo "▶ log.txt, heartbeat lines"
grep "^\[beat\]" "$log" | head -150 || true
echo "▶ log.txt, first 150 lines"
head -n 150 "$log" || true
echo "▶ log.txt, last 40 lines"
tail -n 40 "$log" || true
# crash reports of the app, if any
find "$HOME/Library/Logs/DiagnosticReports" -name 'GoWPad*' -newer "$app" 2>/dev/null | while read -r report; do
  echo "▶ crash report $report"
  head -n 80 "$report"
done
