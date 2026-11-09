// The in-game menu's Controls tab (docs/in-game-menu.md). Two pages, switched on the first row:
// - Keyboard: the key for each of the 21 actions the launcher's Controls tab has, in two columns, and Reset to defaults;
// - Controller: which connected controller is player 1 (Left / Right step through them; Enter, then any button on the controller wanted), the controller
//   button for each of the Switch's 16 buttons (one map for every controller), in two columns, and Reset to defaults (the layout the game starts with).
// Both are saved to the launcher's controls.json: the keys as numbers, the controllers' map as a "pad" object, player 1 as "player1" (SDL's GUID) and
// "player1_name" (front_map.h and front_sdl.c read them; play.ps1's Controls tab writes the same fields). The present thread draws the tab and takes the
// menu's input under the menu's lock; the window thread hands over a key or a controller button for the action waited for (ct_event, under the same lock).
// Our own code, MIT.
#include "menu_int.h"
#include "front_map.h"
#include "front_sdl.h"
#include "dm.h"
#include <stdio.h>
#include <string.h>

void front_sdl_keymap_get(front_keymap *m);
void front_sdl_keymap_set(const front_keymap *m);
void front_sdl_padmap_get(front_padmap *m, int *custom, char guid[40], char name[64]);
void front_sdl_padmap_set(const front_padmap *m, int custom);
const char *mn_settings_dir(void);

enum { CAP_NONE = -1, CAP_P1 = 100 };
#define CAP_NS 8000000000ull   // a capture nobody answers ends by itself
static struct {
    int page;                       // 0 the keyboard, 1 the controllers
    front_keymap keys;
    front_padmap pad; int pad_custom;   // pad_custom 0: no map chosen, the game's own layout (the default map shows it)
    char p1_guid[40], p1_name[64];
    int capture; uint64_t cap_t;    // the action whose key or button is awaited, CAP_P1 a button on the controller to be player 1, CAP_NONE none
    front_pad_info pads[16]; int npads;
} C = {.capture = CAP_NONE};

static const char *const c_label[FRONT_ACTS] = {"A", "B", "X", "Y", "Left stick press", "Right stick press", "L", "R", "ZL", "ZR", "+ (Plus)", "- (Minus)",
                                                "D-pad left", "D-pad up", "D-pad right", "D-pad down", "Stick up", "Stick left", "Stick down", "Stick right", "Shake",
                                                "Camera up", "Camera left", "Camera down", "Camera right"};
// The controller page's rows: the Switch's buttons, then the camera directions (a button can drive the camera; default none)
static const char *const pad_label[FRONT_PACTS] = {"A", "B", "X", "Y", "Left stick press", "Right stick press", "L", "R", "ZL", "ZR", "+ (Plus)", "- (Minus)",
                                                    "D-pad left", "D-pad up", "D-pad right", "D-pad down", "Camera up", "Camera left", "Camera down", "Camera right"};
static const char *ct_label(int i) { return C.page ? pad_label[i] : c_label[i]; }
// A controller input in words: by its place, and for the face buttons what player 1's controller prints there
static const char *const in_words[FRONT_INS] = {
    [SDL_GAMEPAD_BUTTON_SOUTH] = "Bottom", [SDL_GAMEPAD_BUTTON_EAST] = "Right", [SDL_GAMEPAD_BUTTON_WEST] = "Left", [SDL_GAMEPAD_BUTTON_NORTH] = "Top",
    [SDL_GAMEPAD_BUTTON_BACK] = "Back / Select", [SDL_GAMEPAD_BUTTON_GUIDE] = "Home", [SDL_GAMEPAD_BUTTON_START] = "Start / Options",
    [SDL_GAMEPAD_BUTTON_LEFT_STICK] = "Left stick press", [SDL_GAMEPAD_BUTTON_RIGHT_STICK] = "Right stick press",
    [SDL_GAMEPAD_BUTTON_LEFT_SHOULDER] = "LB / L1", [SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER] = "RB / R1",
    [SDL_GAMEPAD_BUTTON_DPAD_UP] = "D-pad up", [SDL_GAMEPAD_BUTTON_DPAD_DOWN] = "D-pad down", [SDL_GAMEPAD_BUTTON_DPAD_LEFT] = "D-pad left",
    [SDL_GAMEPAD_BUTTON_DPAD_RIGHT] = "D-pad right", [SDL_GAMEPAD_BUTTON_MISC1] = "Share / Capture", [SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1] = "Paddle R1",
    [SDL_GAMEPAD_BUTTON_LEFT_PADDLE1] = "Paddle L1", [SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2] = "Paddle R2", [SDL_GAMEPAD_BUTTON_LEFT_PADDLE2] = "Paddle L2",
    [SDL_GAMEPAD_BUTTON_TOUCHPAD] = "Touchpad", [SDL_GAMEPAD_BUTTON_MISC2] = "Extra 2", [SDL_GAMEPAD_BUTTON_MISC3] = "Extra 3", [SDL_GAMEPAD_BUTTON_MISC4] = "Extra 4",
    [SDL_GAMEPAD_BUTTON_MISC5] = "Extra 5", [SDL_GAMEPAD_BUTTON_MISC6] = "Extra 6", [FRONT_IN_LT] = "LT / L2", [FRONT_IN_RT] = "RT / R2"};
static int p1_type(void) { for (int i = 0; i < C.npads; i++) if (C.pads[i].player == 0) return C.pads[i].type; return SDL_GAMEPAD_TYPE_UNKNOWN; }
static void in_text(int in, char *out, size_t n) {
    static const char *const lab[9] = {"", "A", "B", "X", "Y", "Cross", "Circle", "Square", "Triangle"};
    if (in < 0 || in >= FRONT_INS || !in_words[in]) { snprintf(out, n, "(none)"); return; }
    int l = in <= SDL_GAMEPAD_BUTTON_NORTH && p1_type() != SDL_GAMEPAD_TYPE_UNKNOWN ? (int)SDL_GetGamepadButtonLabelForType((SDL_GamepadType)p1_type(), (SDL_GamepadButton)in) : 0;
    if (in <= SDL_GAMEPAD_BUTTON_NORTH && l > 0 && l < 9) snprintf(out, n, "%s (%s)", in_words[in], lab[l]);
    else if (in <= SDL_GAMEPAD_BUTTON_NORTH) snprintf(out, n, "%s face button", in_words[in]);
    else snprintf(out, n, "%s", in_words[in]);
}
static void key_name(SDL_Scancode sc, char *out, size_t n) { const char *k = sc ? SDL_GetScancodeName(sc) : ""; snprintf(out, n, "%s", k && *k ? k : "(none)"); }

// ---------- controls.json ----------
static void save(void) {
    char p[1100]; snprintf(p, sizeof p, "%s/controls.json", mn_settings_dir());
    SDL_CreateDirectory(mn_settings_dir());
    FILE *f = fopen(p, "w"); if (!f) { LOG("[menu] controls.json could not be written in %s", mn_settings_dir()); return; }
    // the launcher's names (play.ps1 $ControlsDefault) for front_map.h's actions, then the controllers' map (only when one was chosen) and player 1
    fprintf(f, "{\n    \"version\":  1,\n");
    for (int i = 0; i < FRONT_ACTS; i++) fprintf(f, "    \"%s\":  %d,\n", front_act_names[i], (int)C.keys.key[i]);
    if (C.pad_custom) { char j[900]; front_padmap_json(&C.pad, j, sizeof j); fprintf(f, "    %s,\n", j); }
    fprintf(f, "    \"player1\":  \"%s\",\n    \"player1_name\":  \"%s\"\n}\n", C.p1_guid, C.p1_name);
    fclose(f);
}

// ---------- The changes ----------
static void key_bind(int act, SDL_Scancode sc) {   // a key for an action; the action that had it takes this one's old key (the two swap)
    char t[120], kn[40]; SDL_Scancode old = C.keys.key[act];
    for (int i = 0; i < FRONT_ACTS; i++) if (i != act && C.keys.key[i] == sc) C.keys.key[i] = old;
    C.keys.key[act] = sc; front_sdl_keymap_set(&C.keys); save();
    key_name(sc, kn, sizeof kn); snprintf(t, sizeof t, "%s: %s", c_label[act], kn); mn_status(t); LOG("[menu] key %s", t);
}
static void key_reset(void) { front_keymap_default(&C.keys); front_sdl_keymap_set(&C.keys); save(); mn_status("Keyboard: back to the default keys"); LOG("[menu] the default keys"); }
static void pad_bind(int act, int in) {
    if (front_in_reserved(in)) { mn_status("Home belongs to the system: choose another button"); return; }
    char t[120], w[48];
    front_padmap_bind(&C.pad, act, in); C.pad_custom = 1; front_sdl_padmap_set(&C.pad, 1); save();
    in_text(in, w, sizeof w); snprintf(t, sizeof t, "%s: %s", c_label[act], w); mn_status(t); LOG("[menu] controller button %s (%s)", t, front_in_names[in]);
}
static void pad_reset(void) {
    front_padmap_default(&C.pad); C.pad_custom = 0; front_sdl_padmap_set(&C.pad, 0); save();
    mn_status("Controller: back to the game's own layout"); LOG("[menu] the controllers' own layout");
}
static void p1_choose(const front_pad_info *p) {
    char t[140];
    snprintf(C.p1_guid, sizeof C.p1_guid, "%s", p->guid); front_name_clean(p->name, C.p1_name, sizeof C.p1_name);
    front_sdl_player1(p->id); save();
    snprintf(t, sizeof t, "Player 1: %s", p->name); mn_status(t); LOG("[menu] player 1 is '%s' (%s)", p->name, p->guid);
}
static void p1_step(int d) {   // the next connected controller after player 1's, or the one before
    if (!C.npads) { mn_status("No controller is connected"); return; }
    int cur = -1; for (int i = 0; i < C.npads; i++) if (C.pads[i].player == 0) cur = i;
    int k = cur < 0 ? 0 : (cur + d + C.npads) % C.npads;
    if (k == cur) { mn_status("This is the only controller connected"); return; }
    p1_choose(&C.pads[k]);
}
static void capture(int what) { C.capture = what; C.cap_t = SDL_GetTicksNS(); mn_status(C.page ? "Press a button on the controller (Esc: keep it)" : "Press a key (Esc: keep it)"); }

void ct_init(void) {
    front_sdl_keymap_get(&C.keys);
    front_sdl_padmap_get(&C.pad, &C.pad_custom, C.p1_guid, C.p1_name);
}
void ct_close(void) { C.capture = CAP_NONE; }
int ct_capturing(void) { return C.capture != CAP_NONE; }

// ---------- The rows: 0 the page switch; keyboard 1..21 the actions, 22 Reset; controller 1 player 1, 2..17 the buttons, 18 Reset ----------
static int nitems(void) { return (C.page ? FRONT_PACTS : FRONT_ACTS) + 1; }   // the grid's: the actions and Reset
static int grid0(void) { return C.page ? 2 : 1; }
static int lines(void) { return (nitems() + 1) / 2; }
int ct_rows(void) { return grid0() + nitems(); }
typedef struct { float row_h, colw, p1y, gy, pill; } lay;
static void layout(const mn_panel *p, lay *L) {
    L->row_h = 46 * p->u; L->colw = (p->pw - 96 * p->u - 32 * p->u) / 2; L->pill = 220 * p->u;
    L->p1y = p->y0 + 58 * p->u; L->gy = C.page ? L->p1y + 58 * p->u : p->y0 + 58 * p->u;
    float avail = p->py + p->ph - 150 * p->u - L->gy;   // from the grid's first row to above the hint text
    if (lines() * L->row_h > avail) L->row_h = avail / lines() < 24 * p->u ? 24 * p->u : avail / lines();   // the camera rows made the grid long: compress the rows to fit
}
static void cell(const mn_panel *p, const lay *L, int i, float *x, float *y) { *x = p->px + 48 * p->u + (i / lines()) * (L->colw + 32 * p->u); *y = L->gy + (i % lines()) * L->row_h; }
// The row under the mouse (and for the page switch which page), -1 none
static int at(const mn_panel *p, float x, float y, int *page) {
    lay L; layout(p, &L);
    for (int k = 0; k < 2; k++) if (mn_inside(x, y, p->px + 48 * p->u + k * (L.pill + 12 * p->u), p->y0, L.pill, L.row_h)) { if (page) *page = k; return 0; }
    if (C.page && mn_inside(x, y, p->px + 24 * p->u, L.p1y, p->pw - 48 * p->u, L.row_h)) return 1;
    for (int i = 0; i < nitems(); i++) { float cx, cy; cell(p, &L, i, &cx, &cy); if (mn_inside(x, y, cx, cy, L.colw, L.row_h)) return grid0() + i; }
    return -1;
}
int ct_pick(const mn_panel *p, float x, float y) { return at(p, x, y, NULL); }

static void enter(int sel) {   // Enter, A or a click on a row
    if (sel == 0) { C.page = !C.page; return; }
    if (C.page && sel == 1) { if (!C.npads) mn_status("No controller is connected"); else capture(CAP_P1); return; }
    int i = sel - grid0();
    if (i == nitems() - 1) { if (C.page) pad_reset(); else key_reset(); return; }
    capture(i);
}
void ct_input(const mn_panel *p, int kind, float x, float y, int *sel) {
    C.npads = front_sdl_pads(C.pads, 16);
    if (C.capture != CAP_NONE) return;   // the key or button comes through ct_event
    int s = *sel, L = lines(), n = nitems(), g0 = grid0();
    if (kind == IN_CLICK) {
        int pg = 0, r = at(p, x, y, &pg); if (r < 0) return;
        *sel = r;
        if (r == 0) { if (pg != C.page) { C.page = pg; *sel = 0; } return; }
        enter(r); return;
    }
    if (kind == IN_OK) { int was = C.page; enter(s); if (C.page != was) *sel = 0; return; }
    if (s == 0) {   // the page switch: left / right pick the page, down goes into it
        if (kind == IN_LEFT || kind == IN_RIGHT) C.page = kind == IN_RIGHT;
        else if (kind == IN_DOWN) *sel = 1;
        return;
    }
    if (C.page && s == 1) {   // player 1: left / right step through the controllers
        if (kind == IN_LEFT || kind == IN_RIGHT) p1_step(kind == IN_RIGHT ? 1 : -1);
        else if (kind == IN_UP) *sel = 0; else if (kind == IN_DOWN) *sel = g0;
        return;
    }
    int i = s - g0;
    if (kind == IN_UP) *sel = i % L ? s - 1 : g0 - 1;
    else if (kind == IN_DOWN) { if (i % L + 1 < L && i + 1 < n) *sel = s + 1; }
    else if (kind == IN_LEFT || kind == IN_RIGHT) { int j = kind == IN_LEFT ? i - L : i + L; if (j >= 0 && j < n) *sel = g0 + j; }
}
// The window thread, the menu open and its lock held: the key or button for the one awaited; 1 when taken
int ct_event(const SDL_Event *e) {
    if (C.capture == CAP_NONE) return 0;
    if (e->type == SDL_EVENT_KEY_DOWN) {
        SDL_Scancode sc = e->key.scancode;
        if (e->key.repeat) return 1;
        if (sc == SDL_SCANCODE_ESCAPE) { C.capture = CAP_NONE; mn_status("Not changed"); return 1; }
        if (C.page) { mn_status("Press a button on the controller (Esc: keep it)"); return 1; }
        if (front_key_reserved(sc) || sc == SDL_SCANCODE_F1 || sc == SDL_SCANCODE_LALT || sc == SDL_SCANCODE_RALT || sc == SDL_SCANCODE_LGUI || sc == SDL_SCANCODE_RGUI)
            mn_status("That key belongs to the window or to Windows: choose another");
        else { key_bind(C.capture, sc); C.capture = CAP_NONE; }
        return 1;
    }
    if (!C.page) return e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;   // a key is awaited: a controller does not choose it
    if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || (e->type == SDL_EVENT_GAMEPAD_AXIS_MOTION && e->gaxis.value > 16384 &&
                                                     (e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || e->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER))) {
        int in = e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ? e->gbutton.button : e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? FRONT_IN_LT : FRONT_IN_RT;
        uint32_t which = e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ? e->gbutton.which : e->gaxis.which;
        if (C.capture == CAP_P1) {
            C.npads = front_sdl_pads(C.pads, 16);
            for (int i = 0; i < C.npads; i++) if (C.pads[i].id == which) p1_choose(&C.pads[i]);
        } else pad_bind(C.capture, in);
        if (!front_in_reserved(in) || C.capture == CAP_P1) C.capture = CAP_NONE;
        return 1;
    }
    return e->type == SDL_EVENT_GAMEPAD_BUTTON_UP || e->type == SDL_EVENT_GAMEPAD_AXIS_MOTION;   // nothing else of the controllers while one is awaited
}

// ---------- Drawing ----------
void ct_draw(mn_list *out, const mn_panel *p, int sel) {
    C.npads = front_sdl_pads(C.pads, 16);
    if (C.capture != CAP_NONE && SDL_GetTicksNS() - C.cap_t > CAP_NS) { C.capture = CAP_NONE; mn_status("Not changed"); }
    lay L; layout(p, &L);
    float u = p->u, fs = 22 * u; char v[160];
    static const char *const pages[2] = {"Keyboard", "Controller"};
    for (int k = 0; k < 2; k++) {
        float x = p->px + 48 * u + k * (L.pill + 12 * u);
        mn_rect(out, x, p->y0 + 4 * u, L.pill, L.row_h - 8 * u, (L.row_h - 8 * u) / 2, C.page == k ? (sel == 0 ? C_SEL : RGBA(255, 236, 150, 255)) : sel == 0 ? C_PILL : C_TAB);
        mn_text(out, pages[k], x + (L.pill - mn_text_w(pages[k], fs)) / 2, p->y0 + (L.row_h - fs) / 2, fs, C.page == k ? C_SELTEXT : C_TEXT);
    }
    if (C.page) {   // player 1
        float y = L.p1y, vw = p->pw * 0.55f, vx = p->px + p->pw - 48 * u - vw;
        if (sel == 1) mn_rect(out, p->px + 24 * u, y + 3 * u, p->pw - 48 * u, L.row_h - 6 * u, (L.row_h - 6 * u) / 2, RGBA(255, 244, 196, 255));
        mn_text(out, "Player 1", p->px + 48 * u, y + (L.row_h - fs) / 2, fs, C_TEXT);
        const char *who = NULL; for (int i = 0; i < C.npads; i++) if (C.pads[i].player == 0) who = C.pads[i].name;
        if (C.capture == CAP_P1) snprintf(v, sizeof v, "press any button on it...");
        else if (who) snprintf(v, sizeof v, "%s%s", who, C.npads > 1 ? "   < >" : "");
        else snprintf(v, sizeof v, "%s", C.npads ? "the keyboard (no controller is player 1)" : "no controller connected");
        mn_rect(out, vx, y + 7 * u, vw, L.row_h - 14 * u, (L.row_h - 14 * u) / 2, C.capture == CAP_P1 ? C_EDGE : sel == 1 ? C_SEL : C_PILL);
        float vs = mn_fit(v, fs, vw - 32 * u, u);
        mn_text(out, v, vx + (vw - mn_text_w(v, vs)) / 2, y + (L.row_h - vs) / 2, vs, sel == 1 ? C_SELTEXT : C_TEXT);
    }
    for (int i = 0; i < nitems(); i++) {
        float x, y; cell(p, &L, i, &x, &y); int r = grid0() + i, on = r == sel;
        if (i == nitems() - 1) {
            mn_rect(out, x, y + 4 * u, L.colw, L.row_h - 8 * u, (L.row_h - 8 * u) / 2, on ? C_SEL : C_TAB);
            mn_text(out, "Reset to defaults", x + (L.colw - mn_text_w("Reset to defaults", fs)) / 2, y + (L.row_h - fs) / 2, fs, on ? C_SELTEXT : C_TEXT);
            continue;
        }
        if (on) mn_rect(out, x - 8 * u, y + 3 * u, L.colw + 16 * u, L.row_h - 6 * u, (L.row_h - 6 * u) / 2, RGBA(255, 244, 196, 255));
        mn_text(out, ct_label(i), x + 12 * u, y + (L.row_h - fs) / 2, fs, C_TEXT);
        float kw = L.colw * 0.5f, kx = x + L.colw - kw; int waiting = C.capture == i;
        mn_rect(out, kx, y + 7 * u, kw, L.row_h - 14 * u, (L.row_h - 14 * u) / 2, waiting ? C_EDGE : on ? C_SEL : C_PILL);
        if (waiting) snprintf(v, sizeof v, "%s", C.page ? "press a button..." : "press a key...");
        else if (C.page) in_text(C.pad.in[i], v, sizeof v); else key_name(C.keys.key[i], v, sizeof v);
        float vs = mn_fit(v, fs, kw - 20 * u, u);
        mn_text(out, v, kx + (kw - mn_text_w(v, vs)) / 2, y + (L.row_h - vs) / 2, vs, on ? C_SELTEXT : C_TEXT);
    }
}
const char *ct_hint(int sel) {
    if (C.capture == CAP_P1) return "Press any button on the controller that should be player 1. Esc keeps it as it is.";
    if (C.capture != CAP_NONE) return C.page ? "Press the controller button for it (a trigger counts too). Esc keeps the old one." : "Press the new key. Esc keeps the old one.";
    if (sel == 0) return "Left / right: the keyboard's keys or the controllers' buttons.";
    if (C.page && sel == 1) return "Which controller is player 1, kept for next time: left / right to step through them, or Enter and a button on the one you want.";
    if (sel == grid0() + nitems() - 1) return C.page ? "The buttons the game starts with (face buttons by their place, as on a Switch)." : "The keys the game starts with.";
    return C.page ? "Choose a Switch button, then press the controller button for it. One map for every controller; the two swap if it was taken."
                  : "Choose an action, then press the key for it. The two swap if the key was taken.";
}

// ---------- DM_CTL "menu ..." (tests): reset, key <action> <scancode>, ct page <0|1>, ct pad <action> <input name>, ct padreset, ct p1 <controller index>,
// ct capture <action, -1 player 1> (the next key or controller button answers it) ----------
void ct_ctl(const char *cmd) {
    int a = -1, b = -1; char nm[32];
    C.npads = front_sdl_pads(C.pads, 16);
    if (!strncmp(cmd, "reset", 5)) key_reset();
    else if (sscanf(cmd, "key %d %d", &a, &b) == 2 && a >= 0 && a < FRONT_ACTS && !front_key_reserved(b)) key_bind(a, (SDL_Scancode)b);
    else if (sscanf(cmd, "ct page %d", &a) == 1) C.page = a ? 1 : 0;
    else if (sscanf(cmd, "ct pad %d %31s", &a, nm) == 2 && a >= 0 && a < FRONT_PACTS) { int in = front_in_by_name(nm, strlen(nm)); if (in >= 0) pad_bind(a, in); }
    else if (!strncmp(cmd, "ct padreset", 11)) pad_reset();
    else if (sscanf(cmd, "ct p1 %d", &a) == 1 && a >= 0 && a < C.npads) p1_choose(&C.pads[a]);
    else if (sscanf(cmd, "ct capture %d", &a) == 1 && a >= -1 && a < (C.page ? FRONT_PACTS : FRONT_ACTS)) capture(a < 0 ? CAP_P1 : a);   // then a key or a button
}
