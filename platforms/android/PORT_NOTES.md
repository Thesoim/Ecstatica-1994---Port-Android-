# Ecstatica for Android — port by The Soim (v1.0.0)

Package `thesoim.ecstatica`, version 1.0.0, Telegram https://t.me/thesoim.
Native build of the open-source C engine in `src/` (no emulator). The original
game data is bundled in the APK (`assets/game.zip`, unpacked silently on first launch, no progress screen)
together with the GPL soundfont TimGM6mb for the music.

## Build
    export ANDROID_HOME=/path/to/sdk   # NDK, build-tools, platform android-34, cmake, ninja
    platforms/android/build.sh v1.0.0  # -> platforms/android/build/ecstatica.apk
ABIs: arm64-v8a, armeabi-v7a, x86_64, x86 (override: `ABIS="arm64-v8a"`). Min Android 8.0 (AAudio).

## What this port changes
* **Touch everywhere.** The engine's own menus only ever read keyboard/pad; they
  now take taps (tap = activate, press highlights, drag scrolls the save list,
  tap outside a panel = back). One tap during the intro skips it, once, straight into the game: the engine runs its own scene scripts to their end in a moment with nothing drawn, no sound and no waiting (nothing is sped up on screen), so you land in exactly the state the intro would have left. Taps in play are controls, not skips. The menu is the top-right button in play.
  A tap shorter than a frame is held for 3 frames so it is never lost.
* **Controls in the game's own style**: bevelled red pixel plates in the game's
  palette (0xC0–0xDF), one pixel-art icon per function (sword = attack, curved
  arrows = left/right swing, hand + L / R = left / right hand, flask = use item,
  pointing hand = interact/pick up, chevrons = jump, footprints = speed mode,
  eye = HUD, bars = menu) and a stick plate whose arrows light up with the
  direction. Pressed state: inverted bevel, brighter colours, icon sinks one pixel.
  Shown only in real gameplay (`port_gameplay_active()`), never in menus/title.
* Multi-touch, floating or fixed left stick, controls released on any screen
  change / focus loss / cancel.
* **Settings** (Android): Language, Music, Sound FX, Subtitles, Scale Mode
  (Pillarbox / Crop / **Stretch = full 16:9, default**), **Smoothing** (sharp or
  bilinear), **Resolution** (Native / 720p / 540p window buffer, for weak CPUs),
  and **Touch Controls**: show/hide, floating/fixed stick, opacity, size,
  **Edit Layout** (drag, -/+ size, hide/show, reset), reset layout. Saved in
  `touch_layout.cfg` and `port_android.cfg`.
* **Gamepads**: any Android pad with no setup: A/B/X/Y, shoulders, triggers (axis
  or key), stick clicks, Start/Select/Mode, d-pad (keys or hat), generic
  BUTTON_1..16, right stick on Z/RZ (RX/RY fallback), dead zone with rescale.
  Touch controls hide while a pad is used and return on touch.
* Telegram badge (top-left), "Port By The Soim", "Release v1.0.0": menus and
  the title card only (hidden in play and in the intro scenes). Tapping the badge opens `tg://resolve?domain=thesoim` (the channel
  in the Telegram app), else https://t.me/thesoim.
* "Port Credits" in the main menu.

## Honest limits
* The art is pre-rendered 4:3. Stretch fills a 16:9 screen by widening it;
  Pillarbox keeps the true shape with side bars.
* No frame-rate option: the engine has none. Resolution/Smoothing are the
  performance/quality options that really exist here.
* Several pads at once are merged into one.
* See the delivery notes for exactly what was and was not tested.

## Clean start every time
The engine keeps its state in C globals. Android keeps a closed app's process
around and reuses it, which replayed stale state on the next start (intro that
no longer skipped). `GameActivity.onDestroy` now ends the game process, and
`android_main` also resets the backend state, so each launch is a clean one.
