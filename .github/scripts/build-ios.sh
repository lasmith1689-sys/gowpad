#!/bin/bash
# Recompiles God of War's EE code from my decrypted game input and builds the
# unsigned iPhone/iPad app around PS2Recomp's runtime (iOS port: raylib on
# SDL2 + OpenGL ES 3, see PS2Recomp/ps2xRuntime/CMakeLists.txt).
#
# Usage: build-ios.sh GAME_DIR WORK_DIR
#   GAME_DIR  SCUS_973.99, the loose disc files and gow_functions.csv (Ghidra map)
#   WORK_DIR  scratch space; the app ends up at WORK_DIR/GoWPad.app
# Environment: GOWPAD_BUNDLE_ID
set -euo pipefail

game=$(cd "$1" && pwd)
mkdir -p "$2"
work=$(cd "$2" && pwd)
root=$(cd "$(dirname "$0")/../.." && pwd)
export CMAKE_POLICY_VERSION_MINIMUM=3.5
jobs=${CMAKE_BUILD_PARALLEL_LEVEL:-$(sysctl -n hw.ncpu)}

echo "▶ Recompiler (host build)"
cmake -S "$root/PS2Recomp" -B "$work/recomp" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DPS2X_BUILD_RUNTIME=OFF -DPS2X_BUILD_ANALYZER=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF > "$work/recomp-configure.log"
cmake --build "$work/recomp" --target ps2_recomp -j "$jobs" > "$work/recomp-build.log" || { tail -40 "$work/recomp-build.log"; exit 1; }

echo "▶ Recompiling the game's EE code"
sed -e "s|@GAME_DIR@|$game|g" -e "s|@OUT_DIR@|$work/generated|g" "$root/game/gow.toml.in" > "$work/gow.toml"
"$work/recomp/ps2xRecomp/ps2_recomp" "$work/gow.toml" > "$work/recomp.log" 2>&1 || { tail -40 "$work/recomp.log"; exit 1; }
tail -n 2 "$work/recomp.log"
echo "  $(find "$work/generated" -name '*.cpp' | wc -l | tr -d ' ') generated sources"

echo "▶ Staging the generated code into a copy of the runtime"
rm -rf "$work/src"
cp -R "$root/PS2Recomp" "$work/src"
rm -f "$work/src/ps2xRuntime/src/runner/register_functions.cpp"
rsync -a --include='*.cpp' --exclude='*' "$work/generated/" "$work/src/ps2xRuntime/src/runner/"
cp "$work/generated/"*.h "$work/src/ps2xRuntime/include/"

# read-only game files that ship inside the app (the disc image itself goes in Documents);
# the game loads its IOP modules as IOP_MOD/<name>.irx
bundle="$work/bundle-game"
rm -rf "$bundle"
mkdir -p "$bundle/IOP_MOD"
cp "$game"/SCUS_973.99 "$game"/SYSTEM.CNF "$game"/GODOFWAR.TOC "$game"/IOPRP300.IMG "$game"/*.IRX "$bundle/"
for f in "$game"/*.IRX; do
  cp "$f" "$bundle/IOP_MOD/$(basename "$f" | tr '[:upper:]' '[:lower:]')"
done
# 989snd stays out: without the IRX the runtime's silent 989snd stand-in answers
# (the real driver fails its start-up on the emulated IOP and never replies)
rm -f "$bundle/IOP_MOD/989nomid.irx"

echo "▶ iOS build ($jobs jobs)"
cmake -S "$work/src" -B "$work/ios" -G Ninja \
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_SYSTEM_PROCESSOR=arm64 -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 -DCMAKE_BUILD_TYPE=Release \
  -DPS2X_BUILD_RECOMP=OFF -DPS2X_BUILD_ANALYZER=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF \
  -DPS2X_ENABLE_FFMPEG=OFF -DPS2X_ENABLE_SCCACHE=OFF -DPS2X_ENABLE_AGRESSIVE_LOGS=OFF \
  -DPS2X_DEFAULT_BOOT_ELF=SCUS_973.99 \
  -DPS2X_IOS_BUNDLE_ID="${GOWPAD_BUNDLE_ID:-com.lasmith1689.gowpad}" -DPS2X_IOS_APP_NAME=GoWPad \
  "-DPS2X_IOS_GAME_DIR=$bundle" > "$work/ios-configure.log" 2>&1 || { tail -60 "$work/ios-configure.log"; exit 1; }
start=$SECONDS
cmake --build "$work/ios" --target ps2EntryRunner -j "$jobs" > "$work/ios-build.log" 2>&1 || {
  grep -E "error:|FAILED" "$work/ios-build.log" | head -40; tail -20 "$work/ios-build.log"; exit 1; }
echo "  compiled in $(( (SECONDS - start) / 60 )) min"

app=$(find "$work/ios" -maxdepth 3 -name 'GoWPad.app' -type d | head -n 1)
[ -n "$app" ] || { echo "::error::GoWPad.app was not produced"; exit 1; }
rm -rf "$work/GoWPad.app"
ditto "$app" "$work/GoWPad.app"
app="$work/GoWPad.app"

echo "▶ App icon"
icons="$work/Assets.xcassets/AppIcon.appiconset"
mkdir -p "$icons"
cp "$root/game/AppIcon-1024.png" "$icons/AppIcon-1024.png"
cat > "$icons/Contents.json" <<'EOF'
{ "images": [ { "filename": "AppIcon-1024.png", "idiom": "universal", "platform": "ios", "size": "1024x1024" } ],
  "info": { "author": "xcode", "version": 1 } }
EOF
printf '{ "info": { "author": "xcode", "version": 1 } }\n' > "$work/Assets.xcassets/Contents.json"
xcrun actool "$work/Assets.xcassets" --compile "$app" --platform iphoneos --minimum-deployment-target 16.0 \
  --app-icon AppIcon --target-device iphone --target-device ipad \
  --output-partial-info-plist "$work/icon-info.plist" > "$work/actool.log"
/usr/libexec/PlistBuddy -c "Merge $work/icon-info.plist" "$app/Info.plist"

ls -la "$app" | head -n 30
du -sh "$app"
