/**
 * platforms/touch_controls.c
 *
 * See touch_controls.h. Bone-white icons on blackened iron with a blood-red
 * rim, to sit inside Ecstatica's palette. Everything is drawn with a few
 * software primitives into the window buffer after the game picture.
 */

#include "touch_controls.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tg_icon.h"
#include "text_masks.h"
#include "touch_icons.h"

enum {
    C_STICK, C_ATTACK, C_SWL, C_SWR, C_LHAND, C_RHAND, C_USE, C_INTERACT,
    C_JUMP, C_SPEED, C_HUD, C_MENU, C_COUNT,
    C_NONE = -1, C_POINTER = -2, C_TELEGRAM = -3, C_TOOL = -4
};

enum { T_MINUS, T_PLUS, T_TOGGLE, T_RESET, T_DONE, T_COUNT };

typedef struct {
    float ax, ay;       /* default centre, in units from the anchor corner */
    bool  right, top;   /* anchor corner */
    float r;            /* radius in units */
    bool  rect_menu;
} def_t;

/* Units: u = shorter side / 12. Left controls anchor bottom-left, the rest
 * bottom-right; the two system buttons sit top-right. */
static const def_t k_def[C_COUNT] = {
    [C_STICK]    = { 3.6f, 3.6f, false, false, 2.35f },
    [C_JUMP]     = { 2.2f, 8.9f, false, false, 0.85f },
    [C_SPEED]    = { 6.0f, 7.4f, false, false, 0.75f },
    /* Right thumb: three tight columns, the most used buttons nearest the edge. */
    [C_ATTACK]   = { 2.9f, 3.3f, true,  false, 1.20f },
    [C_SWR]      = { 2.9f, 5.9f, true,  false, 0.80f },
    [C_RHAND]    = { 2.9f, 8.4f, true,  false, 0.80f },
    [C_INTERACT] = { 5.6f, 2.3f, true,  false, 0.85f },
    [C_SWL]      = { 5.6f, 5.0f, true,  false, 0.80f },
    [C_LHAND]    = { 5.6f, 7.7f, true,  false, 0.80f },
    [C_USE]      = { 7.9f, 2.3f, true,  false, 0.80f },
    [C_HUD]      = { 4.2f, 1.2f, true,  true,  0.60f },
    [C_MENU]     = { 1.6f, 1.2f, true,  true,  0.60f },
};

typedef struct {
    float fx, fy;       /* centre as a fraction of the surface */
    int   scale;        /* percent, 50..220 */
    bool  hidden;
    bool  custom;       /* position moved by the player */
    int   x, y, r;      /* resolved, surface pixels */
} control_t;

#define MAX_TOUCHES 10

typedef struct {
    long id;
    int  control;       /* C_*, T_* + offset, C_POINTER..., or C_NONE for a free slot */
    int  x, y;
    int  ox, oy;        /* drag offset / floating-stick origin */
} touch_t;

static control_t s_ctl[C_COUNT];
static touch_t   s_touch[MAX_TOUCHES];
static int       s_w, s_h, s_unit;
static bool      s_visible, s_gameplay, s_edit, s_overlay_on;
static int       s_sel = C_ATTACK;
static bool      s_enabled = true;
static int       s_opacity = 85, s_size = 100;
static char      s_cfg_path[640];
static void    (*s_url_fn)(const char *);
static platform_sem_t *s_lock;
static struct { int x, y, w, h; } s_tool[T_COUNT];
static struct { int x, y, s; } s_tg;
/* Floating stick: a touch on the free left side steers from where it landed. */
static bool s_floating = true;      /* floating stick on the left side */
static int  s_ptr_latch, s_ptr_x, s_ptr_y;
static bool s_float_active;
static int  s_float_x, s_float_y;

static void lock(void)   { if (s_lock) platform_sem_wait(s_lock); }
static void unlock(void) { if (s_lock) platform_sem_post(s_lock); }

/* ── Config ────────────────────────────────────────────────── */

static void cfg_save(void) {
    if (!s_cfg_path[0]) return;
    FILE *f = fopen(s_cfg_path, "w");
    if (!f) return;
    fprintf(f, "enabled=%d\nopacity=%d\nsize=%d\nfloating=%d\n", s_enabled, s_opacity, s_size, s_floating);
    for (int c = 0; c < C_COUNT; c++)
        fprintf(f, "c%d=%d,%.4f,%.4f,%d,%d\n", c, s_ctl[c].custom, s_ctl[c].fx,
                s_ctl[c].fy, s_ctl[c].scale, s_ctl[c].hidden);
    fclose(f);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void cfg_load(void) {
    FILE *f = fopen(s_cfg_path, "r");
    if (!f) return;
    char line[160];
    while (fgets(line, sizeof line, f)) {
        int v, c, cu, sc, hi;
        float fx, fy;
        if (sscanf(line, "enabled=%d", &v) == 1) s_enabled = v != 0;
        else if (sscanf(line, "opacity=%d", &v) == 1) s_opacity = clampi(v, 20, 100);
        else if (sscanf(line, "size=%d", &v) == 1) s_size = clampi(v, 60, 150);
        else if (sscanf(line, "floating=%d", &v) == 1) s_floating = v != 0;
        else if (sscanf(line, "c%d=%d,%f,%f,%d,%d", &c, &cu, &fx, &fy, &sc, &hi) == 6 &&
                 c >= 0 && c < C_COUNT) {
            s_ctl[c].custom = cu != 0;
            s_ctl[c].fx = fx < 0 ? 0 : (fx > 1 ? 1 : fx);
            s_ctl[c].fy = fy < 0 ? 0 : (fy > 1 ? 1 : fy);
            s_ctl[c].scale = clampi(sc, 50, 220);
            s_ctl[c].hidden = hi != 0;
        }
    }
    fclose(f);
}

void touch_init(void) {
    if (!s_lock) s_lock = platform_sem_create(1);
    for (int i = 0; i < MAX_TOUCHES; i++) s_touch[i].control = C_NONE;
    for (int c = 0; c < C_COUNT; c++) s_ctl[c].scale = 100;
}

void touch_set_config_dir(const char *dir) {
    snprintf(s_cfg_path, sizeof s_cfg_path, "%s/touch_layout.cfg", dir);
    lock();
    cfg_load();
    s_w = s_h = 0;      /* force a re-layout with the loaded values */
    unlock();
}

void touch_set_url_handler(void (*fn)(const char *url)) { s_url_fn = fn; }

/* ── Layout ────────────────────────────────────────────────── */

static void resolve_all(void) {
    int u = s_unit;
    for (int c = 0; c < C_COUNT; c++) {
        const def_t *d = &k_def[c];
        control_t *k = &s_ctl[c];
        k->r = (int)(d->r * (float)u * (float)s_size / 100.0f * (float)k->scale / 100.0f);
        if (k->r < 6) k->r = 6;
        if (!k->custom) {
            float x = d->right ? (float)s_w - d->ax * (float)u : d->ax * (float)u;
            float y = d->top ? d->ay * (float)u : (float)s_h - d->ay * (float)u;
            k->fx = x / (float)s_w;
            k->fy = y / (float)s_h;
        }
        k->x = (int)(k->fx * (float)s_w);
        k->y = (int)(k->fy * (float)s_h);
    }
    /* Editor toolbar, centred along the top. */
    int bw = u * 28 / 10, bh = u * 9 / 10, gap = u / 4;
    int total = T_COUNT * bw + (T_COUNT - 1) * gap;
    int x0 = (s_w - total) / 2;
    for (int t = 0; t < T_COUNT; t++) {
        s_tool[t].x = x0 + t * (bw + gap);
        s_tool[t].y = u / 2;
        s_tool[t].w = bw;
        s_tool[t].h = bh;
    }
    s_tg.s = u * 17 / 10;
    s_tg.x = u / 2;
    s_tg.y = u / 2;
}

void touch_layout(int w, int h) {
    lock();
    if (w == s_w && h == s_h) { unlock(); return; }
    s_w = w;
    s_h = h;
    int u = (w < h ? w : h) / 12;
    s_unit = u < 8 ? 8 : u;
    resolve_all();
    unlock();
}

static void relayout(void) { resolve_all(); }

void touch_reset_layout(void) {
    lock();
    for (int c = 0; c < C_COUNT; c++) {
        s_ctl[c].custom = false;
        s_ctl[c].hidden = false;
        s_ctl[c].scale = 100;
    }
    s_size = 100;
    if (s_w) relayout();
    cfg_save();
    unlock();
}

bool touch_get_floating(void) { return s_floating; }
void touch_set_floating(bool on) { lock(); s_floating = on; s_float_active = false; cfg_save(); unlock(); }
bool touch_get_enabled(void) { return s_enabled; }
void touch_set_enabled(bool on) { lock(); s_enabled = on; cfg_save(); unlock(); }
int  touch_get_opacity(void) { return s_opacity; }
void touch_set_opacity(int p) { lock(); s_opacity = clampi(p, 20, 100); cfg_save(); unlock(); }
int  touch_get_size(void) { return s_size; }
void touch_set_size(int p) {
    lock();
    s_size = clampi(p, 60, 150);
    if (s_w) relayout();
    cfg_save();
    unlock();
}

void touch_edit_begin(void) {
    lock();
    s_edit = true;
    s_visible = true;
    for (int i = 0; i < MAX_TOUCHES; i++) s_touch[i].control = C_NONE;
    unlock();
}
bool touch_edit_active(void) { return s_edit; }
void touch_edit_end(void) {
    lock();
    s_edit = false;
    for (int i = 0; i < MAX_TOUCHES; i++) s_touch[i].control = C_NONE;
    cfg_save();
    unlock();
}

void touch_set_overlay(bool on) { s_overlay_on = on; }

void touch_set_gameplay(bool g) {
    lock();
    if (g != s_gameplay) {
        s_gameplay = g;
        /* Fingers still down belong to the other screen: let go of them so
         * no control or pointer can stay stuck across the switch. */
        for (int i = 0; i < MAX_TOUCHES; i++) s_touch[i].control = C_NONE;
        s_float_active = false;
    }
    unlock();
}

/* ── Input ─────────────────────────────────────────────────── */

static bool controls_live(void) { return s_visible && s_enabled && s_gameplay && !s_edit; }

static int hit(int x, int y) {
    for (int c = C_COUNT - 1; c >= 0; c--) {
        const control_t *k = &s_ctl[c];
        if (k->hidden && !s_edit) continue;
        int r = c == C_STICK ? k->r * 13 / 10 : k->r * 12 / 10;
        if (x >= k->x - r && x <= k->x + r && y >= k->y - r && y <= k->y + r) return c;
    }
    return C_NONE;
}

static int hit_tool(int x, int y) {
    for (int t = 0; t < T_COUNT; t++)
        if (x >= s_tool[t].x && x < s_tool[t].x + s_tool[t].w &&
            y >= s_tool[t].y && y < s_tool[t].y + s_tool[t].h) return t;
    return -1;
}

static bool in_tg(int x, int y) {
    return x >= s_tg.x && x < s_tg.x + s_tg.s && y >= s_tg.y && y < s_tg.y + s_tg.s;
}

static void tool_action(int t) {
    control_t *k = &s_ctl[s_sel];
    switch (t) {
    case T_MINUS: k->scale = clampi(k->scale - 10, 50, 220); relayout(); break;
    case T_PLUS:  k->scale = clampi(k->scale + 10, 50, 220); relayout(); break;
    case T_TOGGLE: k->hidden = !k->hidden; break;
    case T_RESET:
        for (int c = 0; c < C_COUNT; c++) {
            s_ctl[c].custom = false; s_ctl[c].hidden = false; s_ctl[c].scale = 100;
        }
        s_size = 100;
        relayout();
        break;
    case T_DONE:
        s_edit = false;
        for (int i = 0; i < MAX_TOUCHES; i++) s_touch[i].control = C_NONE;
        cfg_save();
        break;
    }
}

void touch_event(long id, int phase, int x, int y) {
    lock();
    int slot = -1;
    for (int i = 0; i < MAX_TOUCHES; i++)
        if (s_touch[i].control != C_NONE && s_touch[i].id == id) { slot = i; break; }

    if (phase == TOUCH_DOWN) {
        if (slot < 0)
            for (int i = 0; i < MAX_TOUCHES; i++)
                if (s_touch[i].control == C_NONE) { slot = i; break; }
        if (slot >= 0) {
            touch_t t = { id, C_NONE, x, y, 0, 0 };
            if (s_edit) {
                int tl = hit_tool(x, y);
                if (tl >= 0) { t.control = C_TOOL; t.ox = tl; tool_action(tl); }
                else {
                    int c = hit(x, y);
                    if (c >= 0) {
                        s_sel = c;
                        t.control = c;
                        t.ox = s_ctl[c].x - x;
                        t.oy = s_ctl[c].y - y;
                    } else t.control = C_TOOL, t.ox = -1;
                }
            } else if (controls_live()) {
                int c = hit(x, y);
                if (c == C_NONE && s_floating && x < s_w * 45 / 100 && !s_float_active) {
                    c = C_STICK;
                    s_float_active = true;
                    s_float_x = t.ox = x;
                    s_float_y = t.oy = y;
                }
                t.control = c == C_NONE ? C_POINTER : c;
            } else if (s_overlay_on && !s_gameplay && in_tg(x, y)) {
                t.control = C_TELEGRAM;
            } else {
                t.control = C_POINTER;
            }
            if (t.control == C_POINTER) {    /* One pointer at a time. */
                for (int i = 0; i < MAX_TOUCHES; i++)
                    if (s_touch[i].control == C_POINTER) s_touch[i].control = C_NONE;
                s_ptr_latch = 3;             /* a tap shorter than a frame still counts */
                s_ptr_x = x;
                s_ptr_y = y;
            }
            s_touch[slot] = t;
        }
        if (!s_gameplay || s_enabled) s_visible = true;
    } else if (slot >= 0) {
        touch_t *t = &s_touch[slot];
        if (phase == TOUCH_UP) {
            if (t->control == C_TELEGRAM && in_tg(x, y) && s_url_fn) s_url_fn(PORT_TELEGRAM_URL);
            if (t->control == C_STICK && s_float_active && t->ox) s_float_active = false;
            t->control = C_NONE;
        } else {
            t->x = x;
            t->y = y;
            if (s_edit && t->control >= 0 && t->control < C_COUNT) {
                control_t *k = &s_ctl[t->control];
                k->x = clampi(x + t->ox, 0, s_w);
                k->y = clampi(y + t->oy, 0, s_h);
                k->fx = (float)k->x / (float)s_w;
                k->fy = (float)k->y / (float)s_h;
                k->custom = true;
            }
        }
    }
    unlock();
}

void touch_cancel_all(void) {
    lock();
    for (int i = 0; i < MAX_TOUCHES; i++) s_touch[i].control = C_NONE;
    s_float_active = false;
    unlock();
}

void touch_hide(void) {
    lock();
    if (!s_edit) s_visible = false;
    for (int i = 0; i < MAX_TOUCHES; i++)
        if (s_touch[i].control != C_POINTER) s_touch[i].control = C_NONE;
    s_float_active = false;
    unlock();
}

static void stick_origin(const touch_t *t, int *cx, int *cy) {
    if (t->ox && t->control == C_STICK) { *cx = t->ox; *cy = t->oy; }
    else { *cx = s_ctl[C_STICK].x; *cy = s_ctl[C_STICK].y; }
}

static void stick_value(const touch_t *t, int16_t *ox, int16_t *oy) {
    int cx, cy, r = s_ctl[C_STICK].r;
    stick_origin(t, &cx, &cy);
    int dx = t->x - cx, dy = t->y - cy;
    int len2 = dx * dx + dy * dy;
    if (len2 > r * r) {
        int len = (int)sqrtf((float)len2);
        if (len < 1) len = 1;
        dx = dx * r / len;
        dy = dy * r / len;
    }
    *ox = (int16_t)(dx * 32767 / r);
    *oy = (int16_t)(-dy * 32767 / r);     /* win.c reads Y as positive-up */
}

/* Once per engine frame: the pointer as the menus and requesters see it. A
 * press is held for at least three frames, so a quick tap is never lost
 * between two polls. */
void touch_pointer_frame(bool *down, int *x, int *y) {
    lock();
    bool live = false;
    for (int i = 0; i < MAX_TOUCHES; i++)
        if (s_touch[i].control == C_POINTER) {
            live = true;
            s_ptr_x = s_touch[i].x;
            s_ptr_y = s_touch[i].y;
        }
    *down = live || s_ptr_latch > 0;
    *x = s_ptr_x;
    *y = s_ptr_y;
    if (s_ptr_latch > 0) s_ptr_latch--;
    unlock();
}

void touch_poll(platform_gamepad_state_t *gp, bool *pointer_down,
                int *pointer_x, int *pointer_y) {
    bool down[C_COUNT] = { false };
    *pointer_down = false;

    lock();
    for (int i = 0; i < MAX_TOUCHES; i++) {
        const touch_t *t = &s_touch[i];
        if (t->control == C_POINTER) {
            *pointer_down = true;
            *pointer_x = t->x;
            *pointer_y = t->y;
        } else if (t->control >= 0 && t->control < C_COUNT && !s_edit) {
            down[t->control] = true;
            if (t->control == C_STICK) stick_value(t, &gp->left_x, &gp->left_y);
        }
    }
    bool live = controls_live();
    unlock();

    if (!live) return;
    gp->connected  = true;
    gp->btn_south  |= down[C_INTERACT];   /* Space: pick up / interact      */
    gp->btn_west   |= down[C_USE];        /* Alt: use the held item         */
    gp->btn_lt     |= down[C_LHAND];      /* Num1: left hand                */
    gp->btn_rt     |= down[C_RHAND];      /* Num3: right hand               */
    gp->btn_rb     |= down[C_ATTACK];     /* Ctrl: attack with the stick    */
    gp->btn_lb     |= down[C_JUMP];       /* Shift: jump (standing)         */
    gp->btn_lstick |= down[C_SPEED];      /* sneak / walk / run             */
    gp->btn_select |= down[C_HUD];        /* I: HUD icons                   */
    gp->btn_start  |= down[C_MENU];       /* Esc: game menu                 */
    if (down[C_SWL]) gp->right_x = -32767;   /* Num7: left swing  */
    if (down[C_SWR]) gp->right_x =  32767;   /* Num9: right swing */
}

/* ── Drawing ───────────────────────────────────────────────
 * Everything is built from square "cells" the size of one art pixel, in the
 * game's own red ramp (palette 0xC0..0xDF) and with its bevelled look, so the
 * controls read as part of the original interface. */

typedef struct { uint32_t *px; int w, h, pitch; } surface_t;

#define COL(r, g, b) (0xFF000000u | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r))
#define R_DEEP   COL(28, 0, 0)        /* 0xC0 */
#define R_DARK   COL(48, 0, 0)        /* 0xC3: dark bevel */
#define R_FILL   COL(92, 0, 0)        /* 0xC9: panel fill */
#define R_FILLP  COL(124, 0, 0)       /* pressed fill */
#define R_LIGHT  COL(168, 4, 4)       /* light bevel, a step above 0xCF */
#define R_BODY   COL(200, 16, 16)     /* 0xD8: game text */
#define R_HI     COL(252, 60, 60)     /* 0xDF: highlights */
#define C_SHADOW COL(0, 0, 0)
#define C_BONE   COL(226, 212, 180)

static int s_alpha = 255;       /* global opacity of the pad, 0..255 */

static void put(const surface_t *s, int x, int y, uint32_t col, int a) {
    if (x < 0 || y < 0 || x >= s->w || y >= s->h || a <= 0) return;
    a = a * s_alpha / 255;
    if (a <= 0) return;
    uint32_t *p = &s->px[(size_t)y * (size_t)s->pitch + (size_t)x];
    uint32_t d = *p, out = 0xFF000000u;
    for (int sh = 0; sh < 24; sh += 8) {
        int dv = (int)((d >> sh) & 0xFF), sv = (int)((col >> sh) & 0xFF);
        out |= (uint32_t)(dv + (sv - dv) * a / 255) << sh;
    }
    *p = out;
}

static void rect(const surface_t *s, int x0, int y0, int x1, int y1, uint32_t col, int a) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s->w) x1 = s->w;
    if (y1 > s->h) y1 = s->h;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) put(s, x, y, col, a);
}

/* A rectangle of cells (gx, gy, gw, gh) on a grid of cell size p at (ox, oy). */
static void cells(const surface_t *s, int ox, int oy, int p, int gx, int gy, int gw, int gh,
                  uint32_t col, int a) {
    rect(s, ox + gx * p, oy + gy * p, ox + (gx + gw) * p, oy + (gy + gh) * p, col, a);
}

/* The game's gadget look: deep outline, light top-left and dark bottom-right
 * bevel (swapped when pressed), chamfered corners. g is the grid size. */
static void plate(const surface_t *s, int ox, int oy, int p, int g, bool pressed,
                  int fill_a, int line_a) {
    uint32_t tl = pressed ? R_DARK : R_LIGHT, br = pressed ? R_LIGHT : R_DARK;
    cells(s, ox, oy, p, 1, 0, g - 2, 1, R_DEEP, line_a);
    cells(s, ox, oy, p, 1, g - 1, g - 2, 1, R_DEEP, line_a);
    cells(s, ox, oy, p, 0, 1, 1, g - 2, R_DEEP, line_a);
    cells(s, ox, oy, p, g - 1, 1, 1, g - 2, R_DEEP, line_a);
    cells(s, ox, oy, p, 1, 1, g - 2, g - 2, pressed ? R_FILLP : R_FILL, fill_a);
    cells(s, ox, oy, p, 2, 1, g - 4, 1, tl, 255);          /* top */
    cells(s, ox, oy, p, 1, 2, 1, g - 4, tl, 255);          /* left */
    cells(s, ox, oy, p, 2, g - 2, g - 4, 1, br, 255);      /* bottom */
    cells(s, ox, oy, p, g - 2, 2, 1, g - 4, br, 255);      /* right */
}

static uint32_t icon_colour(char c, bool pressed) {
    switch (c) {
    case '1': return R_DEEP;
    case '2': return pressed ? R_BODY : COL(136, 0, 0);
    case '3': return pressed ? R_HI : R_BODY;
    default:  return pressed ? COL(255, 150, 150) : R_HI;
    }
}

static void draw_icon(const surface_t *s, const char *ic, int ox, int oy, int p, bool pressed) {
    for (int i = 0; i < 256; i++) {
        if (ic[i] == '.') continue;
        int x = i % 16, y = i / 16;
        cells(s, ox, oy, p, x, y, 1, 1, icon_colour(ic[i], pressed), 255);
    }
}

/* 5x7 glyphs for the editor toolbar, one byte per row, bit 4 leftmost. */
static const uint8_t *glyph(char c) {
    static const uint8_t az[26][7] = {
        {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
        {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},{0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},
        {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
        {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F},{0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
        {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},{0x07,0x02,0x02,0x02,0x02,0x12,0x0C},
        {0x11,0x12,0x14,0x18,0x14,0x12,0x11},{0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
        {0x11,0x1B,0x15,0x15,0x11,0x11,0x11},{0x11,0x11,0x19,0x15,0x13,0x11,0x11},
        {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
        {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
        {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},{0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
        {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},{0x11,0x11,0x11,0x11,0x11,0x0A,0x04},
        {0x11,0x11,0x11,0x15,0x15,0x15,0x0A},{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
        {0x11,0x11,0x0A,0x04,0x04,0x04,0x04},{0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},
    };
    static const uint8_t dg[10][7] = {
        {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
        {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},{0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E},
        {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
        {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},{0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
        {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},{0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
    };
    static const uint8_t plus[7] = {0x00,0x04,0x04,0x1F,0x04,0x04,0x00};
    static const uint8_t minus[7] = {0x00,0x00,0x00,0x1F,0x00,0x00,0x00};
    static const uint8_t pct[7] = {0x18,0x19,0x02,0x04,0x08,0x13,0x03};
    if (c >= 'A' && c <= 'Z') return az[c - 'A'];
    if (c >= '0' && c <= '9') return dg[c - '0'];
    if (c == '+') return plus;
    if (c == '-') return minus;
    if (c == '%') return pct;
    return NULL;
}

static void text(const surface_t *s, int cx, int cy, const char *t, int scale,
                 uint32_t col, int a) {
    int n = (int)strlen(t);
    int x = cx - (n * 6 - 1) * scale / 2;
    int y = cy - 7 * scale / 2;
    for (int i = 0; i < n; i++, x += 6 * scale) {
        const uint8_t *g = glyph(t[i]);
        if (!g) continue;
        for (int row = 0; row < 7; row++)
            for (int c = 0; c < 5; c++)
                if (g[row] & (0x10 >> c))
                    rect(s, x + c * scale, y + row * scale,
                         x + (c + 1) * scale, y + (row + 1) * scale, col, a);
    }
}

/* Alpha-mask text/overlay drawing: box-filtered down from the stored mask. */
static void blit_mask(const surface_t *s, const uint8_t *m, int mw, int mh,
                      int dx, int dy, int dw, int dh, uint32_t col, int a) {
    for (int y = 0; y < dh; y++) {
        int sy0 = y * mh / dh, sy1 = (y + 1) * mh / dh;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int x = 0; x < dw; x++) {
            int sx0 = x * mw / dw, sx1 = (x + 1) * mw / dw;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            int sum = 0, n = 0;
            for (int yy = sy0; yy < sy1 && yy < mh; yy++)
                for (int xx = sx0; xx < sx1 && xx < mw; xx++) { sum += m[yy * mw + xx]; n++; }
            if (n) put(s, dx + x, dy + y, col, sum / n * a / 255);
        }
    }
}

static void blit_icon(const surface_t *s, int dx, int dy, int size) {
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            uint32_t v = k_tg_icon[(y * TG_H / size) * TG_W + (x * TG_W / size)];
            int a = (int)(v >> 24);
            if (a) put(s, dx + x, dy + y, 0xFF000000u | (v & 0x00FFFFFFu), a);
        }
}

/* The port's own labels, drawn over menus and the title screen. */
static void draw_overlay(const surface_t *s) {
    int u = s_unit, save = s_alpha;
    s_alpha = 255;
    blit_icon(s, s_tg.x, s_tg.y, s_tg.s);

    int h1 = u * 45 / 100, h2 = u * 70 / 100;          /* small caption, name */
    int m = u / 2;
    int w;
    /* Bottom-left: "Port By" over "The Soim". Shadow first for legibility. */
    for (int pass = 0; pass < 2; pass++) {
        uint32_t col = pass == 0 ? C_SHADOW : C_BONE;
        int o = pass == 0 ? 2 : 0, a = pass == 0 ? 200 : 235;
        w = h1 * M_PORTBY_W / M_PORTBY_H;
        blit_mask(s, k_m_portby, M_PORTBY_W, M_PORTBY_H, m + o, s->h - m - h1 - h2 - u / 8 + o, w, h1, col, a);
        w = h2 * M_SOIM_W / M_SOIM_H;
        blit_mask(s, k_m_soim, M_SOIM_W, M_SOIM_H, m + o, s->h - m - h2 + o, w, h2, col, a);
        /* Bottom-right: "Release" over "v1.0.0", right aligned. */
        w = h1 * M_RELEASE_W / M_RELEASE_H;
        blit_mask(s, k_m_release, M_RELEASE_W, M_RELEASE_H, s->w - m - w + o,
                  s->h - m - h1 - h2 - u / 8 + o, w, h1, col, a);
        w = h2 * M_VER_W / M_VER_H;
        blit_mask(s, k_m_ver, M_VER_W, M_VER_H, s->w - m - w + o, s->h - m - h2 + o, w, h2, col, a);
    }
    s_alpha = save;
}

static const char *icon_of(int id) {
    switch (id) {
    case C_ATTACK:   return k_icon_attack;
    case C_SWL:      return k_icon_swl;
    case C_SWR:      return k_icon_swr;
    case C_LHAND:    return k_icon_lhand;
    case C_RHAND:    return k_icon_rhand;
    case C_USE:      return k_icon_use;
    case C_INTERACT: return k_icon_interact;
    case C_JUMP:     return k_icon_jump;
    case C_SPEED:    return k_icon_speed;
    case C_HUD:      return k_icon_hud;
    case C_MENU:     return k_icon_menu;
    }
    return NULL;
}

/* Pixel-art arrow, tip towards dir (0 up, 1 right, 2 down, 3 left), on a
 * grid whose centre is cell (c, c) and whose tip sits at distance d. */
static void arrow(const surface_t *s, int ox, int oy, int p, int c, int d, int dir, uint32_t col) {
    for (int i = 0; i < 4; i++) {
        switch (dir) {
        case 0: cells(s, ox, oy, p, c - i, c - d + i, 2 * i + 1, 1, col, 255); break;
        case 2: cells(s, ox, oy, p, c - i, c + d - i, 2 * i + 1, 1, col, 255); break;
        case 3: cells(s, ox, oy, p, c - d + i, c - i, 1, 2 * i + 1, col, 255); break;
        default: cells(s, ox, oy, p, c + d - i, c - i, 1, 2 * i + 1, col, 255); break;
        }
    }
}

static void draw_stick(const surface_t *s, const control_t *k, bool pressed, int kx, int ky, int vx, int vy) {
    const int G = 40;
    int p = (2 * k->r) / G;
    if (p < 2) p = 2;
    int ox = k->x - G * p / 2, oy = k->y - G * p / 2;
    plate(s, ox, oy, p, G, false, 150, 220);
    cells(s, ox, oy, p, 9, 9, 22, 22, R_DEEP, 120);                 /* the well */
    /* Eight directions: an arrow lights up when the stick leans that way. */
    int thr = 9000;
    bool up = vy > thr, down = vy < -thr, right = vx > thr, left = vx < -thr;
    arrow(s, ox, oy, p, 20, 17, 0, up ? R_HI : R_BODY);
    arrow(s, ox, oy, p, 20, 17, 2, down ? R_HI : R_BODY);
    arrow(s, ox, oy, p, 20, 17, 3, left ? R_HI : R_BODY);
    arrow(s, ox, oy, p, 20, 17, 1, right ? R_HI : R_BODY);
    /* The knob: an octagon with the same bevel as the buttons. */
    int kox = kx - 7 * p, koy = ky - 7 * p;
    cells(s, kox, koy, p, 2, -1, 10, 16, R_DEEP, 240);
    cells(s, kox, koy, p, -1, 2, 16, 10, R_DEEP, 240);
    cells(s, kox, koy, p, 0, 0, 14, 14, R_DEEP, 240);
    cells(s, kox, koy, p, 3, 0, 8, 14, pressed ? R_FILLP : R_FILL, 255);
    cells(s, kox, koy, p, 0, 3, 14, 8, pressed ? R_FILLP : R_FILL, 255);
    cells(s, kox, koy, p, 1, 1, 12, 12, pressed ? R_FILLP : R_FILL, 255);
    cells(s, kox, koy, p, 3, 0, 8, 1, pressed ? R_DARK : R_LIGHT, 255);
    cells(s, kox, koy, p, 0, 3, 1, 8, pressed ? R_DARK : R_LIGHT, 255);
    cells(s, kox, koy, p, 3, 13, 8, 1, pressed ? R_LIGHT : R_DARK, 255);
    cells(s, kox, koy, p, 13, 3, 1, 8, pressed ? R_LIGHT : R_DARK, 255);
    uint32_t mark = pressed ? R_HI : R_BODY;                         /* the cross on the cap */
    cells(s, kox, koy, p, 6, 4, 2, 6, mark, 255);
    cells(s, kox, koy, p, 4, 6, 6, 2, mark, 255);
}

static void draw_control(const surface_t *s, int id, bool pressed, int knob_x, int knob_y,
                         int vx, int vy) {
    const control_t *k = &s_ctl[id];
    if (id == C_STICK) { draw_stick(s, k, pressed, knob_x, knob_y, vx, vy); return; }
    const int G = 24;
    int p = (2 * k->r) / G;
    if (p < 1) p = 1;
    int ox = k->x - G * p / 2, oy = k->y - G * p / 2;
    plate(s, ox, oy, p, G, pressed, 175, 220);
    const char *ic = icon_of(id);
    if (ic) {
        int shift = pressed ? 1 : 0;             /* the cap sinks when pushed */
        draw_icon(s, ic, ox + (4 + shift) * p, oy + (4 + shift) * p, p, pressed);
    }
}

/* Corner brackets round the control being edited. */
static void brackets(const surface_t *s, const control_t *k) {
    int h = k->r + s_unit / 6, t = s_unit / 8 > 2 ? s_unit / 8 : 2, l = h / 2;
    int x0 = k->x - h, y0 = k->y - h, x1 = k->x + h, y1 = k->y + h;
    rect(s, x0, y0, x0 + l, y0 + t, R_HI, 255);      rect(s, x0, y0, x0 + t, y0 + l, R_HI, 255);
    rect(s, x1 - l, y0, x1, y0 + t, R_HI, 255);      rect(s, x1 - t, y0, x1, y0 + l, R_HI, 255);
    rect(s, x0, y1 - t, x0 + l, y1, R_HI, 255);      rect(s, x0, y1 - l, x0 + t, y1, R_HI, 255);
    rect(s, x1 - l, y1 - t, x1, y1, R_HI, 255);      rect(s, x1 - t, y1 - l, x1, y1, R_HI, 255);
}

void touch_draw(uint32_t *dst, int surf_w, int surf_h, int pitch) {
    bool down[C_COUNT] = { false };
    int knob_x, knob_y, vx = 0, vy = 0;
    bool float_draw = false;

    lock();
    if (surf_w != s_w || surf_h != s_h) { unlock(); return; }
    surface_t s = { dst, surf_w, surf_h, pitch };
    bool live = controls_live();
    bool edit = s_edit;
    bool overlay = s_overlay_on && !s_gameplay && !edit;
    int stick_cx = s_ctl[C_STICK].x, stick_cy = s_ctl[C_STICK].y;
    knob_x = stick_cx;
    knob_y = stick_cy;
    for (int i = 0; i < MAX_TOUCHES; i++) {
        const touch_t *t = &s_touch[i];
        if (t->control < 0 || t->control >= C_COUNT) continue;
        down[t->control] = true;
        if (t->control == C_STICK && !edit) {
            int16_t sx, sy;
            int cx, cy, r = s_ctl[C_STICK].r;
            stick_origin(t, &cx, &cy);
            stick_value(t, &sx, &sy);
            vx = sx;
            vy = sy;
            if (t->ox) { float_draw = true; s_ctl[C_STICK].x = cx; s_ctl[C_STICK].y = cy; }
            /* The knob travels a third of the plate, like the engine's reach. */
            knob_x = cx + sx * (r * 55 / 100) / 32767;
            knob_y = cy - sy * (r * 55 / 100) / 32767;
        }
    }
    s_alpha = s_opacity * 255 / 100;

    if (live || edit) {
        if (edit) rect(&s, 0, 0, surf_w, surf_h, C_SHADOW, 150);
        for (int c = 0; c < C_COUNT; c++) {
            if (s_ctl[c].hidden) {
                if (!edit) continue;
                int save = s_alpha;
                s_alpha = save / 3;
                draw_control(&s, c, false, s_ctl[c].x, s_ctl[c].y, 0, 0);
                s_alpha = save;
            } else {
                draw_control(&s, c, down[c], knob_x, knob_y, vx, vy);
            }
        }
        if (edit) {
            int save = s_alpha;
            s_alpha = 255;
            const control_t *k = &s_ctl[s_sel];
            brackets(&s, k);
            static const char *names[T_COUNT] = { "-", "+", "HIDE", "RESET", "DONE" };
            int sc = s_unit / 14 > 1 ? s_unit / 14 : 1;
            for (int t = 0; t < T_COUNT; t++) {
                const char *nm = names[t];
                if (t == T_TOGGLE) nm = k->hidden ? "SHOW" : "HIDE";
                int bx = s_tool[t].x, by = s_tool[t].y, bw = s_tool[t].w, bh = s_tool[t].h;
                int cp = bh / 8 > 1 ? bh / 8 : 1;                     /* bevel cell */
                rect(&s, bx, by, bx + bw, by + bh, R_DEEP, 255);
                rect(&s, bx + cp, by + cp, bx + bw - cp, by + bh - cp, R_FILL, 255);
                rect(&s, bx + cp, by + cp, bx + bw - cp, by + 2 * cp, R_LIGHT, 255);
                rect(&s, bx + cp, by + cp, bx + 2 * cp, by + bh - cp, R_LIGHT, 255);
                rect(&s, bx + cp, by + bh - 2 * cp, bx + bw - cp, by + bh - cp, R_DARK, 255);
                rect(&s, bx + bw - 2 * cp, by + cp, bx + bw - cp, by + bh - cp, R_DARK, 255);
                text(&s, bx + bw / 2, by + bh / 2, nm, sc, R_HI, 255);
            }
            char buf[16];
            snprintf(buf, sizeof buf, "%d%%", s_ctl[s_sel].scale);
            text(&s, s_w / 2, s_tool[0].y + s_tool[0].h + s_unit / 2 + s_unit / 4, buf, sc, R_HI, 255);
            s_alpha = save;
        }
    }
    if (float_draw) { resolve_all(); }
    s_alpha = 255;
    if (overlay) draw_overlay(&s);
    unlock();
}
