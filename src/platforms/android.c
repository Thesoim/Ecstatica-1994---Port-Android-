/**
 * platforms/android.c
 *
 * Android backend (NDK NativeActivity), built by platforms/android/build.sh.
 *
 *   Entry     android_main, on the native_app_glue thread, which then runs the
 *             engine's main() (compiled as ecs_main). The engine's blocking
 *             loops are fine there; platform_pump_events drains the looper.
 *   Video     The window buffer is sized to the screen's aspect at no more
 *             than 960 lines and the compositor scales it up. screen.c
 *             expands the frame into it and the on-screen pad is drawn on top.
 *   Input     Touch (on-screen pad and pointer), keyboards, gamepads, and the
 *             Back button as Escape.
 *   Audio     AAudio callback pulling mixer.c.
 *
 * The launcher (platforms/android/java) imports the game folders into the
 * app's external files directory and starts this activity with the game to
 * run, "e1" or "e2", as the intent extra "game".
 */

#ifdef __ANDROID__

#include "../platform.h"
#include "../asm_f.h"
#include "../types.h"
#include "mixer.h"
#include "screen.h"
#include "touch_controls.h"
#include "port_android.h"

#include <aaudio/AAudio.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <math.h>
#include <unistd.h>

int ecs_main(int argc, char *argv[]);
bool port_gameplay_active(void);   /* menu.c */
bool port_overlay_active(void);    /* menu.c */
bool port_wants_skip_tap(void);    /* menu.c */
extern bool port_skip_request;     /* menu.c */
bool menu_is_active(void);         /* menu.c */

#define PKEY_TABLE_SIZE 256
/* Window buffers taller than this cost fill time without looking better: the
 * game is at most 480 lines. */
#define MAX_BUFFER_H 960
static const int k_res_max_h[3] = { MAX_BUFFER_H, 720, 540 };

struct platform_t {
    int render_w, render_h;
    int scale_mode;

    bool keys_now[PKEY_TABLE_SIZE];
    bool keys_prev[PKEY_TABLE_SIZE];
    bool keys_latch[PKEY_TABLE_SIZE];

    int mouse_x, mouse_y;
    int mouse_buttons;
    bool real_mouse;        /* a mouse moved last, not a touch */

    uint64_t start_ns;
};

static platform_t g_plat;
static struct android_app *s_app;
static ANativeWindow *s_window;
static int  s_buf_w, s_buf_h;           /* window buffer geometry */
static int  s_win_w, s_win_h;           /* window in pixels, for input */
static bool s_resumed, s_focused;
static char s_files_dir[512];

static volatile bool s_silent;           /* intro skip running: nothing shown, nothing heard */
static int  s_res_level;
static bool s_smooth;
static char s_prefs_path[640];
static bool s_ptr_was_down;

static platform_gamepad_state_t s_pad;
static bool s_pad_seen;
/* Some pads report the triggers as axes, some as L2/R2 keys, some as both. */
static bool s_key_lt, s_key_rt, s_axis_lt, s_axis_rt;
static bool s_trig_seen, s_zrz_seen;   /* which axes this pad really uses */

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ── Window ──────────────────────────────────────────────── */

static void window_setup(ANativeWindow *win) {
    s_window = win;
    if (!win) return;
    s_win_w = ANativeWindow_getWidth(win);
    s_win_h = ANativeWindow_getHeight(win);
    int max_h = k_res_max_h[s_res_level];
    int div = (s_win_h + max_h - 1) / max_h;
    if (div < 1) div = 1;
    s_buf_w = s_win_w / div;
    s_buf_h = s_win_h / div;
    ANativeWindow_setBuffersGeometry(win, s_buf_w, s_buf_h, WINDOW_FORMAT_RGBX_8888);
    touch_layout(s_buf_w, s_buf_h);
}

/* Window pixels to buffer pixels. */
static void to_buffer(float x, float y, int *bx, int *by) {
    *bx = s_win_w > 0 ? (int)(x * s_buf_w / s_win_w) : 0;
    *by = s_win_h > 0 ? (int)(y * s_buf_h / s_win_h) : 0;
}

/* ── Display preferences ─────────────────────────────────── */

static void prefs_save(void) {
    if (!s_prefs_path[0]) return;
    FILE *f = fopen(s_prefs_path, "w");
    if (!f) return;
    fprintf(f, "smooth=%d\nresolution=%d\n", s_smooth, s_res_level);
    fclose(f);
}

static void prefs_load(const char *dir) {
    snprintf(s_prefs_path, sizeof(s_prefs_path), "%s/port_android.cfg", dir);
    FILE *f = fopen(s_prefs_path, "r");
    if (!f) return;
    char line[96];
    int v;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "smooth=%d", &v) == 1) s_smooth = v != 0;
        else if (sscanf(line, "resolution=%d", &v) == 1 && v >= 0 && v <= 2) s_res_level = v;
    }
    fclose(f);
}

bool port_get_smooth(void) { return s_smooth; }
void port_set_smooth(bool smooth) {
    s_smooth = smooth;
    screen_set_smooth(smooth);
    prefs_save();
}
int port_get_resolution(void) { return s_res_level; }
void port_set_resolution(int level) {
    if (level < 0 || level > 2 || level == s_res_level) return;
    s_res_level = level;
    prefs_save();
    if (s_window) window_setup(s_window);       /* new buffer geometry, new touch layout */
}

/* ── Audio ───────────────────────────────────────────────── */

static AAudioStream *s_stream;

static aaudio_data_callback_result_t audio_cb(AAudioStream *stream, void *user,
                                              void *data, int32_t frames) {
    (void)stream;
    (void)user;
    mixer_render((int16_t *)data, frames);
    if (s_silent) memset(data, 0, (size_t)frames * 2 * sizeof(int16_t));   /* stereo, 16-bit */
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void audio_pause(bool pause) {
    if (!s_stream) return;
    if (pause) AAudioStream_requestPause(s_stream);
    else AAudioStream_requestStart(s_stream);
}

void platform_audio_init(void) {
    if (s_stream) return;
    AAudioStreamBuilder *b;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return;
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(b, audio_cb, NULL);
    AAudioStream *st = NULL;
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &st);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK || !st) {
        DBG_LOG(1, "[AUDIO] AAudio open failed: %s\n", AAudio_convertResultToText(r));
        return;
    }
    mixer_init(AAudioStream_getSampleRate(st), s_files_dir);
    s_stream = st;
    if (s_resumed) AAudioStream_requestStart(st);
}

void platform_audio_shutdown(void) {
    if (!s_stream) return;
    AAudioStream_requestStop(s_stream);
    AAudioStream_close(s_stream);
    s_stream = NULL;
    mixer_shutdown();
}

/* ── Lifecycle ───────────────────────────────────────────── */

static void on_cmd(struct android_app *app, int32_t cmd) {
    switch (cmd) {
    case APP_CMD_INIT_WINDOW:
        window_setup(app->window);
        break;
    case APP_CMD_TERM_WINDOW:
        s_window = NULL;
        touch_cancel_all();
        break;
    case APP_CMD_WINDOW_RESIZED:
    case APP_CMD_CONFIG_CHANGED:
        if (app->window) window_setup(app->window);
        break;
    case APP_CMD_GAINED_FOCUS:
        s_focused = true;
        break;
    case APP_CMD_LOST_FOCUS:
        s_focused = false;
        touch_cancel_all();
        memset(g_plat.keys_now, 0, sizeof(g_plat.keys_now));
        break;
    case APP_CMD_RESUME:
        s_resumed = true;
        audio_pause(false);
        break;
    case APP_CMD_PAUSE:
        s_resumed = false;
        audio_pause(true);
        break;
    default:
        break;
    }
}

/* ── Input ───────────────────────────────────────────────── */

static int keycode_to_pkey(int32_t k) {
    switch (k) {
    case AKEYCODE_ESCAPE: case AKEYCODE_BACK: return PKEY_ESCAPE;
    case AKEYCODE_ENTER: case AKEYCODE_NUMPAD_ENTER: return PKEY_RETURN;
    case AKEYCODE_SPACE: return PKEY_SPACE;
    case AKEYCODE_DPAD_UP: return PKEY_UP;
    case AKEYCODE_DPAD_DOWN: return PKEY_DOWN;
    case AKEYCODE_DPAD_LEFT: return PKEY_LEFT;
    case AKEYCODE_DPAD_RIGHT: return PKEY_RIGHT;
    case AKEYCODE_F1: return PKEY_F1;   case AKEYCODE_F2: return PKEY_F2;
    case AKEYCODE_F3: return PKEY_F3;   case AKEYCODE_F4: return PKEY_F4;
    case AKEYCODE_F5: return PKEY_F5;   case AKEYCODE_F6: return PKEY_F6;
    case AKEYCODE_F7: return PKEY_F7;   case AKEYCODE_F8: return PKEY_F8;
    case AKEYCODE_F9: return PKEY_F9;   case AKEYCODE_F10: return PKEY_F10;
    case AKEYCODE_F11: return PKEY_F11; case AKEYCODE_F12: return PKEY_F12;
    case AKEYCODE_A: return PKEY_A; case AKEYCODE_B: return PKEY_B;
    case AKEYCODE_C: return PKEY_C; case AKEYCODE_D: return PKEY_D;
    case AKEYCODE_E: return PKEY_E; case AKEYCODE_F: return PKEY_F;
    case AKEYCODE_G: return PKEY_G; case AKEYCODE_H: return PKEY_H;
    case AKEYCODE_I: return PKEY_I; case AKEYCODE_L: return PKEY_L;
    case AKEYCODE_M: return PKEY_M; case AKEYCODE_N: return PKEY_N;
    case AKEYCODE_O: return PKEY_O; case AKEYCODE_P: return PKEY_P;
    case AKEYCODE_Q: return PKEY_Q; case AKEYCODE_R: return PKEY_R;
    case AKEYCODE_S: return PKEY_S; case AKEYCODE_T: return PKEY_T;
    case AKEYCODE_V: return PKEY_V; case AKEYCODE_W: return PKEY_W;
    case AKEYCODE_X: return PKEY_X; case AKEYCODE_Z: return PKEY_Z;
    case AKEYCODE_CTRL_LEFT: case AKEYCODE_CTRL_RIGHT: return PKEY_LCTRL;
    case AKEYCODE_ALT_LEFT: case AKEYCODE_ALT_RIGHT: return PKEY_LALT;
    case AKEYCODE_SHIFT_LEFT: return PKEY_LSHIFT;
    case AKEYCODE_SHIFT_RIGHT: return PKEY_RSHIFT;
    case AKEYCODE_NUMPAD_1: return PKEY_NUM1; case AKEYCODE_NUMPAD_2: return PKEY_NUM2;
    case AKEYCODE_NUMPAD_3: return PKEY_NUM3; case AKEYCODE_NUMPAD_4: return PKEY_NUM4;
    case AKEYCODE_NUMPAD_5: return PKEY_NUM5; case AKEYCODE_NUMPAD_6: return PKEY_NUM6;
    case AKEYCODE_NUMPAD_7: return PKEY_NUM7; case AKEYCODE_NUMPAD_8: return PKEY_NUM8;
    case AKEYCODE_NUMPAD_9: return PKEY_NUM9;
    case AKEYCODE_1: return PKEY_1; case AKEYCODE_2: return PKEY_2; case AKEYCODE_3: return PKEY_3;
    case AKEYCODE_TAB: return PKEY_TAB;
    case AKEYCODE_MINUS: return PKEY_MINUS;
    case AKEYCODE_EQUALS: return PKEY_EQUALS;
    case AKEYCODE_LEFT_BRACKET: return PKEY_LBRACKET;
    case AKEYCODE_RIGHT_BRACKET: return PKEY_RBRACKET;
    case AKEYCODE_COMMA: return PKEY_COMMA;
    case AKEYCODE_PERIOD: return PKEY_PERIOD;
    case AKEYCODE_MOVE_HOME: return PKEY_HOME;
    case AKEYCODE_PAGE_UP: return PKEY_PGUP;
    case AKEYCODE_MOVE_END: return PKEY_END;
    case AKEYCODE_PAGE_DOWN: return PKEY_PGDN;
    default: return -1;
    }
}

/* Gamepad buttons; false if the key is not one. Any pad that speaks standard
 * Android codes works with no setup: A/B/X/Y, shoulders and triggers, sticks'
 * clicks, Start/Select, the d-pad, and the generic BUTTON_1..16 that cheap
 * pads and some controller apps send. */
static bool pad_button(int32_t k, bool down) {
    switch (k) {
    case AKEYCODE_BUTTON_A: case AKEYCODE_BUTTON_1: s_pad.btn_south = down; break;
    case AKEYCODE_BUTTON_B: case AKEYCODE_BUTTON_2: s_pad.btn_east = down; break;
    case AKEYCODE_BUTTON_X: case AKEYCODE_BUTTON_3: s_pad.btn_west = down; break;
    case AKEYCODE_BUTTON_Y: case AKEYCODE_BUTTON_4: s_pad.btn_north = down; break;
    case AKEYCODE_BUTTON_L1: case AKEYCODE_BUTTON_5: s_pad.btn_lb = down; break;
    case AKEYCODE_BUTTON_R1: case AKEYCODE_BUTTON_6: s_pad.btn_rb = down; break;
    case AKEYCODE_BUTTON_L2: case AKEYCODE_BUTTON_7: s_key_lt = down; s_trig_seen = true; break;
    case AKEYCODE_BUTTON_R2: case AKEYCODE_BUTTON_8: s_key_rt = down; s_trig_seen = true; break;
    case AKEYCODE_BUTTON_SELECT: case AKEYCODE_BUTTON_9: s_pad.btn_select = down; break;
    case AKEYCODE_BUTTON_START: case AKEYCODE_BUTTON_10: s_pad.btn_start = down; break;
    case AKEYCODE_BUTTON_THUMBL: case AKEYCODE_BUTTON_11: s_pad.btn_lstick = down; break;
    case AKEYCODE_BUTTON_THUMBR: case AKEYCODE_BUTTON_12: s_pad.btn_rstick = down; break;
    case AKEYCODE_BUTTON_C: s_pad.btn_east = down; break;
    case AKEYCODE_BUTTON_Z: s_pad.btn_west = down; break;
    case AKEYCODE_BUTTON_MODE: s_pad.btn_start = down; break;
    case AKEYCODE_DPAD_UP: s_pad.dpad_up = down; break;
    case AKEYCODE_DPAD_DOWN: s_pad.dpad_down = down; break;
    case AKEYCODE_DPAD_LEFT: s_pad.dpad_left = down; break;
    case AKEYCODE_DPAD_RIGHT: s_pad.dpad_right = down; break;
    case AKEYCODE_DPAD_CENTER: s_pad.btn_south = down; break;
    default: return false;
    }
    return true;
}

/* A BUTTON_* code is a pad's whatever the device says its source is. */
static bool is_button_code(int32_t k) {
    return (k >= AKEYCODE_BUTTON_A && k <= AKEYCODE_BUTTON_MODE) ||
           (k >= AKEYCODE_BUTTON_1 && k <= AKEYCODE_BUTTON_16);
}

static bool from_gamepad(int32_t source) {
    return (source & AINPUT_SOURCE_GAMEPAD) == AINPUT_SOURCE_GAMEPAD ||
           (source & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK;
}

/* Radial dead zone with the rest of the travel rescaled to the full range, so
 * a worn stick neither drifts nor loses its top end. */
static void stick(float x, float y, int16_t *ox, int16_t *oy) {
    const float dz = 0.18f;
    float mag = sqrtf(x * x + y * y);
    if (mag < dz) { *ox = *oy = 0; return; }
    if (mag > 1.0f) mag = 1.0f;
    float scale = (mag - dz) / (1.0f - dz) / sqrtf(x * x + y * y);
    float fx = x * scale, fy = y * scale;
    int ix = (int)(fx * 32767.0f), iy = (int)(fy * 32767.0f);
    *ox = (int16_t)(ix > 32767 ? 32767 : (ix < -32767 ? -32767 : ix));
    *oy = (int16_t)(iy > 32767 ? 32767 : (iy < -32767 ? -32767 : iy));
}

static int32_t on_key(const AInputEvent *e) {
    int32_t k = AKeyEvent_getKeyCode(e);
    int32_t action = AKeyEvent_getAction(e);
    if (action != AKEY_EVENT_ACTION_DOWN && action != AKEY_EVENT_ACTION_UP)
        return 0;
    bool down = action == AKEY_EVENT_ACTION_DOWN;

    /* Volume keys stay the system's. */
    if (k == AKEYCODE_VOLUME_UP || k == AKEYCODE_VOLUME_DOWN || k == AKEYCODE_VOLUME_MUTE)
        return 0;

    if ((from_gamepad(AInputEvent_getSource(e)) || is_button_code(k)) && pad_button(k, down)) {
        s_pad_seen = true;
        touch_hide();
        return 1;
    }
    int key = keycode_to_pkey(k);
    if (key < 0) return 0;
    if (down && AKeyEvent_getRepeatCount(e) == 0) g_plat.keys_latch[key] = true;
    g_plat.keys_now[key] = down;
    if (k != AKEYCODE_BACK) touch_hide();
    return 1;
}

static int32_t on_motion(const AInputEvent *e) {
    int32_t source = AInputEvent_getSource(e);
    int32_t action = AMotionEvent_getAction(e);
    int32_t masked = action & AMOTION_EVENT_ACTION_MASK;

    if ((source & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK) {
        s_pad_seen = true;
        float lx = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_X, 0);
        float ly = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_Y, 0);
        stick(lx, ly, &s_pad.left_x, &s_pad.left_y);
        s_pad.left_y = (int16_t)-s_pad.left_y;
        /* Right stick: Z/RZ on nearly every pad (Xbox, PlayStation, Shield,
         * Switch); RX/RY on a few HID ones, used only when Z/RZ never move
         * and RX/RY rest at centre, so triggers reported there are not read
         * as a stick. */
        float zx = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_Z, 0);
        float zy = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_RZ, 0);
        float rx = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_RX, 0);
        float ry = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_RY, 0);
        if (zx != 0.0f || zy != 0.0f) s_zrz_seen = true;
        float cx = zx, cy = zy;
        if (!s_zrz_seen && s_trig_seen && rx > -0.5f && ry > -0.5f && (rx < 0.0f || ry < 0.0f || rx > 0.0f || ry > 0.0f)) {
            cx = rx;
            cy = ry;
        }
        stick(cx, cy, &s_pad.right_x, &s_pad.right_y);
        s_pad.right_y = (int16_t)-s_pad.right_y;
        float hx = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_X, 0);
        float hy = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_Y, 0);
        s_pad.dpad_left = hx < -0.5f;
        s_pad.dpad_right = hx > 0.5f;
        s_pad.dpad_up = hy < -0.5f;
        s_pad.dpad_down = hy > 0.5f;
        float lt = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_LTRIGGER, 0);
        float rt = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_RTRIGGER, 0);
        float brake = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_BRAKE, 0);
        float gas = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_GAS, 0);
        float lmax = lt > brake ? lt : brake, rmax = rt > gas ? rt : gas;
        if (lmax > 0.05f || rmax > 0.05f) s_trig_seen = true;
        s_axis_lt = lmax > 0.35f;
        s_axis_rt = rmax > 0.35f;
        /* Only a real push hides the touch controls, not a resting stick's noise. */
        if (fabsf(lx) > 0.5f || fabsf(ly) > 0.5f || fabsf(cx) > 0.5f || fabsf(cy) > 0.5f ||
            hx != 0.0f || hy != 0.0f || s_axis_lt || s_axis_rt)
            touch_hide();
        return 1;
    }

    if ((source & AINPUT_SOURCE_MOUSE) == AINPUT_SOURCE_MOUSE) {
        int bx, by;
        to_buffer(AMotionEvent_getX(e, 0), AMotionEvent_getY(e, 0), &bx, &by);
        screen_rect_t pic = screen_picture_rect(s_buf_w, s_buf_h, g_plat.scale_mode);
        int rx, ry;
        screen_to_render(&pic, g_plat.render_w, g_plat.render_h, bx, by, &rx, &ry);
        g_plat.mouse_x = rx;
        g_plat.mouse_y = ry;
        int32_t b = AMotionEvent_getButtonState(e);
        g_plat.mouse_buttons = ((b & AMOTION_EVENT_BUTTON_PRIMARY) ? PMOUSE_LEFT : 0) |
                               ((b & AMOTION_EVENT_BUTTON_SECONDARY) ? PMOUSE_RIGHT : 0) |
                               ((b & AMOTION_EVENT_BUTTON_TERTIARY) ? PMOUSE_MIDDLE : 0);
        g_plat.real_mouse = true;
        return 1;
    }

    if ((source & AINPUT_SOURCE_TOUCHSCREEN) == AINPUT_SOURCE_TOUCHSCREEN) {
        g_plat.real_mouse = false;
        size_t n = AMotionEvent_getPointerCount(e);
        if (masked == AMOTION_EVENT_ACTION_MOVE) {
            for (size_t i = 0; i < n; i++) {
                int bx, by;
                to_buffer(AMotionEvent_getX(e, i), AMotionEvent_getY(e, i), &bx, &by);
                touch_event(AMotionEvent_getPointerId(e, i), TOUCH_MOVE, bx, by);
            }
        } else if (masked == AMOTION_EVENT_ACTION_CANCEL) {
            touch_cancel_all();
        } else {
            size_t i = (size_t)((action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
                                AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
            int phase = (masked == AMOTION_EVENT_ACTION_DOWN ||
                         masked == AMOTION_EVENT_ACTION_POINTER_DOWN) ? TOUCH_DOWN : TOUCH_UP;
            if (i < n) {
                int bx, by;
                to_buffer(AMotionEvent_getX(e, i), AMotionEvent_getY(e, i), &bx, &by);
                touch_event(AMotionEvent_getPointerId(e, i), phase, bx, by);
            }
        }
        return 1;
    }
    return 0;
}

static int32_t on_input(struct android_app *app, AInputEvent *e) {
    (void)app;
    switch (AInputEvent_getType(e)) {
    case AINPUT_EVENT_TYPE_KEY: return on_key(e);
    case AINPUT_EVENT_TYPE_MOTION: return on_motion(e);
    default: return 0;
    }
}

/* Drains the looper; blocks while the app is in the background, so a game in
 * the background is paused rather than running blind. */
static bool drain_events(void) {
    for (;;) {
        bool active = s_resumed && s_window;
        int events;
        struct android_poll_source *src = NULL;
        int r = ALooper_pollOnce(active ? 0 : 250, NULL, &events, (void **)&src);
        if (src) src->process(s_app, src);
        if (s_app->destroyRequested) return false;
        if (r == ALOOPER_POLL_TIMEOUT && (s_resumed && s_window)) return true;
        if (r == ALOOPER_POLL_ERROR) return true;
    }
}

/* ── platform.h ──────────────────────────────────────────── */

platform_t *platform_init(const char *title, int fb_width, int fb_height, int scale) {
    (void)title;
    (void)scale;
    platform_t *p = &g_plat;
    p->render_w = fb_width;
    p->render_h = fb_height;
    p->mouse_x = fb_width / 2;
    p->mouse_y = fb_height / 2;
    p->start_ns = now_ns();
    return p;
}

bool platform_hires_supported(platform_t *p) {
    (void)p;
    return true;
}

bool platform_boot_low_res(platform_t *p) {
    (void)p;
    return false;
}

bool platform_scale_mode_supported(platform_t *p) {
    (void)p;
    return true;
}

void platform_set_scale_mode(platform_t *p, int mode) {
    if (p && mode >= SCALE_PILLARBOX && mode <= SCALE_STRETCH) p->scale_mode = mode;
}

int platform_crop_inset_y(platform_t *p, int render_h) {
    return p ? screen_crop_inset_y(s_buf_w, s_buf_h, render_h, p->scale_mode) : 0;
}

void platform_set_render_size(platform_t *p, int w, int h) {
    if (!p || w <= 0 || h <= 0) return;
    p->render_w = w;
    p->render_h = h;
    if (p->mouse_x >= w) p->mouse_x = w - 1;
    if (p->mouse_y >= h) p->mouse_y = h - 1;
}

void platform_blit(platform_t *p, const uint8_t *framebuffer, const uint8_t *palette) {
    if (!p || !framebuffer || !palette || !s_window) return;
    ANativeWindow_Buffer buf;
    if (ANativeWindow_lock(s_window, &buf, NULL) != 0) return;
    screen_present((uint32_t *)buf.bits, buf.width, buf.height, buf.stride,
                   framebuffer, p->render_w, p->render_h, palette, p->scale_mode);
    touch_draw((uint32_t *)buf.bits, buf.width, buf.height, buf.stride);
    ANativeWindow_unlockAndPost(s_window);
}

void platform_blit_rgba(platform_t *p, const uint8_t *framebuffer) {
    /* Only the debug overlay uses this, and it is off here. */
    (void)p;
    (void)framebuffer;
}

void platform_set_title(platform_t *p, const char *title) {
    (void)p;
    (void)title;
}

bool platform_pump_events(platform_t *p) {
    if (!p) return false;
    memcpy(p->keys_prev, p->keys_now, sizeof(p->keys_now));
    if (!drain_events()) return false;
    /* Controls exist only in real gameplay, not on the title or in menus. */
    touch_set_gameplay(port_gameplay_active());
    touch_set_overlay(port_overlay_active());

    if (!p->real_mouse) {
        bool down = false;
        int x = 0, y = 0;
        touch_pointer_frame(&down, &x, &y);
        if (down) {
            screen_rect_t pic = screen_picture_rect(s_buf_w, s_buf_h, p->scale_mode);
            screen_to_render(&pic, p->render_w, p->render_h, x, y, &p->mouse_x, &p->mouse_y);
        }
        p->mouse_buttons = down ? PMOUSE_LEFT : 0;
        /* One tap during the intro skips it, once. */
        if (down && !s_ptr_was_down && port_wants_skip_tap() && !touch_edit_active())
            port_skip_request = true;
        s_ptr_was_down = down;
    }
    return true;
}

bool platform_key_down(platform_t *p, int keycode) {
    if (!p || keycode < 0 || keycode >= PKEY_TABLE_SIZE) return false;
    return p->keys_now[keycode];
}

bool platform_key_pressed(platform_t *p, int keycode) {
    if (!p || keycode < 0 || keycode >= PKEY_TABLE_SIZE) return false;
    return p->keys_now[keycode] && !p->keys_prev[keycode];
}

bool platform_key_hit(platform_t *p, int keycode) {
    if (!p || keycode < 0 || keycode >= PKEY_TABLE_SIZE) return false;
    bool hit = p->keys_latch[keycode];
    p->keys_latch[keycode] = false;
    return hit;
}

int platform_mouse_state(platform_t *p, int *out_x, int *out_y) {
    if (!p) return 0;
    if (out_x) *out_x = p->mouse_x;
    if (out_y) *out_y = p->mouse_y;
    return p->mouse_buttons;
}

void platform_gamepad_poll(platform_t *p, platform_gamepad_state_t *state) {
    (void)p;
    *state = s_pad;
    state->connected = s_pad_seen;
    state->btn_lt = s_key_lt || s_axis_lt;
    state->btn_rt = s_key_rt || s_axis_rt;
    bool down;
    int x, y;
    touch_poll(state, &down, &x, &y);
}

/* The game clock: real time, plus whatever the intro skip has stepped over. It
 * never goes backwards. */
static uint64_t s_clock_last_ns, s_clock_virt_ns;

uint32_t platform_ticks(platform_t *p) {
    (void)p;
    uint64_t now = now_ns();
    if (!s_clock_last_ns) s_clock_last_ns = now;
    s_clock_virt_ns += now - s_clock_last_ns;
    s_clock_last_ns = now;
    return (uint32_t)(s_clock_virt_ns / 1000000ull);
}

/* Intro skip: the engine runs its scene scripts to the end with the clock
 * stepped by hand, nothing presented, nothing heard, no waiting. */
void port_clock_advance(uint32_t ms) { s_clock_virt_ns += (uint64_t)ms * 1000000ull; }
void port_set_silent(bool silent) { s_silent = silent; }
bool port_is_silent(void) { return s_silent; }

void platform_delay(uint32_t ms) {
    if (ms && !s_silent) usleep(ms * 1000u);
}

/* ── Files ───────────────────────────────────────────────── */

/* The intent extra "game", through JNI: NativeActivity has no C accessor. */
static void intent_game(char *out, size_t outlen) {
    snprintf(out, outlen, "e1");
    JavaVM *vm = s_app->activity->vm;
    JNIEnv *env = NULL;
    if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK || !env) return;

    jobject act = s_app->activity->clazz;
    jclass act_cls = (*env)->GetObjectClass(env, act);
    jmethodID get_intent = (*env)->GetMethodID(env, act_cls, "getIntent", "()Landroid/content/Intent;");
    jobject intent = (*env)->CallObjectMethod(env, act, get_intent);
    if (intent) {
        jclass intent_cls = (*env)->GetObjectClass(env, intent);
        jmethodID get_extra = (*env)->GetMethodID(env, intent_cls, "getStringExtra",
                                                  "(Ljava/lang/String;)Ljava/lang/String;");
        jstring key = (*env)->NewStringUTF(env, "game");
        jstring val = (jstring)(*env)->CallObjectMethod(env, intent, get_extra, key);
        if (val) {
            const char *s = (*env)->GetStringUTFChars(env, val, NULL);
            if (s && (strcmp(s, "e1") == 0 || strcmp(s, "e2") == 0))
                snprintf(out, outlen, "%s", s);
            if (s) (*env)->ReleaseStringUTFChars(env, val, s);
        }
    }
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*vm)->DetachCurrentThread(vm);
}

void platform_early_init(void) {
    char game[8], dir[600];
    intent_game(game, sizeof(game));
    snprintf(dir, sizeof(dir), "%s/%s", s_files_dir, game);
    if (file_dir_has_database(dir) && chdir(dir) == 0) {
        file_flush_path_cache();
        return;
    }
    __android_log_print(ANDROID_LOG_ERROR, "ecstatica", "no game data in %s", dir);
    ANativeActivity_finish(s_app->activity);
    exit(2);
}

/* Flash writes can stall; the log is written off the game thread. */
bool platform_slow_writes(void) {
    return true;
}

int platform_save_slot_count(void) {
    return 11;
}

void platform_save_path(char *buf, int bufsz, int slot, int game_version) {
    (void)game_version;   /* one folder per game */
    snprintf(buf, bufsz, "saved/%04d.ecs", slot);
}

void platform_save_prepare(void) {
    mkdir("saved", 0777);
}

void platform_shutdown(platform_t *p) {
    (void)p;
    platform_audio_shutdown();
}

/* Opens a link in the system browser or Telegram, through GameActivity. */
static void open_url(const char *url) {
    JavaVM *vm = s_app->activity->vm;
    JNIEnv *env = NULL;
    if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK || !env) return;
    jobject act = s_app->activity->clazz;
    jclass cls = (*env)->GetObjectClass(env, act);
    jmethodID m = (*env)->GetMethodID(env, cls, "openLink", "(Ljava/lang/String;)V");
    if (m) {
        jstring u = (*env)->NewStringUTF(env, url);
        (*env)->CallVoidMethod(env, act, m, u);
        (*env)->DeleteLocalRef(env, u);
    }
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*vm)->DetachCurrentThread(vm);
}

/* ── Entry ───────────────────────────────────────────────── */

void android_main(struct android_app *app) {
    /* Start from a known state even if the process was reused. */
    memset(&g_plat, 0, sizeof g_plat);
    s_window = NULL;
    s_resumed = s_focused = false;
    s_silent = false;
    port_skip_request = false;
    s_clock_last_ns = s_clock_virt_ns = 0;
    s_ptr_was_down = false;
    s_key_lt = s_key_rt = s_axis_lt = s_axis_rt = false;
    s_trig_seen = s_zrz_seen = s_pad_seen = false;
    memset(&s_pad, 0, sizeof s_pad);
    s_app = app;
    app->onAppCmd = on_cmd;
    app->onInputEvent = on_input;
    snprintf(s_files_dir, sizeof(s_files_dir), "%s",
             app->activity->externalDataPath ? app->activity->externalDataPath
                                             : app->activity->internalDataPath);
    prefs_load(s_files_dir);
    screen_set_smooth(s_smooth);
    touch_init();
    touch_set_config_dir(s_files_dir);
    touch_set_url_handler(open_url);

    /* The engine draws from its first frame; wait until there is a window. */
    while (!(s_window && s_resumed)) {
        int events;
        struct android_poll_source *src = NULL;
        ALooper_pollOnce(-1, NULL, &events, (void **)&src);
        if (src) src->process(app, src);
        if (app->destroyRequested) return;
    }

    char *argv[] = { "ecstatica", NULL };
    ecs_main(1, argv);
    ANativeActivity_finish(app->activity);
}

#endif /* __ANDROID__ */
