# GoWPad

My personal, native iPhone/iPad build of **God of War (USA, SCUS-97399)**: the game's PS2 EE code is
statically recompiled to C++ with [PS2Recomp](https://github.com/ran-j/PS2Recomp) (vendored in `PS2Recomp/`,
GPL-3.0, with an iOS port of its runtime) and runs natively on the phone. The PS2 graphics synthesizer is
drawn in software; VU1 microcode runs in the runtime's interpreter.

## How a build happens

`.github/workflows/testflight.yml` (run by hand): decrypts my game input (release asset `game-input`, key in
the `GAME_KEY` secret) inside the Mac runner, recompiles the executable (`game/gow.toml.in`, Ghidra function
map + Sony SDK functions bound to runtime handlers), builds the iOS app (`.github/scripts/build-ios.sh`) and
uploads it to TestFlight with cloud signing (`.github/scripts/testflight.sh`). Nothing built from the game is
cached or kept.

Secrets: `GAME_KEY`, `APPLE_TEAM_ID`, `ASC_KEY_ID`, `ASC_ISSUER_ID`, `ASC_KEY_P8`.

## The disc image

The app bundles only the executable and the small loose disc files. Copy the full disc image
(`God of War (USA).iso`, 7.9 GB) into the app's folder on the phone: Files app → On My iPhone → GoWPad, or the
Apple Devices app on Windows → the phone → Files → GoWPad. Any single `.iso` there is used.

Never commit the game, the disc image, or generated code; this repository stays private.