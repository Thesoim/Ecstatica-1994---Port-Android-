<div align="center">
  <img src="docs/media/capsule_616x353.jpg" alt="Ecstatica — artwork" width="616">
  <h1>Ecstatica — Android Port</h1>
  <p>An Android port of Ecstatica, based on the open-source Ecstatica Recompiled engine project.</p>
  <p><a href="README.md">العربية</a></p>
  <p>
    <img src="https://img.shields.io/badge/Android-8.0%2B-3DDC84?logo=android&logoColor=white" alt="Android 8.0+">
    <img src="https://img.shields.io/badge/Release-1.0.0-6D597A" alt="Release 1.0.0">
    <img src="https://img.shields.io/badge/Code%20License-GPL--3.0-315A9B" alt="Code license GPL-3.0">
  </p>
</div>

## About

This project brings **Ecstatica (1994)** to Android using the C engine reimplementation from [Ecstatica Recompiled](https://github.com/spacefarergames/EcstaticaRecompiled). This release adds an Android port with touch input and configurable on-screen controls.

## In-game screenshots

<table>
  <tr>
    <td width="50%"><img src="docs/media/android-gameplay-01.png" alt="Gameplay screenshot with touch controls" width="100%"></td>
    <td width="50%"><img src="docs/media/android-gameplay-02.png" alt="Gameplay and combat screenshot with touch controls" width="100%"></td>
  </tr>
  <tr>
    <td align="center">Gameplay and on-screen controls</td>
    <td align="center">Gameplay scene with touch controls</td>
  </tr>
</table>

## Android changes by The Soim

- Added game-styled touch controls, with a fixed or floating stick and options to adjust control placement, size, and opacity.
- Added touch navigation for menus, alongside support for gamepads, keyboards, and mice.
- Added Android settings for language, audio, subtitles, display scaling, smoothing, and render resolution.
- Added clean session startup on relaunch, with a separate launcher activity and game activity.
- Bundled the game data for automatic setup on first launch.

For implementation details and port limitations, see [Android port notes](platforms/android/PORT_NOTES.md) and the [build guide](BUILD_ANDROID.md).

## Build and install

Requirements: JDK 11 or later, Android SDK, NDK `27.2.12479018`, Android platform 34, build-tools `35.0.0`, CMake, and Ninja.

```bash
export ANDROID_HOME=/path/to/android-sdk
platforms/android/build.sh v1.0.0
adb install -r platforms/android/build/ecstatica.apk
```

The APK is generated locally at `platforms/android/build/ecstatica.apk`. To build a single ABI, for example:

```bash
ABIS="arm64-v8a" platforms/android/build.sh v1.0.0
```

## Repository layout

```text
assets/              Project assets and artwork
 decomp/             Decompilation notes and files
 docs/                Documentation and screenshots
 platforms/android/   Android app, resources, and assets
 src/                 Shared C engine source
```

## Attribution, rights, and license

- **Upstream project:** [Ecstatica Recompiled](https://github.com/spacefarergames/EcstaticaRecompiled) by Spacefarer Games. Original source, documentation, and license notices are retained in this repository.
- **Android port and Android-specific changes:** The Soim.
- **Code license:** GNU GPL v3; see [`LICENSE`](LICENSE). This license does not automatically cover the original game data.
- **Game data:** `platforms/android/assets/game.zip` contains original Ecstatica data. Those materials remain the property of their respective rights holders, are separate from the code license, and are included with redistribution permission.
- See [`NOTICE.md`](NOTICE.md) for attribution details.

---

<p align="center">Android port and changes by The Soim · Release 1.0.0</p>
