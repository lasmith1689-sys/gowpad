#!/bin/bash
# Builds GoWPad: God of War (USA) as native iPhone/iPad code, on Waffle565's God of War runtime (a PS2Recomp fork,
# vendored in PS2Recomp/ with its OpenGL ES port) plus my decrypted game input. The input holds the game's own files and
# the code generated from them on my PC: the recompiled EE functions, the VU1 microprograms compiled to C++ and the IOP
# modules compiled to C++. Nothing is recompiled here; this only stages and compiles.
#
# Usage: build-ios.sh INPUT_DIR WORK_DIR [device|simulator]
#   INPUT_DIR  game/ (SCUS_973.99, IRX modules, IOP_MOD/, TOC, blank.ps2) and generated/ (ee/, vu1/, iop/)
#   WORK_DIR   scratch space; the app ends up at WORK_DIR/GoWPad.app
# Environment: GOWPAD_BUNDLE_ID
set -euo pipefail

input=$(cd "$1" && pwd)
mkdir -p "$2"
work=$(cd "$2" && pwd)
target=${3:-device}
root=$(cd "$(dirname "$0")/../.." && pwd)
export CMAKE_POLICY_VERSION_MINIMUM=3.5
jobs=${CMAKE_BUILD_PARALLEL_LEVEL:-$(sysctl -n hw.ncpu)}
case "$target" in
  device) sysroot=iphoneos; platform=iPhoneOS; triple=arm64-apple-ios26.0 ;;
  simulator) sysroot=iphonesimulator; platform=iPhoneSimulator; triple=arm64-apple-ios26.0-simulator ;;
  *) echo "::error::target must be device or simulator"; exit 1 ;;
esac
bundle=${GOWPAD_BUNDLE_ID:-com.lasmith1689.gowpad}
group="group.$bundle" # shared by the app and its Background Assets downloader extension

echo "▶ Staging the runtime with the generated code ($target)"
rm -rf "$work/src"
cp -R "$root/PS2Recomp" "$work/src"
runner="$work/src/ps2xRuntime/src/runner"
rm -f "$runner"/*.cpp
rsync -a --include='*.cpp' --exclude='*' "$input/generated/ee/" "$runner/"
cp "$input/generated/ee/"*.h "$work/src/ps2xRuntime/include/"
cp "$root/game/overrides/gow_overrides.cpp" "$root/game/overrides/"*.h "$runner/"
cp "$input/generated/iop/"*.cpp "$work/src/ps2xIOP/src/native/"
echo "  $(find "$runner" -name '*.cpp' | wc -l | tr -d ' ') EE sources, $(ls "$input/generated/iop" | wc -l | tr -d ' ') IOP files," \
  "$(find "$input/generated/vu1" -name '*.cpp' | wc -l | tr -d ' ') VU1 files"

echo "▶ iOS build ($jobs jobs)"
cmake -S "$work/src" -B "$work/ios" -G Ninja \
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=$sysroot -DCMAKE_SYSTEM_PROCESSOR=arm64 -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 -DCMAKE_BUILD_TYPE=Release \
  -DPS2X_BUILD_RECOMP=OFF -DPS2X_BUILD_ANALYZER=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF \
  -DPS2X_ENABLE_FFMPEG=OFF -DPS2X_ENABLE_SCCACHE=OFF -DPS2X_ENABLE_AGRESSIVE_LOGS=OFF -DPS2X_ENABLE_RUNTIME_LOGS=OFF \
  -DPS2X_ENABLE_IOP_RPC_TRACE=OFF -DPS2X_VU1_PREGENERATED_DIR="$input/generated/vu1" \
  -DPS2X_IOS_BUNDLE_ID="$bundle" -DPS2X_IOS_APP_NAME=GoWPad \
  -DPS2X_IOS_GAME_DIR="$input/game" -DPS2X_IOS_PLATFORM_NAME=$platform -DPS2X_IOS_APP_GROUP="$group" \
  > "$work/ios-configure.log" 2>&1 || { tail -80 "$work/ios-configure.log"; exit 1; }
grep -E "VU1 compilada|ARM|sse2neon" "$work/ios-configure.log" || true
start=$SECONDS
cmake --build "$work/ios" --target ps2EntryRunner -j "$jobs" -- -k 0 > "$work/ios-build.log" 2>&1 || {
  grep -E "error:|FAILED" "$work/ios-build.log" | sort -u | head -150; tail -30 "$work/ios-build.log"; exit 1; }
echo "  compiled in $(( (SECONDS - start) / 60 )) min"

app=$(find "$work/ios" -maxdepth 3 -name 'GoWPad.app' -type d | head -n 1)
[ -n "$app" ] || { echo "::error::GoWPad.app was not produced"; exit 1; }
rm -rf "$work/GoWPad.app"
ditto "$app" "$work/GoWPad.app"
app="$work/GoWPad.app"

echo "▶ Background Assets downloader extension (Apple-hosted asset packs, iOS 26)"
ext="$app/Extensions/GoWPadDownloader.appex"
mkdir -p "$ext"
# (linked like Xcode's extensionkit-extension product type, which inherits LD_ENTRY_POINT = _NSExtensionMain)
xcrun --sdk $sysroot swiftc -target $triple -parse-as-library -application-extension -O -module-name GoWPadDownloader \
  -Xlinker -e -Xlinker _NSExtensionMain -Xlinker -application_extension \
  "$root/ios/downloader/DownloaderExtension.swift" -o "$ext/GoWPadDownloader"
sed -e "s|@BUNDLE_ID@|$bundle.downloader|" -e "s|@PLATFORM@|$platform|" "$root/ios/downloader/Info.plist" > "$ext/Info.plist"
plutil -lint "$ext/Info.plist"
# entitlements for signing (testflight.sh / simulator-check.sh): the app group of both bundles
sed "s|@APP_GROUP@|$group|" "$root/ios/GoWPad.entitlements" > "$work/GoWPad.entitlements"
sed "s|@APP_GROUP@|$group|" "$root/ios/downloader/GoWPadDownloader.entitlements" > "$work/GoWPadDownloader.entitlements"

echo "▶ App icon"
icons="$work/Assets.xcassets/AppIcon.appiconset"
mkdir -p "$icons"
cp "$root/game/AppIcon-1024.png" "$icons/AppIcon-1024.png"
cat > "$icons/Contents.json" <<'EOF'
{ "images": [ { "filename": "AppIcon-1024.png", "idiom": "universal", "platform": "ios", "size": "1024x1024" } ],
  "info": { "author": "xcode", "version": 1 } }
EOF
printf '{ "info": { "author": "xcode", "version": 1 } }\n' > "$work/Assets.xcassets/Contents.json"
xcrun actool "$work/Assets.xcassets" --compile "$app" --platform $sysroot --minimum-deployment-target 16.0 \
  --app-icon AppIcon --target-device iphone --target-device ipad \
  --output-partial-info-plist "$work/icon-info.plist" > "$work/actool.log"
/usr/libexec/PlistBuddy -c "Merge $work/icon-info.plist" "$app/Info.plist"

ls -la "$app" | head -n 30
du -sh "$app"
