// The SDL3 front end (DESIGN D14): keyboard, mouse look and controllers → hle_hid.c, on the window's thread (vk_window.c calls in; SDL delivers a window's input and
// the controllers' events on the thread that pumps them). Behaviour follows nxnative's Mac front end (dm/host/front_mac.m, MIT, © seedds); written from SDL3's
// public headers (zlib licence). The mapping itself is front_map.h.
// Keyboard: player 1 (dm_input_set), the Mac's layout, or the map DM_KEYMAP names (play.ps1's Controls tab, front_map.h front_keymap_parse). Mouse, while the
// game profile has pointer look (hle_hid.c dm_look_on; Pokopia: direct look through look_cam): a click on the picture captures the mouse (relative mode: the pointer hides and stays put), its movement turns the camera by DM_LOOK_SENS degrees per count
// (default FRONT_LOOK_SENS), the left button is A, the right one B and the middle one the right stick's press; Esc, or the window losing focus, lets it go. Without
// pointer look the left button is the touch screen. Controllers: each is a player of its own (pad_slots.h: the first is player 1, with the keyboard); DM_PAD_MERGE=1
// makes every controller player 1, DM_KBD_PLAYER=own keeps player 1 for the keyboard alone. DM_PAD_FACE=labels maps face buttons by their printed letters,
// DM_PAD_INNER / DM_PAD_OUTER set the sticks' dead zones (front_map.h). Out of focus nothing reaches the game: what was held is released (front_mac.m input_release).
// M6 phase B: rumble (hle_haptics.c → SDL_RumbleGamepad, mixed by front_map.h front_rumble_mix; DM_RUMBLE=0 off, DM_RUMBLE_GAIN, DM_RUMBLE_MIN, suspended out of
// focus as front_mac.m does), gyro and accelerometer (SDL's gamepad sensors → dm_motion_set_hid in the Switch's frame, front_motion_frame; DM_MOTION=0 off), the
// player lights (SDL_SetGamepadPlayerIndex), and the controller check (DM_PAD_CHECK=1: each controller's details, presses, sticks, triggers, motion and rumble in the
// log and the window title; F5 / F6 run the strong / weak motor for a second, F7 sends player 1 a vibration through the game's path, hle_haptics.c)
// Our own additions: the controllers' map (DM_KEYMAP's file, controls.json: a "pad" object, front_map.h front_padmap; without one the layout above), the
// controller that is player 1 (its "player1" GUID: it takes player 1 when it connects; the in-game menu chooses it), more controllers (SDL's HIDAPI driver for
// the Steam Controller, gamecontrollerdb.txt beside nxrt.exe and in the settings folder, and a guessed layout for a device SDL knows no mapping for)
// Split screen (DM_SPLIT=1 or 2, docs/split-screen.md): two games on one PC. Each plays one controller, the one in place DM_SPLIT_PAD (default: its DM_SPLIT;
// 0 none), as its player 1; controllers keep playing while the window is out of focus (only one window can have it); only window 1 takes the keyboard and mouse
#include "dm.h"
#include "dm_input.h"
#include "front.h"
#include "front_map.h"
#include "hle_haptics.h"
#include "pad_slots.h"
#include "front_sdl.h"
#include "menu.h"
#include <SDL3/SDL.h>
#include <stdatomic.h>

#define NPADS 16
enum { RUMBLE_MS = 1000, CHECK_TEST_NS = 1000000000 };
#define RUMBLE_REFRESH_NS 400000000ull   // a running effect is sent again before its duration ends (a process that dies leaves no motor running past RUMBLE_MS)
#define CHECK_TITLE_NS 200000000ull
#define CHECK_LOG_NS 1000000000ull
typedef struct {
    SDL_Gamepad *g; SDL_JoystickID id; char name[64], guid[40]; int type;
    int rumble, motion, has_acc; float acc[3], rot[3];   // rumble: routed to its player; motion: its sensors on; the latest SDL samples
    uint64_t check_btn; float check_max[6];              // the controller check: the buttons last logged, the largest deflection seen per axis
} fpad;
static struct {
    SDL_Window *win;
    int input_on;                 // the window has focus
    front_kbd kbd;
    front_keymap keymap;          // the keyboard's actions (DM_KEYMAP, else the default layout)
    front_padmap padmap; int pad_custom;   // the controllers' map (DM_KEYMAP's "pad" object); without one (pad_custom 0) the layout of front_pad_button
    char p1_guid[40], p1_name[64];   // the controller that takes player 1 when it connects (DM_KEYMAP's "player1" and "player1_name"), "" none
    uint32_t ev_p1;               // the in-game menu's choice of player 1 (code: the joystick id), from whichever thread it was
    front_pad_info roster[NPADS]; int nroster;   // the connected controllers for the menu (under klk)
    uint64_t look_btn;            // the mouse buttons while it looks, ORed into the keyboard's
    int look_cap, mouse_touch;
    float sens, inner, outer;
    int merge, kbd_own, by_label;
    int split, split_pad;         // split screen: this window (1, 2; 0 off) and the place (1-based) of the controller it plays (0 none)
    char title[200];
    pad_slots slots;
    fpad pads[NPADS];             // fixed places: an entry's address is its slot owner
    // rumble: the levels of the game's latest snapshot per player (the haptics sink writes them on a game thread), sent from the window thread
    int rumble_on, motion_on; float rumble_gain, rumble_min;
    uint32_t ev_rumble; _Atomic int rumble_pending; SRWLOCK rlk;
    uint32_t ev_menu;             // the in-game menu opened (code 1) or closed (0), from whichever thread it was
    SRWLOCK klk;                  // the keymap: the in-game menu changes it from the present thread
    struct { uint16_t strong, weak; int dirty; } rum[DM_PLAYERS];
    uint64_t rum_sent[DM_PLAYERS]; uint16_t rum_now[DM_PLAYERS][2];
    // the controller check (DM_PAD_CHECK)
    int check, test_kind; uint64_t check_title_ns, check_log_ns, test_end_ns;
} F;

static SRWLOCK g_sdl_lock = SRWLOCK_INIT;
void front_sdl_lock(void) { AcquireSRWLockExclusive(&g_sdl_lock); }
void front_sdl_unlock(void) { ReleaseSRWLockExclusive(&g_sdl_lock); }

static float env_f(const char *k, float def) { const char *e = getenv(k); return e && *e ? (float)atof(e) : def; }
static void push(void) {
    int32_t lx, ly, krx, kry;
    front_kbd_axes(&F.kbd, &lx, &ly);
    front_kbd_raxes(&F.kbd, &krx, &kry);
    dm_input_set(F.kbd.btn | F.look_btn, lx, ly, krx, kry);
}

// ---------- The keyboard map: DM_KEYMAP=<file> (play.ps1's Controls tab), else the default layout; the active one is logged ----------
static void keymap_load(void) {
    const char *path = getenv("DM_KEYMAP"); char why[MAX_PATH + 80] = "the default layout";
    front_keymap_default(&F.keymap); front_padmap_default(&F.padmap);
    if (path && *path) {
        FILE *f = fopen(path, "rb"); char *t = NULL; long n = 0;
        if (f && !fseek(f, 0, SEEK_END) && (n = ftell(f)) > 0 && n < (1 << 16) && !fseek(f, 0, SEEK_SET) && (t = calloc(1, (size_t)n + 1)) && fread(t, 1, (size_t)n, f) == (size_t)n) {
            int refused = 0, set = front_keymap_parse(&F.keymap, t, &refused);
            snprintf(why, sizeof why, "%s (%d action%s set%s)", path, set, set == 1 ? "" : "s", refused ? ", some refused: a reserved key or a key used twice" : "");
            int pref = 0, pset = front_padmap_parse(&F.padmap, t, &pref);   // the controllers' map and player 1, from the same file
            F.pad_custom = pset >= 0;
            if (!front_json_str(t, "player1", F.p1_guid, sizeof F.p1_guid)) F.p1_guid[0] = 0;
            if (!front_json_str(t, "player1_name", F.p1_name, sizeof F.p1_name)) F.p1_name[0] = 0;
            if (F.pad_custom) {
                char line[700]; int o = 0;
                for (int a = 0; a < FRONT_PACTS && o < (int)sizeof line - 40; a++)
                    o += snprintf(line + o, sizeof line - (size_t)o, "%s%s %s", a ? ", " : "", front_act_names[a], F.padmap.in[a] >= 0 ? front_in_names[F.padmap.in[a]] : "none");
                LOG("front end: controllers from %s (%d button%s set%s): %s", path, pset, pset == 1 ? "" : "s", pref ? ", some refused: Guide, an unknown input or one used twice" : "", line);
            }
            if (F.p1_guid[0]) LOG("front end: player 1 is '%s' (%s) when it is connected (the in-game menu chose it)", F.p1_name[0] ? F.p1_name : "a controller", F.p1_guid);
        } else {
            LOG("[RED] DM_KEYMAP: cannot read %s: the default layout", path);
        }
        free(t); if (f) fclose(f);
    }
    char line[1024]; int o = 0;
    for (int i = 0; i < FRONT_ACTS && o < (int)sizeof line - 40; i++) {
        const char *k = F.keymap.key[i] == SDL_SCANCODE_UNKNOWN ? "none" : SDL_GetScancodeName(F.keymap.key[i]);
        o += snprintf(line + o, sizeof line - (size_t)o, "%s%s %s", i ? ", " : "", front_act_names[i], k && *k ? k : "?");
    }
    LOG("front end: keyboard from %s: %s; F11 or Alt+Enter fullscreen", why, line);
}
// The in-game menu's Controls tab (menu.c): the map as it is, a new one (the file is the menu's to write)
void front_sdl_keymap_get(front_keymap *m) { AcquireSRWLockShared(&F.klk); *m = F.keymap; ReleaseSRWLockShared(&F.klk); }
void front_sdl_keymap_set(const front_keymap *m) { AcquireSRWLockExclusive(&F.klk); F.keymap = *m; ReleaseSRWLockExclusive(&F.klk); }
// The controllers' map (custom 0: none chosen, the layout of front_pad_button) and the controller remembered as player 1
void front_sdl_padmap_get(front_padmap *m, int *custom, char guid[40], char name[64]) {
    AcquireSRWLockShared(&F.klk); *m = F.padmap; *custom = F.pad_custom; memcpy(guid, F.p1_guid, 40); memcpy(name, F.p1_name, 64); ReleaseSRWLockShared(&F.klk);
}
// The controller remembered for player 1: its GUID and, when one is kept, its name (Steam's virtual controllers share one GUID; their names tell them apart)
static int p1_match(const fpad *p) {
    char n[64]; front_name_clean(p->name, n, sizeof n);
    return F.p1_guid[0] && !strcmp(F.p1_guid, p->guid) && (!F.p1_name[0] || !strcmp(F.p1_name, n));
}
void front_sdl_padmap_set(const front_padmap *m, int custom) { AcquireSRWLockExclusive(&F.klk); F.padmap = *m; F.pad_custom = custom; ReleaseSRWLockExclusive(&F.klk); }
int front_sdl_pads(front_pad_info *out, int max) {
    AcquireSRWLockShared(&F.klk); int n = F.nroster < max ? F.nroster : max; memcpy(out, F.roster, sizeof *out * (size_t)(n > 0 ? n : 0)); ReleaseSRWLockShared(&F.klk);
    return n;
}
void front_sdl_player1(uint32_t id) {
    if (!F.ev_p1) return;
    SDL_Event e; SDL_zero(e); e.type = F.ev_p1; e.user.code = (Sint32)id; SDL_PushEvent(&e);
}
void front_sdl_menu_opened(int open) {   // the game's input let go while it is open, taken up again after (on the window thread)
    if (!F.ev_menu) return;
    SDL_Event e; SDL_zero(e); e.type = F.ev_menu; e.user.code = open; SDL_PushEvent(&e);
}

// ---------- Mouse look ----------
static void look_capture(int on) {
    if (on == F.look_cap) return;
    F.look_cap = on;
    SDL_SetWindowRelativeMouseMode(F.win, on);
    char t[300]; snprintf(t, sizeof t, "%s%s", F.title, on ? " - mouse look (Esc releases the mouse)" : "");
    SDL_SetWindowTitle(F.win, t);
    LOG("front end: mouse look %s", on ? "on (Esc releases the mouse)" : "off");
}
static void look_button(uint64_t bit, int down) { F.look_btn = down ? F.look_btn | bit : F.look_btn & ~bit; push(); }
static void touch_at(float x, float y) {
    int w = 0, h = 0; SDL_GetWindowSize(F.win, &w, &h);
    dm_touch t = {0}; front_touch_point(x, y, w, h, &t.x, &t.y);
    dm_touch_set(&t, 1); F.mouse_touch = 1;
}

// ---------- Controllers ----------
static fpad *pad_by_id(SDL_JoystickID id) { for (int i = 0; i < NPADS; i++) if (F.pads[i].g && F.pads[i].id == id) return &F.pads[i]; return NULL; }
static int pad_player(fpad *p) {
    if (F.merge) return 0;
    int s = pad_slot_of(&F.slots, p);
    if (F.split) return F.split_pad && s == F.split_pad - 1 ? 0 : -1;   // split screen: this window's controller is its player 1, the others are the other window's
    return s;
}
static void players_note(void) { dm_pad_players(F.split ? 1u : pad_slot_mask(&F.slots)); }
static void pad_light(fpad *p) {   // the player lights; in split screen only this window's controller, lit as its window
    int pl = pad_player(p);
    if (!F.split) SDL_SetGamepadPlayerIndex(p->g, pl); else if (pl == 0) SDL_SetGamepadPlayerIndex(p->g, F.split - 1);
}
static uint64_t pad_buttons(fpad *p, uint8_t held[FRONT_INS]) {
    uint64_t m = 0;
    if (F.pad_custom) {
        AcquireSRWLockShared(&F.klk);
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++) held[b] = SDL_GetGamepadButton(p->g, (SDL_GamepadButton)b);
        held[FRONT_IN_LT] = (uint8_t)front_trigger(SDL_GetGamepadAxis(p->g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
        held[FRONT_IN_RT] = (uint8_t)front_trigger(SDL_GetGamepadAxis(p->g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
        m = front_padmap_buttons(&F.padmap, held);   // the Switch buttons; the camera's bindings come back through held (pad_read's axes)
        ReleaseSRWLockShared(&F.klk);
        return m;
    }
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++)
        if (SDL_GetGamepadButton(p->g, (SDL_GamepadButton)b))
            m |= front_pad_button((SDL_GamepadButton)b, F.by_label, b <= SDL_GAMEPAD_BUTTON_NORTH ? SDL_GetGamepadButtonLabel(p->g, (SDL_GamepadButton)b) : SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN);
    if (front_trigger(SDL_GetGamepadAxis(p->g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER))) m |= DM_BTN_ZL;
    if (front_trigger(SDL_GetGamepadAxis(p->g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER))) m |= DM_BTN_ZR;
    return m;
}
static void check_buttons(fpad *p, uint64_t m);
static void pad_read(fpad *p) {
    if (!F.input_on && !F.split) return;
    int pl = pad_player(p); if (pl < 0) return;
    uint8_t held[FRONT_INS] = {0};
    uint64_t m = pad_buttons(p, held);
    int32_t lx, ly, rx, ry;
    front_stick(SDL_GetGamepadAxis(p->g, SDL_GAMEPAD_AXIS_LEFTX), SDL_GetGamepadAxis(p->g, SDL_GAMEPAD_AXIS_LEFTY), F.inner, F.outer, &lx, &ly);
    front_stick(SDL_GetGamepadAxis(p->g, SDL_GAMEPAD_AXIS_RIGHTX), SDL_GetGamepadAxis(p->g, SDL_GAMEPAD_AXIS_RIGHTY), F.inner, F.outer, &rx, &ry);
    AcquireSRWLockShared(&F.klk);
    if (F.pad_custom) { int32_t cx, cy; front_padmap_cam(&F.padmap, held, &cx, &cy); rx += cx; ry += cy; }   // a camera button held: add its direction to the right stick
    ReleaseSRWLockShared(&F.klk);
    if (rx > DM_STICK_MAX) rx = DM_STICK_MAX; if (rx < -DM_STICK_MAX) rx = -DM_STICK_MAX;
    if (ry > DM_STICK_MAX) ry = DM_STICK_MAX; if (ry < -DM_STICK_MAX) ry = -DM_STICK_MAX;
    dm_pad_set_player(pl, m, lx, ly, rx, ry);
    if (F.check) check_buttons(p, m);
}
static void npad_name(int p, char *out, size_t cap) { uint32_t id = dm_player_npad(p); if (id == 0x20) snprintf(out, cap, "handheld"); else snprintf(out, cap, "%u", id); }
static const char *face_layout(SDL_Gamepad *g) {
    if (F.pad_custom) return "buttons as the controller map says (controls.json)";
    if (F.by_label && SDL_GetGamepadButtonLabel(g, SDL_GAMEPAD_BUTTON_SOUTH) == SDL_GAMEPAD_BUTTON_LABEL_A) return "face buttons by their letters (DM_PAD_FACE=labels)";
    return SDL_GetGamepadButtonLabel(g, SDL_GAMEPAD_BUTTON_EAST) == SDL_GAMEPAD_BUTTON_LABEL_A ? "Nintendo layout, buttons as labelled"
                                                                                              : "face buttons by position (right A, bottom B, top X, left Y)";
}
// Rumble goes to a player while one of its controllers can rumble (hle_haptics.c routes: the game sees vibration devices only then)
static void rumble_route(int pl) {
    if (pl < 0) return;
    int any = 0; for (int i = 0; i < NPADS; i++) any |= F.pads[i].g && F.pads[i].rumble && pad_player(&F.pads[i]) == pl;
    dm_haptics_route(pl, any);
}
static void roster_update(void) {   // the menu's list: every connected controller with its player
    AcquireSRWLockExclusive(&F.klk);
    F.nroster = 0;
    for (int i = 0; i < NPADS; i++) {
        fpad *p = &F.pads[i]; if (!p->g) continue;
        front_pad_info *r = &F.roster[F.nroster++];
        r->id = p->id; r->player = pad_player(p); r->type = p->type;
        snprintf(r->name, sizeof r->name, "%s", p->name); snprintf(r->guid, sizeof r->guid, "%s", p->guid);
    }
    ReleaseSRWLockExclusive(&F.klk);
}
static const char g_kbd_token = 0;   // the keyboard's hold on player 1 (DM_KBD_PLAYER=own)
// p made player 1: whoever had it takes p's place; both players' buttons, lights, rumble and motion follow
static void pad_first(fpad *p, const char *why) {
    if (F.merge || !p) return;
    int was = pad_slot_of(&F.slots, p);
    if (pad_slot_first(&F.slots, p, &g_kbd_token) != 0 || was <= 0) return;
    dm_pad_set_player(0, 0, 0, 0, 0, 0); dm_pad_set_player(was, 0, 0, 0, 0, 0);   // nothing stays pressed in either place
    dm_motion_clear_hid(0); dm_motion_clear_hid(was);
    for (int i = 0; i < NPADS; i++) if (F.pads[i].g) { pad_light(&F.pads[i]); pad_read(&F.pads[i]); }
    rumble_route(0); rumble_route(was);
    roster_update();
    LOG("front end: controller '%s' is player 1 now (%s); player 1's controller before it is player %d", p->name, why, was + 1);
}
static void check_pad_details(fpad *p);
static void pad_added(SDL_JoystickID id) {
    if (pad_by_id(id)) return;
    fpad *p = NULL; for (int i = 0; i < NPADS && !p; i++) if (!F.pads[i].g) p = &F.pads[i];
    if (!p) { LOG("front end: controller %u connected: more than %d controllers, not used", (unsigned)id, NPADS); return; }
    SDL_Gamepad *g = SDL_OpenGamepad(id);
    if (!g) { LOG("[RED] front end: controller %u could not be opened: %s", (unsigned)id, SDL_GetError()); return; }
    p->g = g; p->id = id; snprintf(p->name, sizeof p->name, "%s", SDL_GetGamepadName(g) ? SDL_GetGamepadName(g) : "?");
    SDL_GUIDToString(SDL_GetGamepadGUIDForID(id), p->guid, sizeof p->guid); p->type = (int)SDL_GetGamepadType(g);
    int pl = F.merge ? 0 : pad_slot_take(&F.slots, p, p->name); char nb[16];
    const char *type = SDL_GetGamepadStringForType(SDL_GetGamepadType(g));
    if (pl < 0) { LOG("front end: controller '%s' connected: all %d players are taken, not used", p->name, DM_PLAYERS); SDL_CloseGamepad(g); memset(p, 0, sizeof *p); return; }
    p->rumble = F.rumble_on && SDL_GetBooleanProperty(SDL_GetGamepadProperties(g), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
    if (F.motion_on && SDL_GamepadHasSensor(g, SDL_SENSOR_GYRO) && SDL_GamepadHasSensor(g, SDL_SENSOR_ACCEL))
        p->motion = SDL_SetGamepadSensorEnabled(g, SDL_SENSOR_GYRO, true) && SDL_SetGamepadSensorEnabled(g, SDL_SENSOR_ACCEL, true);
    pad_light(p);   // the player lights (Switch Pro, DualSense, Xbox 360); no effect elsewhere
    npad_name(pl, nb, sizeof nb);
    LOG("front end: controller '%s' (%s) connected: player %d (npad %s), %s; rumble %s, motion %s%s", p->name, type ? type : "?", pl + 1, nb, face_layout(g),
        p->rumble ? "on" : !F.rumble_on ? "off (DM_RUMBLE=0)" : "none", p->motion ? "on (gyro + accelerometer)" : !F.motion_on ? "off (DM_MOTION=0)" : "none",
        !F.split ? "" : pad_player(p) == 0 ? "; split screen: this window's player" : "; split screen: the other window's");
    if (!F.merge) players_note();
    if (p->rumble) rumble_route(pad_player(p));
    if (F.check) check_pad_details(p);
    pad_read(p);
    roster_update();
    if (pl > 0 && p1_match(p)) {   // the controller remembered as player 1, unless player 1 already has one like it
        fpad *q = NULL; for (int i = 0; i < NPADS && !q; i++) if (F.pads[i].g && pad_player(&F.pads[i]) == 0) q = &F.pads[i];
        if (!q || !p1_match(q)) pad_first(p, "the controller remembered for player 1");
    }
}
static void pad_removed(SDL_JoystickID id) {
    fpad *p = pad_by_id(id); if (!p) return;
    int pl = pad_player(p); char nb[16]; npad_name(pl < 0 ? 0 : pl, nb, sizeof nb);
    if (F.merge) LOG("front end: controller '%s' disconnected", p->name);
    else LOG("front end: controller '%s' disconnected: player %d (npad %s) is free", p->name, pl + 1, nb);
    int rumbled = p->rumble, moved = p->motion;
    if (p->rumble) SDL_RumbleGamepad(p->g, 0, 0, 0);
    if (F.merge) {   // as front_mac.m: released when the last controller goes away
        SDL_CloseGamepad(p->g); memset(p, 0, sizeof *p);
        int any = 0; for (int i = 0; i < NPADS; i++) any |= F.pads[i].g != NULL;
        if (!any) dm_pad_set(0, 0, 0, 0, 0);
        if (rumbled) rumble_route(0);
        if (moved && !any) dm_motion_clear_hid(0);
        return;
    }
    pad_slot_free(&F.slots, p);
    if (pl >= 0) dm_pad_set_player(pl, 0, 0, 0, 0, 0);   // nothing stays pressed
    SDL_CloseGamepad(p->g); memset(p, 0, sizeof *p);
    players_note();
    if (rumbled) rumble_route(pl);
    if (moved && pl >= 0) dm_motion_clear_hid(pl);
    roster_update();
}
// A device SDL knows no gamepad mapping for: a guessed layout (the common DirectInput one: buttons 0-3 the face buttons, 4 and 5 the shoulders, 6 and 7
// Back and Start, 8 and 9 the stick presses; axes 0-1 the left stick, 2-3 the right, 4-5 the triggers; hat 0 the D-pad), so it plays at all; the in-game
// menu's Controller page or a line in gamecontrollerdb.txt fixes the rest. SDL then reports it as a gamepad
static void guess_mapping(SDL_JoystickID id) {
    if (SDL_IsGamepad(id)) return;
    SDL_Joystick *j = SDL_OpenJoystick(id); if (!j) return;
    int nb = SDL_GetNumJoystickButtons(j), na = SDL_GetNumJoystickAxes(j), nh = SDL_GetNumJoystickHats(j);
    const char *name = SDL_GetJoystickName(j) ? SDL_GetJoystickName(j) : "a controller";
    char guid[40], m[700]; SDL_GUIDToString(SDL_GetJoystickGUID(j), guid, sizeof guid);
    if (nb < 4 || (na < 2 && nh < 1)) { LOG("front end: '%s' (%d buttons, %d axes, %d hats) is not a controller SDL knows, and too small to guess: not used", name, nb, na, nh); SDL_CloseJoystick(j); return; }
    static const char *const bn[10] = {"a", "b", "x", "y", "leftshoulder", "rightshoulder", "back", "start", "leftstick", "rightstick"};
    int o = snprintf(m, sizeof m, "%s,%s", guid, "Guessed layout");
    for (int b = 0; b < 10 && b < nb; b++) o += snprintf(m + o, sizeof m - (size_t)o, ",%s:b%d", bn[b], b);
    if (na >= 2) o += snprintf(m + o, sizeof m - (size_t)o, ",leftx:a0,lefty:a1");
    if (na >= 4) o += snprintf(m + o, sizeof m - (size_t)o, ",rightx:a2,righty:a3");
    if (na >= 6) o += snprintf(m + o, sizeof m - (size_t)o, ",lefttrigger:a4,righttrigger:a5");
    if (nh >= 1) o += snprintf(m + o, sizeof m - (size_t)o, ",dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8");
    LOG("front end: '%s' (%d buttons, %d axes, %d hats) has no SDL mapping: a guessed layout (the in-game menu's Controller page, or a line for it in "
        "gamecontrollerdb.txt, fixes what is wrong): %s", name, nb, na, nh, m);
    SDL_CloseJoystick(j);
    if (SDL_AddGamepadMapping(m) < 0) LOG("[RED] front end: the guessed layout for '%s' was refused: %s", name, SDL_GetError());
}
// SDL's mapping files (SDL_GameControllerDB's format): beside nxrt.exe (the kit's), then the settings folder's (the player's own lines win)
const char *mn_settings_dir(void);
static void mappings_load(void) {
    char p[1200]; const char *base = SDL_GetBasePath(), *dir = mn_settings_dir();
    const char *where[2] = {base, dir}; const char *sep[2] = {"", "/"};
    for (int k = 0; k < 2; k++) {
        if (!where[k] || !*where[k]) continue;
        snprintf(p, sizeof p, "%s%sgamecontrollerdb.txt", where[k], sep[k]);
        FILE *f = fopen(p, "rb"); if (!f) continue; fclose(f);
        int n = SDL_AddGamepadMappingsFromFile(p);
        if (n < 0) LOG("[RED] front end: controller mappings in %s could not be read: %s", p, SDL_GetError());
        else LOG("front end: %d controller mapping%s from %s", n, n == 1 ? "" : "s", p);
    }
}
// Tests: DM_PAD_VIRTUAL=<n> attaches n virtual controllers (SDL's virtual joysticks, gamepads with every button and axis, "Virtual pad 1".. with USB ids
// 1209:7001..), and DM_CTL "menu vpad <i> <input name> <0|1>" holds or lets go one of their inputs (front_map.h's names; a trigger is pulled all the way)
static SDL_JoystickID g_vpad[4]; static int g_nvpad;
static void vpads_attach(void) {
    const char *e = getenv("DM_PAD_VIRTUAL"); int n = e ? atoi(e) : 0; if (n > 4) n = 4;
    for (int i = 0; i < n; i++) {
        SDL_VirtualJoystickDesc d; SDL_INIT_INTERFACE(&d); char nm[32]; snprintf(nm, sizeof nm, "Virtual pad %d", i + 1);
        d.type = SDL_JOYSTICK_TYPE_GAMEPAD; d.vendor_id = 0x1209; d.product_id = (Uint16)(0x7001 + i); d.naxes = SDL_GAMEPAD_AXIS_COUNT; d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT; d.name = nm;
        SDL_JoystickID id = SDL_AttachVirtualJoystick(&d);
        if (!id) { LOG("[RED] DM_PAD_VIRTUAL: virtual controller %d not attached: %s", i + 1, SDL_GetError()); break; }
        g_vpad[g_nvpad++] = id;
    }
    if (g_nvpad) LOG("front end: %d virtual controller%s attached (DM_PAD_VIRTUAL, tests)", g_nvpad, g_nvpad == 1 ? "" : "s");
}
void front_sdl_vpad(const char *cmd) {
    int i = -1, v = 0; char nm[32];
    if (sscanf(cmd, "%d %31s %d", &i, nm, &v) != 3 || i < 0 || i >= g_nvpad) return;
    SDL_Joystick *j = SDL_GetJoystickFromID(g_vpad[i]); int in = front_in_by_name(nm, strlen(nm));
    if (!j || in < 0) return;
    if (in == FRONT_IN_LT || in == FRONT_IN_RT) SDL_SetJoystickVirtualAxis(j, in == FRONT_IN_LT ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, v ? 32767 : 0);
    else SDL_SetJoystickVirtualButton(j, in, v != 0);
}
void front_sdl_hints(void) {
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_STEAM, "1");   // Steam Controllers without Steam running (an environment variable of that name still decides)
}

// ---------- Motion ----------
static void pad_sensor(const SDL_GamepadSensorEvent *e) {
    fpad *p = pad_by_id(e->which); if (!p || !p->motion) return;
    if (e->sensor == SDL_SENSOR_ACCEL) { memcpy(p->acc, e->data, sizeof p->acc); p->has_acc = 1; return; }
    if (e->sensor != SDL_SENSOR_GYRO) return;
    memcpy(p->rot, e->data, sizeof p->rot);
    int pl = pad_player(p);
    if ((!F.input_on && !F.split) || !p->has_acc || pl < 0) return;
    float acc[3], rot[3]; front_motion_frame(p->acc, p->rot, acc, rot);
    uint64_t now = now_ns(), sdl_now = SDL_GetTicksNS(), age = sdl_now > e->timestamp ? sdl_now - e->timestamp : 0;   // the sample's time on the host clock
    dm_motion_set_hid(pl, acc, rot, now > age ? now - age : now);
}

// ---------- Rumble ----------
// The haptics sink (hle_haptics.c): called under its lock on a game thread; keeps the levels and wakes the window thread, which sends them (no SDL call here)
static void rumble_sink(dm_haptic_state s) {
    if (s.player < 0 || s.player >= DM_PLAYERS) return;
    uint16_t a, b;
    front_rumble_mix(s.motor[0].amplitude_low, s.motor[0].amplitude_high, s.motor[1].amplitude_low, s.motor[1].amplitude_high, F.rumble_gain, F.rumble_min, &a, &b);
    AcquireSRWLockExclusive(&F.rlk);
    int change = F.rum[s.player].strong != a || F.rum[s.player].weak != b;
    F.rum[s.player].strong = a; F.rum[s.player].weak = b; F.rum[s.player].dirty |= change;
    ReleaseSRWLockExclusive(&F.rlk);
    if (change && F.ev_rumble && !atomic_exchange(&F.rumble_pending, 1)) { SDL_Event e; SDL_zero(e); e.type = F.ev_rumble; SDL_PushEvent(&e); }
}
// On the window thread: changed levels to the player's controllers, and running effects sent again before their duration ends
static void rumble_apply(void) {
    atomic_store(&F.rumble_pending, 0);
    uint64_t now = SDL_GetTicksNS();
    for (int pl = 0; pl < DM_PLAYERS; pl++) {
        AcquireSRWLockExclusive(&F.rlk);
        uint16_t a = F.rum[pl].strong, b = F.rum[pl].weak; int dirty = F.rum[pl].dirty; F.rum[pl].dirty = 0;
        ReleaseSRWLockExclusive(&F.rlk);
        if (!dirty && !((a || b) && now - F.rum_sent[pl] > RUMBLE_REFRESH_NS)) continue;
        F.rum_sent[pl] = now;
        int sent = 0;
        for (int i = 0; i < NPADS; i++) if (F.pads[i].g && F.pads[i].rumble && pad_player(&F.pads[i]) == pl) { SDL_RumbleGamepad(F.pads[i].g, a, b, a || b ? RUMBLE_MS : 0); sent++; }
        if (F.check && dirty && sent && ((a || b) != (F.rum_now[pl][0] || F.rum_now[pl][1])))
            LOG("pad check: rumble for player %d %s (strong %.2f, weak %.2f)", pl + 1, a || b ? "starts" : "stops", a / 65535.0, b / 65535.0);
        F.rum_now[pl][0] = a; F.rum_now[pl][1] = b;
    }
}

// ---------- The controller check (DM_PAD_CHECK) ----------
static void check_pad_details(fpad *p) {
    SDL_Gamepad *g = p->g; SDL_PropertiesID pr = SDL_GetGamepadProperties(g); int pct = -1;
    SDL_PowerState ps = SDL_GetGamepadPowerInfo(g, &pct);
    SDL_JoystickConnectionState cs = SDL_GetGamepadConnectionState(g);
    LOG("pad check: '%s': type %s, USB id %04x:%04x, %s, battery %s%s%d%%; rumble %d, trigger rumble %d, gyro %d (%.0f Hz), accelerometer %d (%.0f Hz), touchpads %d, "
        "light %d, player lights %d", p->name, SDL_GetGamepadStringForType(SDL_GetGamepadType(g)) ? SDL_GetGamepadStringForType(SDL_GetGamepadType(g)) : "?",
        SDL_GetGamepadVendor(g), SDL_GetGamepadProduct(g), cs == SDL_JOYSTICK_CONNECTION_WIRED ? "wired" : cs == SDL_JOYSTICK_CONNECTION_WIRELESS ? "wireless" : "connection unknown",
        ps == SDL_POWERSTATE_ON_BATTERY ? "on battery " : ps == SDL_POWERSTATE_CHARGING ? "charging " : ps == SDL_POWERSTATE_CHARGED ? "charged " : ps == SDL_POWERSTATE_NO_BATTERY ? "none " : "unknown ",
        pct >= 0 ? "" : "?", pct, (int)SDL_GetBooleanProperty(pr, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false), (int)SDL_GetBooleanProperty(pr, SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN, false),
        (int)SDL_GamepadHasSensor(g, SDL_SENSOR_GYRO), SDL_GetGamepadSensorDataRate(g, SDL_SENSOR_GYRO), (int)SDL_GamepadHasSensor(g, SDL_SENSOR_ACCEL), SDL_GetGamepadSensorDataRate(g, SDL_SENSOR_ACCEL),
        SDL_GetNumGamepadTouchpads(g), (int)SDL_GetBooleanProperty(pr, SDL_PROP_GAMEPAD_CAP_RGB_LED_BOOLEAN, false), (int)SDL_GetBooleanProperty(pr, SDL_PROP_GAMEPAD_CAP_PLAYER_LED_BOOLEAN, false));
    char *map = SDL_GetGamepadMapping(g);
    LOG("pad check: '%s' mapping: %s", p->name, map ? map : "(none)");
    SDL_free(map);
    LOG("pad check: press every button once (each press is logged with the Switch button it plays), turn both sticks round to their edges, pull both triggers, tilt and "
        "turn the controller (motion is logged once a second), F5 / F6 run the strong / weak motor for a second, F7 vibrates player 1 through the game's path");
}
// A controller button as the player sees it: a face button by its place and printed label, the others by SDL's name for them
static const char *btn_name(SDL_Gamepad *g, int b, char *buf, size_t n) {
    static const char *pos[4] = {"bottom", "right", "left", "top"};   // SOUTH, EAST, WEST, NORTH
    static const char *lab[9] = {"unlabelled", "A", "B", "X", "Y", "Cross", "Circle", "Square", "Triangle"};
    if (b <= SDL_GAMEPAD_BUTTON_NORTH) { int l = (int)SDL_GetGamepadButtonLabel(g, (SDL_GamepadButton)b); snprintf(buf, n, "%s face button (%s)", pos[b], l >= 0 && l < 9 ? lab[l] : "?"); return buf; }
    const char *s = SDL_GetGamepadStringForButton((SDL_GamepadButton)b);
    snprintf(buf, n, "%s", s ? s : "?"); return buf;
}
static void check_buttons(fpad *p, uint64_t m) {
    uint64_t down = m & ~p->check_btn; p->check_btn = m;
    if (!down) return;
    char held[240] = "", sw[120], b1[48]; int o = 0;
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && o < 200; b++)
        if (SDL_GetGamepadButton(p->g, (SDL_GamepadButton)b)) o += snprintf(held + o, sizeof held - (size_t)o, "%s%s", o ? ", " : "", btn_name(p->g, b, b1, sizeof b1));
    front_button_names(down, sw, sizeof sw);
    LOG("pad check: '%s' -> Switch %s (held on the controller: %s)", p->name, sw, o ? held : "a trigger past a quarter of its travel");
}
static void check_axes(fpad *p) {   // the largest deflection seen per axis (sticks after the dead zones, triggers raw)
    int32_t s[4]; front_stick(SDL_GetGamepadAxis(p->g, SDL_GAMEPAD_AXIS_LEFTX), SDL_GetGamepadAxis(p->g, SDL_GAMEPAD_AXIS_LEFTY), F.inner, F.outer, &s[0], &s[1]);
    front_stick(SDL_GetGamepadAxis(p->g, SDL_GAMEPAD_AXIS_RIGHTX), SDL_GetGamepadAxis(p->g, SDL_GAMEPAD_AXIS_RIGHTY), F.inner, F.outer, &s[2], &s[3]);
    float v[6] = {(float)hypot(s[0], s[1]) / DM_STICK_MAX, 0, (float)hypot(s[2], s[3]) / DM_STICK_MAX, 0,
                  SDL_GetGamepadAxis(p->g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) / 32767.0f, SDL_GetGamepadAxis(p->g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) / 32767.0f};
    for (int i = 0; i < 6; i++) if (v[i] > p->check_max[i]) p->check_max[i] = v[i];
}
static void check_tick(uint64_t now) {
    if (F.test_kind && now >= F.test_end_ns) {   // the end of an F5 / F6 / F7 test
        if (F.test_kind == 3) { dm_vibration_update u[2] = {{0, 0, {0, 160, 0, 320}}, {0, 1, {0, 160, 0, 320}}}; dm_haptics_send(u, 2); }
        else for (int i = 0; i < NPADS; i++) if (F.pads[i].g && F.pads[i].rumble) SDL_RumbleGamepad(F.pads[i].g, 0, 0, 0);
        LOG("pad check: rumble test ends"); F.test_kind = 0;
    }
    for (int i = 0; i < NPADS; i++) if (F.pads[i].g) check_axes(&F.pads[i]);
    int log = now - F.check_log_ns >= CHECK_LOG_NS, title = now - F.check_title_ns >= CHECK_TITLE_NS;
    if (!log && !title) return;
    char t[400]; int o = snprintf(t, sizeof t, "%s | pad check:", F.title);
    for (int i = 0; i < NPADS; i++) {
        fpad *p = &F.pads[i]; if (!p->g) continue;
        char held[120]; uint8_t chk_held[FRONT_INS] = {0}; front_button_names(pad_buttons(p, chk_held), held, sizeof held);
        int32_t lx, ly, rx, ry;
        front_stick(SDL_GetGamepadAxis(p->g, SDL_GAMEPAD_AXIS_LEFTX), SDL_GetGamepadAxis(p->g, SDL_GAMEPAD_AXIS_LEFTY), F.inner, F.outer, &lx, &ly);
        front_stick(SDL_GetGamepadAxis(p->g, SDL_GAMEPAD_AXIS_RIGHTX), SDL_GetGamepadAxis(p->g, SDL_GAMEPAD_AXIS_RIGHTY), F.inner, F.outer, &rx, &ry);
        float acc[3] = {0}, rot[3] = {0}; if (p->motion && p->has_acc) front_motion_frame(p->acc, p->rot, acc, rot);
        int pl = pad_player(p);
        o += snprintf(t + o, sizeof t - o, " [P%d %.20s: %s L%+.2f,%+.2f R%+.2f,%+.2f", pl + 1, p->name, held[0] ? held : "-", lx / (float)DM_STICK_MAX, ly / (float)DM_STICK_MAX,
                      rx / (float)DM_STICK_MAX, ry / (float)DM_STICK_MAX);
        if (p->motion) o += snprintf(t + o, sizeof t - o, " acc %+.1f,%+.1f,%+.1f", acc[0], acc[1], acc[2]);
        o += snprintf(t + o, sizeof t - o, "]");
        if (o >= (int)sizeof t - 2) break;
        if (log) {
            LOG("pad check: '%s' player %d: held %s; left stick %+.2f,%+.2f, right %+.2f,%+.2f (largest so far: left %.2f, right %.2f; triggers %.2f / %.2f)", p->name, pl + 1,
                held[0] ? held : "nothing", lx / (float)DM_STICK_MAX, ly / (float)DM_STICK_MAX, rx / (float)DM_STICK_MAX, ry / (float)DM_STICK_MAX, p->check_max[0], p->check_max[2], p->check_max[4], p->check_max[5]);
            if (p->motion) LOG("pad check: '%s' motion in the Switch frame: acc %+.2f %+.2f %+.2f G (flat and still: 0 0 -1), gyro %+.2f %+.2f %+.2f rad/s (still: about 0)%s", p->name,
                               acc[0], acc[1], acc[2], rot[0], rot[1], rot[2], p->has_acc ? "" : " (no accelerometer sample yet)");
            if (p->rumble && pl >= 0 && (F.rum_now[pl][0] || F.rum_now[pl][1])) LOG("pad check: '%s' rumble now strong %.2f weak %.2f", p->name, F.rum_now[pl][0] / 65535.0, F.rum_now[pl][1] / 65535.0);
        }
    }
    if (title && !F.look_cap) SDL_SetWindowTitle(F.win, t);
    if (log) F.check_log_ns = now;
    if (title) F.check_title_ns = now;
}
static void check_test(int kind) {   // F5 strong motor, F6 weak motor (straight to every controller), F7 player 1 through hle_haptics.c and the mix
    uint64_t now = SDL_GetTicksNS(); int any = 0;
    for (int i = 0; i < NPADS; i++) any |= F.pads[i].g && F.pads[i].rumble;
    if (!any) { LOG("pad check: no controller here can rumble (or DM_RUMBLE=0)"); return; }
    if (kind == 3) {
        if (!(dm_haptics_available() & 1)) { LOG("pad check: player 1 has no controller that rumbles"); return; }
        dm_vibration_update u[2] = {{0, 0, {0.7f, 160, 0.3f, 320}}, {0, 1, {0.3f, 160, 0.7f, 320}}}; dm_haptics_send(u, 2);
        LOG("pad check: rumble test: player 1 through the game's path (left actuator 0.7 low, right 0.7 high) for a second");
    } else {
        for (int i = 0; i < NPADS; i++) if (F.pads[i].g && F.pads[i].rumble) SDL_RumbleGamepad(F.pads[i].g, kind == 1 ? 0xc000 : 0, kind == 2 ? 0xc000 : 0, RUMBLE_MS);
        LOG("pad check: rumble test: the %s motor of every controller for a second", kind == 1 ? "strong (low-frequency, left)" : "weak (high-frequency, right)");
    }
    F.test_kind = kind; F.test_end_ns = now + CHECK_TEST_NS;
}

// ---------- Focus ----------
static void input_release(void) {
    F.input_on = 0;
    look_capture(0); F.look_btn = 0;
    memset(&F.kbd, 0, sizeof F.kbd); push();
    if (F.mouse_touch) { F.mouse_touch = 0; dm_touch_set(NULL, 0); }
    if (F.split) return;   // split screen: the controllers play on (the other window has the focus)
    for (int i = 0; i < NPADS; i++) if (F.pads[i].g) { int pl = pad_player(&F.pads[i]); if (pl >= 0) { dm_pad_set_player(pl, 0, 0, 0, 0, 0); if (F.pads[i].motion) dm_motion_clear_hid(pl); } }
    dm_haptics_suspend(DM_HAPTICS_INACTIVE, 1);   // front_mac.m: no rumble while another application has the focus
}
static void input_resume(void) {
    F.input_on = 1;
    dm_haptics_suspend(DM_HAPTICS_INACTIVE, 0);
    for (int i = 0; i < NPADS; i++) if (F.pads[i].g) pad_read(&F.pads[i]);   // a button still held, a stick still deflected
}

void front_sdl_start(SDL_Window *win) {
    F.win = win; F.input_on = 1;
    front_text_window(SDL_GetPointerProperty(SDL_GetWindowProperties(win), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL));   // the software keyboard's box goes over it
    snprintf(F.title, sizeof F.title, "%s", SDL_GetWindowTitle(win) ? SDL_GetWindowTitle(win) : "");
    const char *e = getenv("DM_PAD_MERGE"); F.merge = e && atoi(e);
    e = getenv("DM_KBD_PLAYER"); F.kbd_own = !F.merge && e && !strcmp(e, "own");
    e = getenv("DM_PAD_FACE"); F.by_label = e && !strcmp(e, "labels");
    F.inner = env_f("DM_PAD_INNER", FRONT_PAD_INNER); F.outer = env_f("DM_PAD_OUTER", FRONT_PAD_OUTER);
    if (F.inner < 0 || F.inner >= 1) F.inner = FRONT_PAD_INNER;
    if (F.outer <= F.inner || F.outer > 1) F.outer = FRONT_PAD_OUTER;
    F.sens = dm_look_sens(FRONT_LOOK_SENS);
    e = getenv("DM_RUMBLE"); F.rumble_on = !(e && *e && !atoi(e));
    F.rumble_gain = env_f("DM_RUMBLE_GAIN", 1); if (!(F.rumble_gain >= 0)) F.rumble_gain = 1; if (F.rumble_gain > 3) F.rumble_gain = 3;
    F.rumble_min = env_f("DM_RUMBLE_MIN", 0); if (!(F.rumble_min >= 0)) F.rumble_min = 0; if (F.rumble_min > 0.9f) F.rumble_min = 0.9f;
    e = getenv("DM_MOTION"); F.motion_on = !(e && *e && !atoi(e));
    e = getenv("DM_PAD_CHECK"); F.check = e && atoi(e);
    e = getenv("DM_SPLIT"); F.split = e && (atoi(e) == 1 || atoi(e) == 2) ? atoi(e) : 0;
    if (F.split) { F.merge = 0; F.kbd_own = 0; e = getenv("DM_SPLIT_PAD"); F.split_pad = e && *e ? atoi(e) : F.split; if (F.split_pad < 0 || F.split_pad > NPADS) F.split_pad = F.split; }
    InitializeSRWLock(&F.rlk); InitializeSRWLock(&F.klk);
    F.ev_rumble = SDL_RegisterEvents(1); F.ev_menu = SDL_RegisterEvents(1); F.ev_p1 = SDL_RegisterEvents(1);
    dm_haptics_init(rumble_sink, F.rumble_on);   // the game sees vibration devices once a controller that rumbles is a player's (rumble_route)
    if (F.kbd_own) pad_slot_take(&F.slots, &g_kbd_token, "keyboard");   // holds player 1 for good
    keymap_load();
    mappings_load();   // before the controllers are opened: a mapping can make a device a controller
    vpads_attach();
    int nj = 0; SDL_JoystickID *js = SDL_GetJoysticks(&nj);
    for (int i = 0; js && i < nj; i++) guess_mapping(js[i]);
    SDL_free(js);
    if (dm_look_on()) LOG("front end: mouse look available: click the picture to capture the mouse (left A, right B, middle right-stick press), Esc releases it "
                          "(%.3f degrees per count, DM_LOOK_SENS / DM_LOOK_SCALE)", F.sens);
    if (F.split) {
        char pc[48]; if (F.split_pad) snprintf(pc, sizeof pc, "the controller in place %d", F.split_pad); else snprintf(pc, sizeof pc, "no controller");
        LOG("front end: split screen, window %d: player 1 plays %s%s; controllers play while the window is out of focus", F.split, pc,
            F.split == 1 ? " and the keyboard and mouse" : " (the keyboard and mouse are window 1's)");
    }
    players_note();
    LOG("front end: controllers: %s; sticks' dead zones %.2f inner, %.2f outer; rumble %s (gain %.2f, minimum %.2f), motion %s%s", F.merge ? "every controller is player 1 (DM_PAD_MERGE)"
        : F.kbd_own ? "the keyboard is player 1, each controller a player of its own from player 2 on (DM_KBD_PLAYER=own)" : "each controller is a player of its own, the keyboard plays with player 1",
        F.inner, F.outer, F.rumble_on ? "on" : "off", F.rumble_gain, F.rumble_min, F.motion_on ? "on" : "off", F.check ? "; the controller check is on (DM_PAD_CHECK)" : "");
    int n = 0; SDL_JoystickID *ids = SDL_GetGamepads(&n);   // those already connected (an ADDED event follows for each too; pad_added ignores repeats)
    for (int i = 0; ids && i < n; i++) pad_added(ids[i]);
    SDL_free(ids);
    if (F.check && !n) LOG("pad check: no controller connected yet: plug one in (or switch it on) and it is logged here");
    menu_init();
}

void front_sdl_tick(void) {
    if (F.ev_rumble) { int run = atomic_load(&F.rumble_pending); for (int p = 0; p < DM_PLAYERS && !run; p++) run = F.rum_now[p][0] || F.rum_now[p][1]; if (run) rumble_apply(); }
    if (F.check) check_tick(SDL_GetTicksNS());
}

void front_sdl_event(const SDL_Event *e) {
    if (F.ev_rumble && e->type == F.ev_rumble) { rumble_apply(); return; }
    if (F.ev_menu && e->type == F.ev_menu) {
        if (e->user.code) input_release(); else if (SDL_GetWindowFlags(F.win) & SDL_WINDOW_INPUT_FOCUS) input_resume();
        return;
    }
    if (F.ev_p1 && e->type == F.ev_p1) {   // the in-game menu's choice: made player 1 and remembered (the menu writes it to controls.json)
        fpad *p = pad_by_id((SDL_JoystickID)e->user.code); if (!p) return;
        AcquireSRWLockExclusive(&F.klk); snprintf(F.p1_guid, sizeof F.p1_guid, "%s", p->guid); front_name_clean(p->name, F.p1_name, sizeof F.p1_name); ReleaseSRWLockExclusive(&F.klk);
        if (pad_player(p) == 0) LOG("front end: controller '%s' stays player 1 (chosen in the in-game menu)", p->name);
        else pad_first(p, "chosen in the in-game menu");
        return;
    }
    if (e->type == SDL_EVENT_JOYSTICK_ADDED) { guess_mapping(e->jdevice.which); return; }
    // The in-game menu first: what it takes the game never sees, and while it is open the game sees nothing but controllers coming and going and keys and
    // mouse buttons let go (nothing stays held)
    if (menu_event(e)) return;
    if (menu_is_open() && e->type != SDL_EVENT_GAMEPAD_ADDED && e->type != SDL_EVENT_GAMEPAD_REMOVED && e->type != SDL_EVENT_WINDOW_FOCUS_LOST &&
        e->type != SDL_EVENT_KEY_UP && e->type != SDL_EVENT_MOUSE_BUTTON_UP) return;
    switch (e->type) {
    case SDL_EVENT_WINDOW_FOCUS_LOST: input_release(); break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED: input_resume(); break;
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: {
        if (!F.input_on || e->key.repeat || F.split == 2) break;
        int down = e->type == SDL_EVENT_KEY_DOWN;
        if (e->key.scancode == SDL_SCANCODE_ESCAPE) { if (down && F.look_cap) { look_capture(0); F.look_btn = 0; push(); } break; }   // Esc lets the mouse go
        if (down && (e->key.mod & SDL_KMOD_ALT)) break;   // Alt+Enter is the window's (fullscreen), Alt+Tab the system's
        if (F.check && down && e->key.scancode >= SDL_SCANCODE_F5 && e->key.scancode <= SDL_SCANCODE_F7) { check_test(e->key.scancode - SDL_SCANCODE_F5 + 1); break; }
#ifndef _WIN32   // a name box waiting in the game window (no dialog could show: text_input_linux.c): the keys of A, B and + answer it
        if (down) { AcquireSRWLockShared(&F.klk); uint64_t nb = front_keymap_button(&F.keymap, e->key.scancode); ReleaseSRWLockShared(&F.klk);
                    if (front_text_pad(nb)) break; }
#endif
        AcquireSRWLockShared(&F.klk); int hit = front_kbd_key_map(&F.kbd, &F.keymap, e->key.scancode, down); ReleaseSRWLockShared(&F.klk);
        if (hit) push();
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (!F.input_on || F.split == 2) break;
        if (dm_look_on()) {   // pointer look: the first click captures, then the buttons play
            if (!F.look_cap) { if (e->button.button == SDL_BUTTON_LEFT) look_capture(1); }
            else look_button(e->button.button == SDL_BUTTON_LEFT ? DM_BTN_A : e->button.button == SDL_BUTTON_RIGHT ? DM_BTN_B : e->button.button == SDL_BUTTON_MIDDLE ? DM_BTN_RS : 0, 1);
        } else if (e->button.button == SDL_BUTTON_LEFT) touch_at(e->button.x, e->button.y);
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        uint64_t bit = e->button.button == SDL_BUTTON_LEFT ? DM_BTN_A : e->button.button == SDL_BUTTON_RIGHT ? DM_BTN_B : e->button.button == SDL_BUTTON_MIDDLE ? DM_BTN_RS : 0;
        if (F.look_btn & bit) look_button(bit, 0);
        if (e->button.button == SDL_BUTTON_LEFT && F.mouse_touch) { F.mouse_touch = 0; dm_touch_set(NULL, 0); }
        break;
    }
    case SDL_EVENT_MOUSE_MOTION:
        if (!F.input_on || F.split == 2) break;
        if (F.look_cap) {   // SDL's relative deltas are positive to the right and down
            Uint64 now = SDL_GetTicksNS(); double age = now > e->motion.timestamp ? (now - e->motion.timestamp) / 1e9 : 0;
            dm_look_add_at(e->motion.xrel * F.sens, -e->motion.yrel * F.sens, age);
        } else if (F.mouse_touch) touch_at(e->motion.x, e->motion.y);
        break;
    case SDL_EVENT_GAMEPAD_ADDED: pad_added(e->gdevice.which); break;
    case SDL_EVENT_GAMEPAD_REMOVED: pad_removed(e->gdevice.which); break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        fpad *p = pad_by_id(e->gbutton.which);
        if (p && e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && front_text_pad(pad_buttons(p, (uint8_t[FRONT_INS]){0}))) break;   // the name box is up and has the focus: a player without a keyboard accepts it here
        if (p) pad_read(p);
        break; }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: { fpad *p = pad_by_id(e->gaxis.which); if (p) pad_read(p); break; }
    case SDL_EVENT_GAMEPAD_REMAPPED: { fpad *p = pad_by_id(e->gdevice.which); if (p) pad_read(p); break; }
    case SDL_EVENT_GAMEPAD_SENSOR_UPDATE: pad_sensor(&e->gsensor); break;
    default: break;
    }
}
