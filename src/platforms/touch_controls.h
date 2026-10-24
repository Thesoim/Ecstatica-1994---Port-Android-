/**
 * platforms/touch_controls.h
 *
 * On-screen controls for touch screens (Android, iOS), themed after
 * Ecstatica's dark palette. They are only live and visible in actual
 * gameplay (never on the title screen or menus), report a
 * platform_gamepad_state_t so win.c maps them exactly like a real pad, and
 * can be moved, resized, hidden and re-faded by the player.
 *
 * Outside gameplay the same layer draws the port's overlay (Telegram link,
 * "Port By" and "Release" labels) and turns a touch into the menu pointer.
 *
 * Coordinates are surface pixels: the buffer the backend presents, which
 * touch_draw() paints into. Events may come from another thread than the
 * game's.
 */

#ifndef PLATFORMS_TOUCH_CONTROLS_H
#define PLATFORMS_TOUCH_CONTROLS_H

#include "screen.h"
#include "../platform.h"

enum { TOUCH_DOWN, TOUCH_MOVE, TOUCH_UP };

#define PORT_VERSION_STRING "v1.0.0"
#define PORT_TELEGRAM_URL   "https://t.me/thesoim"

void touch_init(void);

/* Folder for touch_layout.cfg; loads the saved layout and settings. */
void touch_set_config_dir(const char *dir);

/* Called with the URL when the Telegram badge is tapped. */
void touch_set_url_handler(void (*fn)(const char *url));

/* Surface size; the controls scale with its shorter side. */
void touch_layout(int surf_w, int surf_h);

/* True while the player is really playing: no menu, no title or intro. */
void touch_set_gameplay(bool gameplay);

/* True where the port's overlay (Telegram badge, "Port By", "Release") may be
 * drawn: the menus and the title card, never the game or its cutscenes. */
void touch_set_overlay(bool on);

void touch_event(long id, int phase, int x, int y);
void touch_cancel_all(void);

/* Controls start hidden and appear with the first touch; the backend hides
 * them again when a real gamepad or keyboard is used. */
void touch_hide(void);

/* Adds the on-screen pad to *gp (sets connected while any control is
 * visible). The pointer is returned in surface pixels. */
void touch_poll(platform_gamepad_state_t *gp, bool *pointer_down,
                int *pointer_x, int *pointer_y);

/* Once per engine frame: the pointer for menus, with taps held long enough to
 * be seen. */
void touch_pointer_frame(bool *down, int *x, int *y);

void touch_draw(uint32_t *dst, int surf_w, int surf_h, int pitch);

/* Settings, stored in touch_layout.cfg. Opacity and size are percentages. */
bool touch_get_enabled(void);
void touch_set_enabled(bool on);
int  touch_get_opacity(void);          /* 20..100 */
void touch_set_opacity(int pct);
bool touch_get_floating(void);         /* floating stick on the left */
void touch_set_floating(bool on);
int  touch_get_size(void);             /* 60..150 */
void touch_set_size(int pct);
void touch_reset_layout(void);         /* positions, sizes, visibility */

/* Layout editor: drag a control to move it, - / + to resize the selected
 * one, HIDE / SHOW, RESET, DONE. The caller keeps the engine's frame loop
 * running until touch_edit_active() is false. */
void touch_edit_begin(void);
bool touch_edit_active(void);
void touch_edit_end(void);

#endif /* PLATFORMS_TOUCH_CONTROLS_H */
