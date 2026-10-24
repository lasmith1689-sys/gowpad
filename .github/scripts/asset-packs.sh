#!/bin/bash
# Builds and uploads GoWPad's Apple-hosted asset packs: the God of War disc's two large files, PART1.PAK (pack gowdisc1)
# and PART2.PAK (pack gowdisc2), essential packs the system downloads together with the app (iOS 26 Background Assets).
# The files arrive as encrypted chunks (release disc-input, key in GAME_KEY), are decrypted only inside the runner, one
# pack at a time to fit the runner's disk, checked against pak.sha256, packaged with ba-package and uploaded with altool.
#
# Usage: asset-packs.sh WORK_DIR [pack numbers, default "1 2"]
# Environment: GH_TOKEN, GITHUB_REPOSITORY, GAME_KEY, ASC_KEY_ID, ASC_ISSUER_ID, ASC_KEY_P8, GOWPAD_APPLE_ID
set -euo pipefail

mkdir -p "$1"
work=$(cd "$1" && pwd)
packs=${2:-"1 2"}
apple_id=${GOWPAD_APPLE_ID:?}
openssl=/opt/homebrew/opt/openssl@3/bin/openssl
[ -x "$openssl" ] || openssl=openssl

# the App Store Connect API key where altool looks for it
keys="$HOME/.appstoreconnect/private_keys"
mkdir -p "$keys"
p8=$(printf '%s' "$ASC_KEY_P8" | tr -d '\r')
if [[ "$p8" != *"BEGIN PRIVATE KEY"* ]]; then
  p8=$(printf -- '-----BEGIN PRIVATE KEY-----\n%s\n-----END PRIVATE KEY-----' "$(printf '%s' "$p8" | tr -d ' \n' | fold -w 64)")
fi
(umask 077 && printf '%s\n' "$p8" > "$keys/AuthKey_$ASC_KEY_ID.p8")
unset p8
trap 'rm -rf "$keys/AuthKey_$ASC_KEY_ID.p8" "$work/disc"' EXIT

cd "$work"
echo "▶ ba-package template (for reference)"
xcrun ba-package template -o template.json > /dev/null 2>&1 && grep -v '^\s*//' template.json | head -40 || xcrun ba-package --help | head -30
gh release download disc-input -R "$GITHUB_REPOSITORY" -p pak.sha256 -D "$work" --clobber

for n in $packs; do
  pack=gowdisc$n
  file=PART$n.PAK
  echo "▶ $pack: $file"
  rm -rf disc && mkdir disc
  for chunk in $(gh release view disc-input -R "$GITHUB_REPOSITORY" --json assets --jq ".assets[].name | select(startswith(\"$file.part\"))" | sort); do
    gh release download disc-input -R "$GITHUB_REPOSITORY" -p "$chunk" -D "$work" --clobber
    "$openssl" enc -d -aes-256-cbc -pbkdf2 -iter 600000 -md sha256 -pass env:GAME_KEY -in "$chunk" >> "disc/$file"
    rm -f "$chunk"
    echo "  $chunk -> $(stat -f %z "disc/$file") bytes"
  done
  expected=$(grep "$file" pak.sha256 | awk '{print $1}')
  actual=$(shasum -a 256 "disc/$file" | awk '{print $1}')
  [ "$expected" = "$actual" ] || { echo "::error::$file checksum mismatch ($actual, expected $expected)"; exit 1; }
  cat > "$pack.json" <<EOF
{
  "assetPackID": "$pack",
  "downloadPolicy": { "essential": { "installationEventTypes": ["firstInstallation", "subsequentUpdate"] } },
  "fileSelectors": [ { "file": "$file" } ],
  "platforms": ["iOS"]
}
EOF
  start=$SECONDS
  (cd disc && xcrun ba-package "$work/$pack.json" -o "$work/$pack.aar")
  rm -f "disc/$file"
  echo "  packaged in $((SECONDS - start)) s: $(stat -f %z "$pack.aar") bytes"
  xcrun altool --upload-asset-pack "$pack.aar" --apple-id "$apple_id" \
    --apiKey "$ASC_KEY_ID" --apiIssuer "$ASC_ISSUER_ID" --output-format normal
  rm -f "$pack.aar"
  # processing ends in READY_FOR_TESTING (success) or FAILED (altool --wait only stops on READY/FAILED: never here)
  for attempt in $(seq 1 120); do
    versions=$(xcrun altool --list-asset-pack-versions --apple-id "$apple_id" --asset-pack-identifier "$pack" \
      --apiKey "$ASC_KEY_ID" --apiIssuer "$ASC_ISSUER_ID" --output-format normal 2>&1 || true)
    if grep -q "FAILED" <<<"$versions"; then echo "$versions"; echo "::error::$pack processing failed"; exit 1; fi
    if grep -q "READY_FOR_TESTING\|READY_FOR_INTERNAL_TESTING" <<<"$versions"; then echo "$versions" | tail -n 8; break; fi
    [ "$attempt" = 120 ] && { echo "$versions"; echo "::warning::$pack still processing after an hour"; }
    sleep 30
  done
done
xcrun altool --list-asset-packs --apple-id "$apple_id" --apiKey "$ASC_KEY_ID" --apiIssuer "$ASC_ISSUER_ID" || true
