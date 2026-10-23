# GoWPad

My personal, native iPhone/iPad build of **God of War (USA, SCUS-97399)**. Nothing is emulated: the game's code runs as
native ARM64 code on the phone.

- **Runtime:** Waffle565's [God of War native port](https://github.com/Waffle565/God-Of-War-Native-Port) runtime
  (its `switch` branch: a fork of [PS2Recomp](https://github.com/ran-j/PS2Recomp), GPL-3.0), vendored in `PS2Recomp/`
  with my iOS port: the GS renderer on OpenGL ES 3.0 (GLSL ES shaders, framebuffer fetch for the blend and
  destination-alpha reads), raylib on SDL2, an app bundle, and `src/lib/ps2_ios_runtime.cpp` for start-up.
- **Game code:** the EE executable statically recompiled to C++ (6,418 functions), the game's VU1 microprograms compiled
  ahead of time to C++, and its 8 IOP modules compiled ahead of time to C++. All three are generated on my PC from my
  own disc (config: KIexster/god-of-war-recomp `3f349b2`) and arrive in the encrypted game input; game-specific
  overrides are in `game/overrides/` (Waffle565's `port/overrides-usa`).

## How a build happens

`.github/workflows/testflight.yml` (run by hand) decrypts my game input (release asset `game-input` /
`gow-native-input.tar.enc`, key in the `GAME_KEY` secret) inside the Mac runner and builds the iOS app
(`.github/scripts/build-ios.sh`). Mode `simulator` runs it in the iOS Simulator (`simulator-check.sh`; no disc there, so
start-up and boot only); mode `testflight` uploads it with cloud signing (`.github/scripts/testflight.sh`). Nothing
built from the game is cached; only the simulator screenshots are kept, for one day.

Secrets: `GAME_KEY`, `APPLE_TEAM_ID`, `ASC_KEY_ID`, `ASC_ISSUER_ID`, `ASC_KEY_P8`.

## On the phone

- Copy the disc image (`God of War (USA).iso`, 7.9 GB) into the app's folder: Files app → On My iPhone → GoWPad, or
  the Apple Devices app on Windows → the phone → Files → GoWPad. Any single `.iso` there is used.
- A game controller is needed (no touch controls).
- The same folder holds the memory card (`GodOfWar.ps2`), `log.txt` (the last run; `log-prev.txt` the one before) and
  an optional `env.txt` with `KEY=VALUE` lines, e.g. `PS2X_GS_HW_SCALE=1` for the native resolution.

Never commit the game, the disc image, or generated code; this repository stays private.
