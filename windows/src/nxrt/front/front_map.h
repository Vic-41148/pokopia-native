// The front end's input mapping (front_sdl.c), pure so tests/test_input.c checks it without a window or a controller. Written from SDL3's public headers
// (scancodes, gamepad buttons, labels and axes; zlib licence) and nxnative's Mac front end (dm/host/front_mac.m, MIT, © seedds) for the layout:
//   keyboard (Nintendo layout, by key position): WASD left stick, arrow keys D-pad, J=A K=B U=X I=Y, Q=L E=R, Z=ZL C=ZR, F=press left stick G=press right stick,
//   Return=+ Tab=−, Space=shake the controller (front_mac.m key_bit; SDL scancodes are positions, as the Mac's key codes are);
//   controllers: face buttons by position as on a Switch (right A, bottom B, top X, left Y: SDL3 names them by position, so a Nintendo pad maps name for name, as
//   pad_apple.h has it), or by printed label with DM_PAD_FACE=labels (an Xbox pad's A is then A); shoulders L R, triggers ZL ZR, Start + Back −, both sticks and
//   their presses, the D-pad. Guide (Home) and Misc1 (Capture) have no function, as on the Mac.
// Sticks: SDL's y grows downward, the Switch's upward. A radial inner dead zone (a resting stick's drift) and an outer one (research/01 #18: a DualSense tops
// out near 0.96, so a full push never reached the game): magnitudes below inner are 0, at or above outer full tilt, linear between, the direction kept.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_gamepad.h>
#include "dm_input.h"

#define FRONT_PAD_INNER 0.08f        // DM_PAD_INNER
#define FRONT_PAD_OUTER 0.92f        // DM_PAD_OUTER
#define FRONT_TRIGGER_PRESS 8192     // a trigger counts as pressed past a quarter of its travel
#define FRONT_LOOK_SENS 0.05f        // degrees per mouse count (raw input, no pointer acceleration); DM_LOOK_SENS / DM_LOOK_SCALE (hle_hid.c dm_look_sens)

// The keyboard map: one key position per action. Actions 0-15 are the NpadButtons in their bit order, then the left stick's four directions and the shake.
// DM_KEYMAP=<file> (play.ps1's Controls tab writes it) replaces actions by name, "A": <SDL scancode>, ...; the layout above is the default
enum { FRONT_ACT_STICK_UP = 16, FRONT_ACT_STICK_LEFT, FRONT_ACT_STICK_DOWN, FRONT_ACT_STICK_RIGHT, FRONT_ACT_SHAKE, FRONT_ACT_CAM_UP, FRONT_ACT_CAM_LEFT, FRONT_ACT_CAM_DOWN, FRONT_ACT_CAM_RIGHT, FRONT_ACTS };
static const char *const front_act_names[FRONT_ACTS] = {"A", "B", "X", "Y", "LS", "RS", "L", "R", "ZL", "ZR", "Plus", "Minus", "Left", "Up", "Right", "Down",
                                                          "StickUp", "StickLeft", "StickDown", "StickRight", "Shake", "CamUp", "CamLeft", "CamDown", "CamRight"};
typedef struct { SDL_Scancode key[FRONT_ACTS]; } front_keymap;
static inline void front_keymap_default(front_keymap *m) {
    static const SDL_Scancode d[FRONT_ACTS] = {
        SDL_SCANCODE_J, SDL_SCANCODE_K, SDL_SCANCODE_U, SDL_SCANCODE_I, SDL_SCANCODE_F, SDL_SCANCODE_G, SDL_SCANCODE_Q, SDL_SCANCODE_E, SDL_SCANCODE_Z, SDL_SCANCODE_C,
        SDL_SCANCODE_RETURN, SDL_SCANCODE_TAB, SDL_SCANCODE_LEFT, SDL_SCANCODE_UP, SDL_SCANCODE_RIGHT, SDL_SCANCODE_DOWN,
        SDL_SCANCODE_W, SDL_SCANCODE_A, SDL_SCANCODE_S, SDL_SCANCODE_D, SDL_SCANCODE_SPACE,
        SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN};
    for (int i = 0; i < FRONT_ACTS; i++) m->key[i] = d[i];
}
// Keys a map may not take: Esc releases the mouse, F11 is fullscreen; anything outside the keyboard's usage codes
static inline int front_key_reserved(long sc) { return sc < SDL_SCANCODE_A || sc > SDL_SCANCODE_RGUI || sc == SDL_SCANCODE_ESCAPE || sc == SDL_SCANCODE_F11; }
// A map file's text (JSON written by play.ps1): every "<action>": <number> pair names an action's key; other fields (strings, the version) are skipped. Starts from
// the default; a reserved key, or a key an earlier action already asked for, is refused. An action not asked for keeps its default key unless an asked one took it
// (then it has none). Returns the actions set; *refused counts the refusals
static inline int front_keymap_parse(front_keymap *m, const char *t, int *refused) {
    front_keymap_default(m);
    SDL_Scancode want[FRONT_ACTS]; for (int i = 0; i < FRONT_ACTS; i++) want[i] = SDL_SCANCODE_UNKNOWN;
    int set = 0, bad = 0;
    for (const char *p = t ? strchr(t, '"') : NULL; p; p = strchr(p, '"')) {
        const char *q = strchr(p + 1, '"'); if (!q) break;
        size_t n = (size_t)(q - p - 1); const char *r = q + 1;
        while (*r == ' ' || *r == '\t' || *r == '\r' || *r == '\n') r++;
        if (*r != ':') { p = q + 1; continue; }   // a string value, not a name
        r++; while (*r == ' ' || *r == '\t' || *r == '\r' || *r == '\n') r++;
        if (*r >= '0' && *r <= '9') {
            long v = strtol(r, NULL, 10);
            for (int i = 0; i < FRONT_ACTS; i++)
                if (strlen(front_act_names[i]) == n && !strncmp(p + 1, front_act_names[i], n)) { if (front_key_reserved(v)) bad++; else want[i] = (SDL_Scancode)v; }
        }
        p = r;
    }
    for (int i = 0; i < FRONT_ACTS; i++) {   // a key asked twice goes to the first action
        if (want[i] == SDL_SCANCODE_UNKNOWN) continue;
        int dup = 0; for (int j = 0; j < i; j++) dup |= want[j] == want[i];
        if (dup) { bad++; want[i] = SDL_SCANCODE_UNKNOWN; } else { m->key[i] = want[i]; set++; }
    }
    for (int i = 0; i < FRONT_ACTS; i++) {
        if (want[i] != SDL_SCANCODE_UNKNOWN) continue;
        for (int j = 0; j < FRONT_ACTS; j++) if (want[j] == m->key[i]) { m->key[i] = SDL_SCANCODE_UNKNOWN; break; }
    }
    if (refused) *refused = bad;
    return set;
}
// The action a key does under a map; -1 for none
static inline int front_keymap_action(const front_keymap *m, SDL_Scancode sc) {
    if (sc == SDL_SCANCODE_UNKNOWN) return -1;
    for (int i = 0; i < FRONT_ACTS; i++) if (m->key[i] == sc) return i;
    return -1;
}
static inline const front_keymap *front_keymap_builtin(void) {
    static front_keymap m; static int init;
    if (!init) { front_keymap_default(&m); init = 1; }
    return &m;
}

// The keyboard: buttons held and the four left-stick keys and the four camera keys
typedef struct { uint64_t btn; int w, a, s, d, cw, ca, cs, cd; } front_kbd;
// The NpadButton (or DM_BTN_SHAKE) a key presses under a map; 0 for none (a stick or camera key, or no action)
static inline uint64_t front_keymap_button(const front_keymap *m, SDL_Scancode sc) {
    int a = front_keymap_action(m, sc);
    return a < 0 || (a >= FRONT_ACT_STICK_UP && a < FRONT_ACT_SHAKE) || a >= FRONT_ACT_CAM_UP ? 0 : a == FRONT_ACT_SHAKE ? DM_BTN_SHAKE : 1ull << a;
}
static inline uint64_t front_key_button(SDL_Scancode sc) { return front_keymap_button(front_keymap_builtin(), sc); }
// A key event; returns 1 when the keyboard's state changed (it is a key the game takes)
static inline int front_kbd_key_map(front_kbd *k, const front_keymap *m, SDL_Scancode sc, int down) {
    int a = front_keymap_action(m, sc);
    int *stick = a == FRONT_ACT_STICK_UP ? &k->w : a == FRONT_ACT_STICK_LEFT ? &k->a : a == FRONT_ACT_STICK_DOWN ? &k->s : a == FRONT_ACT_STICK_RIGHT ? &k->d
               : a == FRONT_ACT_CAM_UP ? &k->cw : a == FRONT_ACT_CAM_LEFT ? &k->ca : a == FRONT_ACT_CAM_DOWN ? &k->cs : a == FRONT_ACT_CAM_RIGHT ? &k->cd : NULL;
    if (stick) { if (*stick == !!down) return 0; *stick = !!down; return 1; }
    uint64_t mk = front_keymap_button(m, sc); if (!mk) return 0;
    uint64_t b = down ? k->btn | mk : k->btn & ~mk; if (b == k->btn) return 0;
    k->btn = b; return 1;
}
static inline int front_kbd_key(front_kbd *k, SDL_Scancode sc, int down) { return front_kbd_key_map(k, front_keymap_builtin(), sc, down); }
static inline void front_kbd_axes(const front_kbd *k, int32_t *lx, int32_t *ly) { *lx = (k->d - k->a) * DM_STICK_MAX; *ly = (k->w - k->s) * DM_STICK_MAX; }
// The camera keys as the right stick, the Switch's way (up is positive y)
static inline void front_kbd_raxes(const front_kbd *k, int32_t *rx, int32_t *ry) { *rx = (k->cd - k->ca) * DM_STICK_MAX; *ry = (k->cw - k->cs) * DM_STICK_MAX; }

// A controller button as NpadButton; by_label: DM_PAD_FACE=labels, label = SDL_GetGamepadButtonLabel's answer for a face button
static inline uint64_t front_pad_button(SDL_GamepadButton b, int by_label, SDL_GamepadButtonLabel label) {
    if (by_label && b <= SDL_GAMEPAD_BUTTON_NORTH) switch (label) {
        case SDL_GAMEPAD_BUTTON_LABEL_A: return DM_BTN_A; case SDL_GAMEPAD_BUTTON_LABEL_B: return DM_BTN_B;
        case SDL_GAMEPAD_BUTTON_LABEL_X: return DM_BTN_X; case SDL_GAMEPAD_BUTTON_LABEL_Y: return DM_BTN_Y;
        default: break;   // shapes (PlayStation): by position
    }
    switch (b) {
    case SDL_GAMEPAD_BUTTON_EAST: return DM_BTN_A;   case SDL_GAMEPAD_BUTTON_SOUTH: return DM_BTN_B;
    case SDL_GAMEPAD_BUTTON_NORTH: return DM_BTN_X;  case SDL_GAMEPAD_BUTTON_WEST: return DM_BTN_Y;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return DM_BTN_L;  case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return DM_BTN_R;
    case SDL_GAMEPAD_BUTTON_START: return DM_BTN_PLUS;       case SDL_GAMEPAD_BUTTON_BACK: return DM_BTN_MINUS;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return DM_BTN_LS;    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return DM_BTN_RS;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return DM_BTN_LEFT;   case SDL_GAMEPAD_BUTTON_DPAD_UP: return DM_BTN_UP;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return DM_BTN_RIGHT; case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return DM_BTN_DOWN;
    default: return 0;
    }
}
// One stick, SDL's axis values (-32768..32767, y down) to the Switch's (±DM_STICK_MAX, y up), through the radial dead zones
static inline void front_stick(int32_t x, int32_t y, float inner, float outer, int32_t *ox, int32_t *oy) {
    double fx = x / 32767.0, fy = -y / 32767.0, m = sqrt(fx * fx + fy * fy);
    if (!(m > inner) || m <= 0) { *ox = *oy = 0; return; }
    double t = outer > inner ? (m - inner) / (outer - inner) : 1; if (t > 1) t = 1;
    double vx = fx * t / m, vy = fy * t / m;
    *ox = (int32_t)lrint(fmax(-1, fmin(1, vx)) * DM_STICK_MAX); *oy = (int32_t)lrint(fmax(-1, fmin(1, vy)) * DM_STICK_MAX);
}
static inline int front_trigger(int32_t v) { return v > FRONT_TRIGGER_PRESS; }
// A pointer position (window pixels) to the touch screen's normalized point on the centred 16:9 picture (front_mac.m mouseDragged; origin top left)
static inline void front_touch_point(double px, double py, double w, double h, float *tx, float *ty) {
    double pw = w, ph = h;
    if (pw <= 0 || ph <= 0) { *tx = *ty = 0; return; }
    if (pw / ph > 16.0 / 9.0) pw = ph * 16.0 / 9.0; else ph = pw * 9.0 / 16.0;
    double x0 = (w - pw) / 2, y0 = (h - ph) / 2;
    *tx = (float)fmin(fmax((px - x0) / pw, 0), 0.999); *ty = (float)fmin(fmax((py - y0) / ph, 0), 0.999);
}

// Rumble (M6 phase B): a player's two actuators (hle_haptics.c: left and right, each a low and a high band) as SDL_RumbleGamepad's strong (low-frequency) and weak
// (high-frequency) motors, mixed as nxnative's DualShock 4 route feeds SDL's hidapi driver (haptics_apple.m ds4_apply): the strong motor follows the low band, mostly
// from the left actuator, the weak one the high band, mostly from the right. gain scales (DM_RUMBLE_GAIN, 0-3); levels at or below min are silent and the rest is
// stretched over the full range (DM_RUMBLE_MIN, 0-0.9: haptics_apple.m controller_level)
static inline float front_rumble_level(float x, float gain, float min) {
    x = isfinite(x) && isfinite(gain) ? x * gain : 0; x = x < 0 ? 0 : x > 1 ? 1 : x;
    return x <= min ? 0 : (x - min) / (1 - min);
}
static inline void front_rumble_mix(float l_low, float l_high, float r_low, float r_high, float gain, float min, uint16_t *strong, uint16_t *weak) {
    *strong = (uint16_t)lrintf(front_rumble_level(0.85f * l_low + 0.15f * r_low, gain, min) * 65535.0f);
    *weak = (uint16_t)lrintf(front_rumble_level(0.15f * l_high + 0.85f * r_high, gain, min) * 65535.0f);
}

// Motion (M6 phase B): SDL's gamepad sensors in the Switch's frame for dm_motion_set_hid. SDL (SDL_sensor.h), a controller held in front of you: x right, y up out of
// its face, z toward the player; acceleration in m/s² as the specific force (+g along y lying flat), rotation in rad/s, counter-clockwise positive. The Switch
// (motion_fusion.h): x right, y away from the player, z up out of the face; acceleration in G as the gravity direction ((0, 0, -1) lying flat), rad/s. The axis map
// (x, -z, y) is a proper rotation, so the rates map as the vectors do
#define FRONT_STANDARD_GRAVITY 9.80665f
static inline void front_motion_frame(const float sdl_acc[3], const float sdl_gyro[3], float acc[3], float rot[3]) {
    acc[0] = -sdl_acc[0] / FRONT_STANDARD_GRAVITY; acc[1] = sdl_acc[2] / FRONT_STANDARD_GRAVITY; acc[2] = -sdl_acc[1] / FRONT_STANDARD_GRAVITY;
    rot[0] = sdl_gyro[0]; rot[1] = -sdl_gyro[2]; rot[2] = sdl_gyro[1];
}
// The Switch button names of an NpadButton mask, for the controller check's log (DM_PAD_CHECK)
static inline int front_button_names(uint64_t m, char *out, int cap) {
    static const char *nm[] = {"A", "B", "X", "Y", "LS", "RS", "L", "R", "ZL", "ZR", "+", "-", "Left", "Up", "Right", "Down"};
    int o = 0; out[0] = 0;
    for (int i = 0; i < 16 && o < cap - 8; i++) if (m >> i & 1) o += snprintf(out + o, (size_t)(cap - o), "%s%s", o ? " " : "", nm[i]);
    return o;
}

// ---------- The controller map: controls.json's "pad" object (the F1 Controls tab's Controller page, play.ps1's Controls tab) ----------
// One controller input per Switch button (actions 0-15, as the keyboard's), shared by every controller. An input is one of SDL's gamepad buttons, named by
// its place (south = the bottom face button, whatever is printed on it), or a trigger. Without a "pad" object the front end keeps the layout above
// (front_pad_button, DM_PAD_FACE); the default map here is that same layout by place
#define FRONT_PACTS 20
enum { FRONT_PAD_CAM_UP = 16, FRONT_PAD_CAM_LEFT, FRONT_PAD_CAM_DOWN, FRONT_PAD_CAM_RIGHT };   // the map's actions 16-19: the camera, on the right stick's directions
enum { FRONT_IN_LT = SDL_GAMEPAD_BUTTON_COUNT, FRONT_IN_RT, FRONT_INS };   // the triggers, after SDL's buttons; -1 none
typedef struct { int8_t in[FRONT_PACTS]; } front_padmap;
// The controller map's action names (A..Down as the Switch buttons, then the camera); the keyboard's list covers the sticks separately
static const char *const front_pad_act_names[FRONT_PACTS] = {"A", "B", "X", "Y", "LS", "RS", "L", "R", "ZL", "ZR", "Plus", "Minus", "Left", "Up", "Right", "Down",
                                                              "CamUp", "CamLeft", "CamDown", "CamRight"};
static const char *const front_in_names[FRONT_INS] = {
    [SDL_GAMEPAD_BUTTON_SOUTH] = "south", [SDL_GAMEPAD_BUTTON_EAST] = "east", [SDL_GAMEPAD_BUTTON_WEST] = "west", [SDL_GAMEPAD_BUTTON_NORTH] = "north",
    [SDL_GAMEPAD_BUTTON_BACK] = "back", [SDL_GAMEPAD_BUTTON_GUIDE] = "guide", [SDL_GAMEPAD_BUTTON_START] = "start",
    [SDL_GAMEPAD_BUTTON_LEFT_STICK] = "leftstick", [SDL_GAMEPAD_BUTTON_RIGHT_STICK] = "rightstick",
    [SDL_GAMEPAD_BUTTON_LEFT_SHOULDER] = "leftshoulder", [SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER] = "rightshoulder",
    [SDL_GAMEPAD_BUTTON_DPAD_UP] = "dpup", [SDL_GAMEPAD_BUTTON_DPAD_DOWN] = "dpdown", [SDL_GAMEPAD_BUTTON_DPAD_LEFT] = "dpleft", [SDL_GAMEPAD_BUTTON_DPAD_RIGHT] = "dpright",
    [SDL_GAMEPAD_BUTTON_MISC1] = "misc1", [SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1] = "rightpaddle1", [SDL_GAMEPAD_BUTTON_LEFT_PADDLE1] = "leftpaddle1",
    [SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2] = "rightpaddle2", [SDL_GAMEPAD_BUTTON_LEFT_PADDLE2] = "leftpaddle2", [SDL_GAMEPAD_BUTTON_TOUCHPAD] = "touchpad",
    [SDL_GAMEPAD_BUTTON_MISC2] = "misc2", [SDL_GAMEPAD_BUTTON_MISC3] = "misc3", [SDL_GAMEPAD_BUTTON_MISC4] = "misc4", [SDL_GAMEPAD_BUTTON_MISC5] = "misc5",
    [SDL_GAMEPAD_BUTTON_MISC6] = "misc6", [FRONT_IN_LT] = "lefttrigger", [FRONT_IN_RT] = "righttrigger"};
static inline void front_padmap_default(front_padmap *m) {
    static const int8_t d[FRONT_PACTS] = {SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_NORTH, SDL_GAMEPAD_BUTTON_WEST,
        SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, FRONT_IN_LT, FRONT_IN_RT,
        SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_DOWN,
        -1, -1, -1, -1};
    for (int i = 0; i < FRONT_PACTS; i++) m->in[i] = d[i];
}
// Inputs a map may not take: Guide (Home) belongs to the system (Steam, the Xbox Game Bar), as on the Switch
static inline int front_in_reserved(int in) { return in < 0 || in >= FRONT_INS || in == SDL_GAMEPAD_BUTTON_GUIDE; }
static inline int front_in_by_name(const char *s, size_t n) {
    for (int i = 0; i < FRONT_INS; i++) if (front_in_names[i] && strlen(front_in_names[i]) == n && !strncmp(s, front_in_names[i], n)) return i;
    return -1;
}
// After "key" and its colon: the value's first character, or NULL
static inline const char *front_json_value(const char *t, const char *key) {
    char k[40]; snprintf(k, sizeof k, "\"%s\"", key);
    const char *p = t ? strstr(t, k) : NULL; if (!p) return NULL;
    p += strlen(k); while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != ':') return NULL;
    p++; while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}
// A controller's name as controls.json keeps it: quotes, backslashes and control characters become '_' (front_json_str reads no escapes)
static inline void front_name_clean(const char *in, char *out, size_t cap) {
    size_t i = 0; for (; in && in[i] && i + 1 < cap; i++) out[i] = in[i] == '"' || in[i] == '\\' || (unsigned char)in[i] < 32 ? '_' : in[i];
    if (cap) out[i] = 0;
}
// A string field's value ("player1": "<guid>"); 0 when there is none or it does not fit
static inline int front_json_str(const char *t, const char *key, char *out, size_t cap) {
    const char *p = front_json_value(t, key); if (!p || *p != '"' || !cap) return 0;
    const char *q = strchr(p + 1, '"'); if (!q || (size_t)(q - p - 1) >= cap) return 0;
    memcpy(out, p + 1, (size_t)(q - p - 1)); out[q - p - 1] = 0; return 1;
}
// controls.json's text: the "pad" object's "<action>": "<input>" pairs, as front_keymap_parse does the keys (from the default; a reserved or unknown input,
// or one an earlier action asked for, is refused; "none" leaves an action without one; an action not asked for keeps its default input unless an asked one
// took it). The actions set; -1 when the file has no "pad" object (the front end then keeps its layout)
static inline int front_padmap_parse(front_padmap *m, const char *t, int *refused) {
    front_padmap_default(m);
    if (refused) *refused = 0;
    const char *p = front_json_value(t, "pad"), *end = p && *p == '{' ? strchr(p, '}') : NULL;
    if (!end) return -1;
    int want[FRONT_PACTS], set = 0, bad = 0;
    for (int i = 0; i < FRONT_PACTS; i++) want[i] = -2;   // -2 not asked, -1 asked for none
    for (p++; p < end && (p = memchr(p, '"', (size_t)(end - p))) != NULL; ) {
        const char *q = memchr(p + 1, '"', (size_t)(end - p - 1)); if (!q) break;
        const char *r = q + 1; while (r < end && (*r == ' ' || *r == '\t' || *r == '\r' || *r == '\n')) r++;
        if (r >= end || *r != ':') { p = q + 1; continue; }
        r++; while (r < end && (*r == ' ' || *r == '\t' || *r == '\r' || *r == '\n')) r++;
        const char *v0 = r < end && *r == '"' ? r + 1 : NULL, *v1 = v0 ? memchr(v0, '"', (size_t)(end - v0)) : NULL;
        if (!v1) { p = r; continue; }
        size_t n = (size_t)(q - p - 1), vn = (size_t)(v1 - v0);
        for (int a = 0; a < FRONT_PACTS; a++) if (strlen(front_pad_act_names[a]) == n && !strncmp(p + 1, front_pad_act_names[a], n)) {
            int none = vn == 4 && !strncmp(v0, "none", 4), in = none ? -1 : front_in_by_name(v0, vn);
            if (!none && front_in_reserved(in)) bad++; else want[a] = in;
        }
        p = v1 + 1;
    }
    for (int a = 0; a < FRONT_PACTS; a++) {   // an input asked twice goes to the first action
        if (want[a] == -2) continue;
        if (want[a] == -1) { m->in[a] = -1; set++; continue; }
        int dup = 0; for (int b = 0; b < a; b++) dup |= want[b] == want[a];
        if (dup) { bad++; want[a] = -2; } else { m->in[a] = (int8_t)want[a]; set++; }
    }
    for (int a = 0; a < FRONT_PACTS; a++) {
        if (want[a] != -2) continue;
        for (int b = 0; b < FRONT_PACTS; b++) if (want[b] >= 0 && want[b] == m->in[a]) { m->in[a] = -1; break; }
    }
    if (refused) *refused = bad;
    return set;
}
// The "pad" object for controls.json, without a trailing comma; its length
static inline int front_padmap_json(const front_padmap *m, char *out, size_t cap) {
    int o = snprintf(out, cap, "\"pad\":  {");
    for (int a = 0; a < FRONT_PACTS && o > 0 && (size_t)o < cap; a++)
        o += snprintf(out + o, cap - (size_t)o, "%s\"%s\": \"%s\"", a ? ", " : "", front_pad_act_names[a], m->in[a] >= 0 && m->in[a] < FRONT_INS ? front_in_names[m->in[a]] : "none");
    if (o > 0 && (size_t)o < cap) o += snprintf(out + o, cap - (size_t)o, "}");
    return o;
}
// An input for an action; the action that had it takes this one's old input (the two swap)
static inline void front_padmap_bind(front_padmap *m, int act, int in) {
    if (act < 0 || act >= FRONT_PACTS || front_in_reserved(in)) return;
    int old = m->in[act];
    for (int a = 0; a < FRONT_PACTS; a++) if (a != act && m->in[a] == in) m->in[a] = (int8_t)old;
    m->in[act] = (int8_t)in;
}
// The NpadButtons of the inputs held (held[i]: input i down, a trigger past FRONT_TRIGGER_PRESS)
static inline uint64_t front_padmap_buttons(const front_padmap *m, const uint8_t held[FRONT_INS]) {
    uint64_t b = 0;
    for (int a = 0; a < FRONT_PACTS; a++) if (m->in[a] >= 0 && m->in[a] < FRONT_INS && held[m->in[a]]) b |= 1ull << a;
    return b;
}
// The camera actions of the inputs held, as right-stick axes (up is positive y, as the Switch's radial)
static inline void front_padmap_cam(const front_padmap *m, const uint8_t held[FRONT_INS], int32_t *rx, int32_t *ry) {
    int l = m->in[FRONT_PAD_CAM_LEFT] >= 0 && held[m->in[FRONT_PAD_CAM_LEFT]], r = m->in[FRONT_PAD_CAM_RIGHT] >= 0 && held[m->in[FRONT_PAD_CAM_RIGHT]];
    int u = m->in[FRONT_PAD_CAM_UP] >= 0 && held[m->in[FRONT_PAD_CAM_UP]], d = m->in[FRONT_PAD_CAM_DOWN] >= 0 && held[m->in[FRONT_PAD_CAM_DOWN]];
    *rx = (r - l) * DM_STICK_MAX; *ry = (u - d) * DM_STICK_MAX;
}
