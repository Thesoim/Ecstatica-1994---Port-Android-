# Ecstatica Android (thesoim.ecstatica v1.0.0) — build

Requirements: JDK 11+, Android SDK with `ndk;27.2.12479018`, `platforms;android-34`,
`build-tools;35.0.0`, plus `cmake` and `ninja` (the SDK's `cmake;3.22.1` works).

    export ANDROID_HOME=/path/to/android-sdk
    platforms/android/build.sh v1.0.0
    # -> platforms/android/build/ecstatica.apk   (arm64-v8a, armeabi-v7a, x86_64)

Single ABI: `ABIS="arm64-v8a" platforms/android/build.sh v1.0.0`
Install: `adb install -r platforms/android/build/ecstatica.apk`

Game data is already bundled (platforms/android/assets/game.zip); the app unpacks
it on first launch and starts the game. Details: platforms/android/PORT_NOTES.md
