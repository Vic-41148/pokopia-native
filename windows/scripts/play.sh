#!/bin/bash
# play.sh: plays Pokemon Pokopia with nxrt on Linux (docs/LINUX-PLAN.md), the counterpart of play.ps1, with the same switches:
#   play.sh --exefs <ExeFS folder> --game <game.so> --romfs <RomFS.img> --save <save folder> [--work <folder>] [--silent] [--fullscreen] [--window 1600x900]
#           [--exe <nxrt>] [--link-play] [--join <IPv4[:port]>|ask] [--internet [--room <code>|ask] [--relay <IPv4[:port]>]] [--interact] [--story] [--ask] [--pad-check]
#           [--dry-run]
#   play.sh --config <play.json> [any switch above]: an installer build's launcher; paths come from play.json (relative ones from its folder, $VARIABLES
#           and ~ expanded); a switch given here wins over the file. A save folder named only by the file is created empty the first time.
# --save is the save the game plays on and writes to (B00 / G00 / S00; the game commits through <save>.work beside it): point it at a copy, never your only
# save. --work holds the shader and pipeline cache, temp files and the logs (default ${XDG_CACHE_HOME:-~/.cache}/pokopia-native). nxrt runs a few nice steps down.
# Link Play (docs/coop-step1c.md, coop-step1d.md): Linux finds no hosts on its own yet, so playing with someone on this network is "join by address"
# (--join <the host's address>) or hosting (--link-play: others join this PC). --internet plays across the internet with a room code (--room), --relay through a
# relay you trust. With --config (or --ask) and no Link Play switch, a choice comes first (zenity or kdialog when the desktop has one, else this terminal),
# remembered in ${XDG_CONFIG_HOME:-~/.config}/pokopia-native/linkplay.json; POKOPIA_ASK=1 brings it back after "don't ask again".
# --interact turns on "Interact everywhere" (experimental, docs/coop-interact.md): Link Play visitors can use moves and items and build in every town, not
# only Palette Town. Every player in a session needs it on. --story turns on "Story co-op" (experimental, docs/story-coop.md) on top of it: a visitor
# talks to the town's NPCs as the host does; every player needs it on too. The keyboard's keys: ${XDG_CONFIG_HOME:-~/.config}/pokopia-native/controls.json,
# as the Windows window's Controls tab writes it (an action's SDL scancode; front_map.h), handed to the game as DM_KEYMAP. The picture: display.json beside it
# (console mode, the size the game draws at, window or full screen, upscaling, 30 fps, vsync, the FPS counter), applied with --config or --ask, a switch or
# variable from outside winning. In the game, F1 (or a controller's Minus + Plus) changes the same settings and saves them there (docs/in-game-menu.md).
set -euo pipefail
die() { echo "play.sh: $*" >&2; exit 2; }
EXEFS= GAME= ROMFS= SAVE= WORK= EXE= CONFIG= SHADERS= SHADER_BUILD= SHADER_TOOL= PYTHON= INSTALLER= SPIRV_TOOLS= FONTS= SEED= WINDOW=
SILENT= FULLSCREEN= ASK= LINKPLAY= JOIN= INTERNET= ROOM= RELAY= PADCHECK= DRYRUN= NEWSAVE= INTERACT= STORY= NOSAVE= mode=
while [ $# -gt 0 ]; do
  a=$1; shift
  case $a in
    --exefs) EXEFS=$1; shift ;; --game|--dll) GAME=$1; shift ;; --romfs) ROMFS=$1; shift ;; --save) SAVE=$1; shift ;; --work) WORK=$1; shift ;;
    --exe) EXE=$1; shift ;; --config) CONFIG=$1; shift ;; --shaders) SHADERS=$1; shift ;; --shader-build) SHADER_BUILD=$1; shift ;;
    --shader-tool) SHADER_TOOL=$1; shift ;; --python) PYTHON=$1; shift ;; --installer) INSTALLER=$1; shift ;; --spirv-tools) SPIRV_TOOLS=$1; shift ;;
    --fonts) FONTS=$1; shift ;; --pipeline-seed) SEED=$1; shift ;; --window) WINDOW=$1; shift ;;
    --silent) SILENT=1 ;; --fullscreen) FULLSCREEN=1 ;; --ask) ASK=1 ;; --link-play) LINKPLAY=1 ;; --join) JOIN=$1; shift ;;
    --internet) INTERNET=1 ;; --room) ROOM=$1; shift ;; --relay) RELAY=$1; shift ;; --pad-check) PADCHECK=1 ;; --dry-run) DRYRUN=1 ;; --interact) INTERACT=1 ;;
    --story) STORY=1 ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    *) die "unknown switch $a (play.sh --help)" ;;
  esac
done
HERE=$(cd "$(dirname "$0")" && pwd)
PY=$(command -v python3.12 || command -v python3 || true)
[ -n "${POKOPIA_PY:-}" ] && [ -x "$POKOPIA_PY" ] && PY=$POKOPIA_PY   # the launcher's: the builder's own Python (the AppImage's)
if [ -n "$CONFIG" ]; then
  [ -f "$CONFIG" ] || die "not found: $CONFIG"
  [ -n "$PY" ] || die "python3 is needed to read $CONFIG"
  # play.json's keys as shell assignments, each path made absolute against the file's folder
  eval "$("$PY" -I - "$CONFIG" <<'EOF'
import json, os, shlex, sys
cfg = sys.argv[1]; d = os.path.dirname(os.path.abspath(cfg)); c = json.load(open(cfg, encoding='utf-8'))
def p(v):
    if not v: return ''
    v = os.path.expanduser(os.path.expandvars(v))
    return v if os.path.isabs(v) else os.path.normpath(os.path.join(d, v))
for key, var in (('exefs', 'C_EXEFS'), ('game', 'C_GAME'), ('romfs', 'C_ROMFS'), ('save', 'C_SAVE'), ('work', 'C_WORK'), ('exe', 'C_EXE'), ('shaders', 'C_SHADERS'),
                 ('shader_build', 'C_SHADER_BUILD'), ('shader_tool', 'C_SHADER_TOOL'), ('python', 'C_PYTHON'), ('installer', 'C_INSTALLER'),
                 ('spirv_tools', 'C_SPIRV_TOOLS'), ('fonts', 'C_FONTS'), ('pipeline_seed', 'C_SEED'), ('dotnet', 'C_DOTNET'), ('dlc', 'C_DLC')):
    print(f'{var}={shlex.quote(p(c.get(key)))}')
EOF
)"
  : "${EXEFS:=$C_EXEFS}" "${GAME:=$C_GAME}" "${ROMFS:=$C_ROMFS}" "${WORK:=$C_WORK}" "${EXE:=$C_EXE}" "${SHADERS:=$C_SHADERS}"
  : "${SHADER_BUILD:=$C_SHADER_BUILD}" "${SHADER_TOOL:=$C_SHADER_TOOL}" "${PYTHON:=$C_PYTHON}" "${INSTALLER:=$C_INSTALLER}" "${SPIRV_TOOLS:=$C_SPIRV_TOOLS}"
  : "${FONTS:=$C_FONTS}" "${SEED:=$C_SEED}" "${DLC:=$C_DLC}"
  [ -z "$SAVE" ] && SAVE=$C_SAVE && NEWSAVE=1
  # the .NET the build used (the AppImage's own, or ~/.dotnet): the shader translator runs on it for shaders the game makes in play
  [ -n "$C_DOTNET" ] && [ -x "$C_DOTNET/dotnet" ] && [ -z "${DOTNET_ROOT:-}" ] && export DOTNET_ROOT=$C_DOTNET PATH=$C_DOTNET:$PATH
fi
for n in EXEFS GAME ROMFS SAVE; do [ -n "${!n}" ] || die "--$(echo "$n" | tr 'A-Z' 'a-z') is needed (or --config <play.json>)"; done
if [ -z "$EXE" ]; then
  for c in "$HERE/nxrt" "$HERE/../build/linux-Release/bin/nxrt" "$HOME/pkn-linux/build/linux-Release/bin/nxrt"; do [ -x "$c" ] && EXE=$c && break; done
  [ -n "$EXE" ] || die "nxrt not found: give --exe"
fi
WORK=${WORK:-${XDG_CACHE_HOME:-$HOME/.cache}/pokopia-native}
case $WORK in /*) ;; *) WORK=$PWD/$WORK ;; esac   # nxrt starts in $WORK: its paths must not be relative
if [ -n "$NEWSAVE" ] && [ ! -d "$SAVE" ] && [ -z "$DRYRUN" ]; then mkdir -p "$SAVE"; echo "a new save folder: $SAVE"; fi
for p in "$EXEFS" "$GAME" "$ROMFS" "$EXE"; do [ -e "$p" ] || die "not found: $p"; done
[ -n "$NEWSAVE" ] && [ -n "$DRYRUN" ] || [ -d "$SAVE" ] || die "not found: $SAVE"
for m in main sdk; do [ -f "$EXEFS/$m" ] || die "$EXEFS has no '$m': give the game's ExeFS folder"; done
ls -d "$SAVE"/[BGS]00 >/dev/null 2>&1 || { NOSAVE=1; echo "play.sh: $SAVE holds no B00 / G00 / S00 folders: the game will start a new save there" >&2; }
# Interact everywhere and Story co-op have sites in these game versions only (src/coop/coop_interact.c, as play.ps1): main's build ID is the module
# ID in its NSO header
COOP_BUILDS="76a489f3bd3642fa5e18bd83d32c98ba=2.0.1 816c5358e8178d802fcec4a7ce5a741a=1.0.5" COOP_NEEDS="Pokopia 2.0.1 or 1.0.5" COOP_OK=
MAIN_BUILD=$(od -An -tx1 -j 64 -N 16 "$EXEFS/main" 2>/dev/null | tr -d ' \n' || true)
for b in $COOP_BUILDS; do [ "${b%%=*}" = "$MAIN_BUILD" ] && COOP_OK=1; done
COOP_GAME=$([ -n "$MAIN_BUILD" ] && echo "main ${MAIN_BUILD:0:16}" || echo "main unreadable")

IPPORT='^((25[0-5]|2[0-4][0-9]|1?[0-9]?[0-9])\.){3}(25[0-5]|2[0-4][0-9]|1?[0-9]?[0-9])(:[0-9]{1,5})?$'
ROOMRX='^[0-9A-TV-Za-tv-z]{16}$'   # after dashes and spaces are taken out: Crockford's base32
SETTINGS=${POKOPIA_SETTINGS_DIR:-${XDG_CONFIG_HOME:-$HOME/.config}/pokopia-native}   # POKOPIA_SETTINGS_DIR: another folder (tests)
export NXRT_SETTINGS_DIR=$SETTINGS   # the in-game menu (F1) saves its changes there too
PREF=$SETTINGS/linkplay.json
pref_get() { [ -f "$PREF" ] && "$PY" -I -c 'import json,sys; v=json.load(open(sys.argv[1])).get(sys.argv[2]); print("" if v is None else (str(v).lower() if isinstance(v,bool) else v))' "$PREF" "$1" 2>/dev/null || true; }
pref_put() {   # key=value ...
  mkdir -p "$(dirname "$PREF")"
  "$PY" -I - "$PREF" "$@" <<'EOF' || true
import json, os, sys
f = sys.argv[1]; d = json.load(open(f)) if os.path.isfile(f) else {}
for kv in sys.argv[2:]:
    k, v = kv.split('=', 1); d[k] = v == 'true' if v in ('true', 'false') else v
json.dump(d, open(f, 'w'), indent=1)
EOF
}
ask_text() {   # title prompt default -> the answer on stdout (empty: cancelled)
  if [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] && command -v zenity >/dev/null; then zenity --entry --title "$1" --text "$2" --entry-text "$3" 2>/dev/null || true
  elif [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] && command -v kdialog >/dev/null; then kdialog --title "$1" --inputbox "$2" "$3" 2>/dev/null || true
  else read -r -p "$2 [$3]: " r; echo "${r:-$3}"; fi
}
ask_yes() {   # title text default(yes/no) -> yes or no
  if [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] && command -v zenity >/dev/null; then
    zenity --question --title "$1" --text "$2" --ok-label Yes --cancel-label No 2>/dev/null && echo yes || echo no
  elif [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] && command -v kdialog >/dev/null; then kdialog --title "$1" --yesno "$2" 2>/dev/null && echo yes || echo no
  else read -r -p "$2 (yes / no) [$3]: " r; r=${r:-$3}; [ "${r,,}" = yes ] || [ "${r,,}" = y ] && echo yes || echo no; fi
}
choose_mode() {   # the last mode -> the new one (alone, host, join, inet-host, inet-visit) or empty (cancelled)
  local text="How do you want to play? In the game: Link Play, then Local connection."
  [ -n "$NOSAVE" ] && text="No save here yet, so the game starts a new one. To play your save from the phone, a Mac or another PC, cancel and run"$'\n'"nxs2.py import-save first."$'\n\n'"$text"
  local last=$1 items=("alone" "Play alone" "host" "Link Play: host here (players on this network join this computer's address)"
                       "join" "Link Play: join a host on this network by its address" "inet-host" "Host over the internet (your router forwards port 47600)"
                       "inet-visit" "Visit a host over the internet")
  if [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] && command -v zenity >/dev/null; then
    local rows=() i; for ((i = 0; i < ${#items[@]}; i += 2)); do rows+=("$([ "${items[i]}" = "$last" ] && echo TRUE || echo FALSE)" "${items[i]}" "${items[i+1]}"); done
    zenity --list --radiolist --title "Pokopia - Link Play" --text "$text" \
      --column "" --column "mode" --column "" --hide-column 2 --print-column 2 --width 640 --height 330 "${rows[@]}" 2>/dev/null || true
  elif [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] && command -v kdialog >/dev/null; then
    local rows=() i; for ((i = 0; i < ${#items[@]}; i += 2)); do rows+=("${items[i]}" "${items[i+1]}" "$([ "${items[i]}" = "$last" ] && echo on || echo off)"); done
    kdialog --title "Pokopia - Link Play" --radiolist "$text" "${rows[@]}" 2>/dev/null || true
  else
    local i n=1; echo "$text" >&2
    for ((i = 0; i < ${#items[@]}; i += 2)); do echo "  $n) ${items[i+1]}" >&2; n=$((n + 1)); done
    read -r -p "Choose 1-5 [alone]: " r; case ${r:-1} in 1) echo alone ;; 2) echo host ;; 3) echo join ;; 4) echo inet-host ;; 5) echo inet-visit ;; esac
  fi
}

if { [ -n "$CONFIG" ] || [ -n "$ASK" ]; } && [ -z "$LINKPLAY$JOIN$INTERNET$ROOM$RELAY${DM_COOP:-}$DRYRUN" ] && [ -n "$PY" ]; then
  mode=$(pref_get mode); mode=${mode:-alone}
  GAMEMODE=; { [ -n "${GAMESCOPE_WAYLAND_DISPLAY:-}" ] || [ "${XDG_CURRENT_DESKTOP:-}" = gamescope ]; } && GAMEMODE=1   # the Steam Deck's Game Mode: no dialogs
  if { [ "$(pref_get ask)" != "false" ] || [ -n "${POKOPIA_ASK:-}" ]; } && [ -z "$GAMEMODE" ]; then
    mode=$(choose_mode "$mode"); [ -n "$mode" ] || { echo "cancelled"; exit 0; }
    if [ "$mode" = join ] || [ "$mode" = inet-visit ]; then
      JOIN=$(ask_text "Pokopia - Link Play" "The host's address (for example 192.168.1.20)" "$(pref_get join)"); [ -n "$JOIN" ] || { echo "cancelled"; exit 0; }
      pref_put "join=$JOIN"
    fi
    if [ "$mode" = inet-visit ]; then
      ROOM=$(ask_text "Pokopia - Link Play" "The host's room code (XXXX-XXXX-XXXX-XXXX)" "$(pref_get room)"); [ -n "$ROOM" ] || { echo "cancelled"; exit 0; }
      pref_put "room=$ROOM"
    fi
    if [ "$mode" != alone ] && [ -z "$COOP_OK" ]; then   # another game version: neither is asked for (the choices stay remembered for one that has them)
      echo "Interact everywhere and Story co-op need $COOP_NEEDS. This game is another version ($COOP_GAME), so they stay off."
    elif [ "$mode" != alone ] && [ -z "$INTERACT" ]; then
      ie=$(ask_yes "Pokopia - Link Play" "Interact everywhere (experimental)? Visitors can use moves and items and build in every town, not only Palette Town. Every player needs it on." \
        "$([ "$(pref_get interact)" = true ] && echo yes || echo no)")
      pref_put "interact=$([ "$ie" = yes ] && echo true || echo false)"; [ "$ie" = yes ] && INTERACT=1
    fi
    if [ -n "$COOP_OK" ] && [ -n "$INTERACT" ] && [ -z "$STORY" ]; then
      sc=$(ask_yes "Pokopia - Link Play" "Story co-op (experimental)? Visitors can talk to the town's story characters. Story requests still count only for the host. Every player needs it on." \
        "$([ "$(pref_get story)" = true ] && echo yes || echo no)")
      pref_put "story=$([ "$sc" = yes ] && echo true || echo false)"; [ "$sc" = yes ] && STORY=1
    fi
    again=$(ask_text "Pokopia - Link Play" "Ask again next time? (yes / no; POKOPIA_ASK=1 brings this back)" "yes")
    pref_put "mode=$mode" "ask=$([ "${again,,}" = no ] && echo false || echo true)"
  else
    JOIN=$(pref_get join); ROOM=$(pref_get room)
    [ "$mode" != alone ] && [ "$(pref_get interact)" = true ] && INTERACT=1
    [ "$mode" != alone ] && [ "$(pref_get story)" = true ] && STORY=1
  fi
  case $mode in
    alone) JOIN= ROOM= ;; host) LINKPLAY=1; JOIN= ROOM= ;; join) LINKPLAY=1; ROOM= ;; inet-host) INTERNET=1; JOIN= ROOM= ;; inet-visit) INTERNET=1 ;;
  esac
fi
[ -n "$JOIN$INTERNET$ROOM$RELAY" ] && LINKPLAY=1
[ -n "$STORY" ] && INTERACT=1   # Story co-op needs Interact everywhere
[ -z "$INTERACT" ] || [ -n "$LINKPLAY" ] || die "--interact$([ -n "$STORY" ] && echo ' (and --story)') goes with Link Play (--link-play, --join or --internet)"
if [ -n "$LINKPLAY" ]; then
  [ "$JOIN" = ask ] && { read -r -p "The host's address (for example 203.0.113.7, or 192.168.1.20:47600): " JOIN; [ -n "$JOIN" ] || die "no address given"; }
  [ -z "$JOIN" ] || [[ $JOIN =~ $IPPORT ]] || die "--join wants the host's IPv4 address, for example 203.0.113.7 or 192.168.1.20:47600 (got '$JOIN')"
  [ "$ROOM" = ask ] && { read -r -p "The host's room code (XXXX-XXXX-XXXX-XXXX): " ROOM; [ -n "$ROOM" ] || die "no room code given"; }
  r=${ROOM//[- ]/}; [ -z "$ROOM" ] || [[ $r =~ $ROOMRX ]] || die "--room wants 16 letters and digits, XXXX-XXXX-XXXX-XXXX (got '$ROOM')"
  [ -z "$RELAY" ] || [ -n "$INTERNET" ] || die "--relay goes with --internet"
  [ -z "$RELAY" ] || [[ $RELAY =~ $IPPORT ]] || die "--relay wants the relay's IPv4 address[:port] (got '$RELAY')"
  [ -n "$INTERNET" ] && [ -n "$JOIN" ] && [ -z "$ROOM" ] && die "visiting over the internet needs the host's room code: --room XXXX-XXXX-XXXX-XXXX (or --room ask)"
  if [ -n "$INTERNET" ] && [ -z "$ROOM" ]; then   # this computer hosts: a new code for this session (256 is a multiple of 32: no bias)
    abc=0123456789ABCDEFGHJKMNPQRSTVWXYZ; s=
    for b in $(od -An -tu1 -N16 /dev/urandom); do s+=${abc:$((b % 32)):1}; done
    ROOM="${s:0:4}-${s:4:4}-${s:8:4}-${s:12:4}"
  fi
  export DM_COOP=auto
  [ -n "$JOIN" ] && export DM_COOP_JOIN=$JOIN
  [ -n "$INTERNET" ] && export DM_COOP_INTERNET=1 DM_COOP_ROOM=${ROOM^^}
  [ -n "$RELAY" ] && export DM_COOP_RELAY=$RELAY
  echo
  [ -n "$INTERACT" ] && export DM_COOP_INTERACT=1
  [ -n "$STORY" ] && export DM_COOP_STORY=1
  if [ -n "$INTERACT" ] && [ -z "$COOP_OK" ]; then
    echo "play.sh: Interact everywhere$([ -n "$STORY" ] && echo ' and Story co-op') need $COOP_NEEDS; this game is another version ($COOP_GAME), so they stay off" >&2
  elif [ -n "$INTERACT" ]; then
    echo "Interact everywhere: on (experimental). Every player needs it on; a session refuses a player whose setting differs."
    [ -n "$STORY" ] && echo "Story co-op: on (experimental). Every player needs it on as well."
  fi
  if [ -n "$JOIN" ]; then echo "Link Play: visiting $JOIN. In the game: Link Play, Local connection, then look for a town."
  else echo "Link Play: on. In the game: Link Play, Local connection, to host. Other players join this computer's address (ip -4 addr shows it)."; fi
  if [ -n "$INTERNET" ]; then
    echo "Internet play, room code: $DM_COOP_ROOM"
    if [ -n "$RELAY" ]; then echo "Through the relay $RELAY: every player needs this code and the relay's address."
    elif [ -z "$JOIN" ]; then echo "Visitors need this code and this computer's internet address; your router must forward port 47600 (TCP and UDP) here."; fi
    echo "Nothing is encrypted: anyone on the way can read the game's traffic."
    if [ -z "$JOIN" ] && [ -z "$DRYRUN" ] && [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] && command -v zenity >/dev/null; then
      zenity --info --title "Pokopia - your room code" --no-wrap --text "Your room code for this session:\n\n<big><b>$DM_COOP_ROOM</b></big>\n\nGive it to the players who visit." 2>/dev/null || true
    fi
  fi
  echo
  [ "$mode" = inet-host ] 2>/dev/null && pref_put "host_room=$DM_COOP_ROOM"
fi

CACHE=$WORK/cache TEMP=$WORK/temp LOGS=$WORK/logs
[ -n "$DRYRUN" ] || mkdir -p "$CACHE" "$TEMP" "$LOGS"
export NXRT_VK_CACHE_DIR=$CACHE
export DM_AUDIO=$([ -n "$SILENT" ] && echo 0 || echo 1)
if [ -n "$CONFIG$ASK" ]; then   # the picture, saved for this computer (display.json); a switch or variable given from outside wins
  # A Steam Deck (Steam's flag, or Valve's LCD / OLED board) starts in the game's handheld mode, its lighter settings (24 % less GPU time measured),
  # until the player picks docked or handheld in F1 -> Graphics (display.json then holds "console")
  DECK=; [ "${SteamDeck:-}" = 1 ] && DECK=1
  case "$(cat /sys/class/dmi/id/board_vendor 2>/dev/null || true)/$(cat /sys/class/dmi/id/board_name 2>/dev/null || true)" in Valve/Jupiter|Valve/Galileo) DECK=1 ;; esac
  D_CONSOLE=$([ -n "$DECK" ] && echo 0 || echo 1) D_RES=default D_FULL=false D_WIN=1280x720 D_UPSCALE=sharp D_SHARP=0.5 D_FPS=60 D_VSYNC=true D_COUNTER=off D_PRESENT= D_ANISO=0 D_LOD=0
  [ -n "$PY" ] && eval "$("$PY" -I - "$SETTINGS/display.json" "$D_CONSOLE" <<'EOF'
import json, re, shlex, sys
d = {'console': int(sys.argv[2]), 'resolution': 'default', 'fullscreen': False, 'window': '1280x720', 'upscale': 'sharp', 'sharpness': 5, 'fps': 60, 'vsync': True,
     'present_mode': '', 'aniso': 0, 'lod_bias': 0.0, 'fps_counter': 'off'}
try: d.update({k: v for k, v in json.load(open(sys.argv[1], encoding='utf-8')).items() if k in d})
except Exception: pass
def num(v, dflt):
    try: return int(v)
    except (TypeError, ValueError): return dflt
try: lod = float(d['lod_bias'])   # -1.0 .. +1.0 (the F1 menu's Texture bias)
except (TypeError, ValueError): lod = 0.0
lod = max(-1.0, min(1.0, lod))
pm = str(d['present_mode'])   # the F1 menu's Vsync row's exact choice; the older "vsync": false was Mailbox
if pm not in ('fifo', 'mailbox', 'immediate'): pm = '' if d['vsync'] else 'mailbox'
r = str(d['resolution'])   # "default" or "<w>x<h>", kept as saved; a factor from before the sizes (1 / 1.5 / 2 / 3) was meant docked
if r != 'default' and not re.fullmatch(r'\d+x\d+', r):
    try: f = float(r)
    except ValueError: f = 0
    h = round(1080 * f); r = f'{round(h * 16 / 9)}x{h}' if f >= 1 else 'default'
for var, v in (('D_CONSOLE', num(d['console'], int(sys.argv[2]))), ('D_RES', r), ('D_FULL', 'true' if d['fullscreen'] else 'false'), ('D_WIN', d['window'] or ''),
               ('D_UPSCALE', d['upscale']), ('D_SHARP', f"{num(d['sharpness'], 5) / 10:g}"), ('D_FPS', num(d['fps'], 60)),
               ('D_VSYNC', 'true' if d['vsync'] else 'false'), ('D_PRESENT', pm), ('D_ANISO', max(0, min(16, num(d['aniso'], 0)))),
               ('D_LOD', f'{lod:g}'), ('D_COUNTER', d['fps_counter'] if d['fps_counter'] in ('off', 'corner', 'detailed', 'graph') else 'off')):
    print(f'{var}={shlex.quote(str(v))}')
EOF
)"
  [ -z "${DM_OPMODE:-}" ] && export DM_OPMODE=$D_CONSOLE
  [ -n "$DECK" ] && [ "$DM_OPMODE" = 0 ] && echo "Steam Deck: the game's handheld mode (F1 -> Graphics -> Docked switches it at the next start)."
  [ -z "${NXRT_VK_SCALE:-}${NXRT_VK_RESOLUTION:-}" ] && export NXRT_VK_RESOLUTION=$D_RES
  if [ -z "$WINDOW$FULLSCREEN${NXRT_VK_WINDOW:-}${NXRT_VK_FULLSCREEN:-}" ]; then [ "$D_FULL" = true ] && FULLSCREEN=1 || WINDOW=$D_WIN; fi
  [ -z "${NXRT_VK_UPSCALE:-}" ] && export NXRT_VK_UPSCALE=$D_UPSCALE
  [ -z "${NXRT_VK_SHARPNESS:-}" ] && export NXRT_VK_SHARPNESS=$D_SHARP
  [ -z "${DM_FPS:-}" ] && [ "$D_FPS" = 30 ] && export DM_FPS=30
  [ -z "${NXRT_FPS_UNCAPPED:-}" ] && [ "$D_FPS" = "0" ] && export NXRT_FPS_UNCAPPED=1
  [ -z "${NXRT_VK_PRESENT:-}" ] && [ -n "$D_PRESENT" ] && export NXRT_VK_PRESENT=$D_PRESENT
  [ -z "${NXRT_VK_ANISO:-}" ] && [ "$D_ANISO" != 0 ] && export NXRT_VK_ANISO=$D_ANISO
  [ -z "${NXRT_VK_LOD:-}" ] && [ "$D_LOD" != "0" ] && export NXRT_VK_LOD=$D_LOD
  [ -z "${NXRT_FPS_COUNTER:-}" ] && [ "$D_COUNTER" != off ] && export NXRT_FPS_COUNTER=$D_COUNTER
fi
[ -n "$WINDOW" ] && export NXRT_VK_WINDOW=$WINDOW
[ -n "$FULLSCREEN" ] && export NXRT_VK_FULLSCREEN=1
[ -n "$PADCHECK" ] && export DM_PAD_CHECK=1 DM_RUMBLE_LOG=1 DM_MOTION_LOG=1
if [ -n "$SHADERS" ]; then
  export NXRT_SHADER_CACHE=$SHADERS
  [ -n "$SHADER_BUILD" ] && export NXRT_SHADER_BUILD=$SHADER_BUILD
  [ -n "$SPIRV_TOOLS" ] && export NXSHADER_TOOLS=$SPIRV_TOOLS
  if [ -n "$SHADER_TOOL" ]; then
    py=$PYTHON; [ -n "$py" ] && [ -x "$py" ] || py=$PY
    if [ -n "$py" ]; then   # nxrt runs it through /bin/sh: quoted for sh
      q() { printf "'%s'" "${1//\'/\'\\\'\'}"; }
      NXRT_SHADER_TOOL="$(q "$py") -I $(q "$SHADER_TOOL") --cache $(q "$SHADERS")"
      [ -n "$INSTALLER" ] && NXRT_SHADER_TOOL+=" --installer $(q "$INSTALLER")"
      export NXRT_SHADER_TOOL
    else echo "play.sh: no Python found: shaders the game makes while it runs cannot be translated (their draws are skipped)" >&2; fi
  fi
fi
[ -n "$FONTS" ] && export DM_FONT_DIR=$FONTS
[ -n "$SEED" ] && export NXRT_VK_PIPELINE_SEED=$SEED
KEYS=$SETTINGS/controls.json
[ -z "${DM_KEYMAP:-}" ] && [ -f "$KEYS" ] && export DM_KEYMAP=$KEYS   # the keyboard's keys; the game logs the map it uses
# your own DLC: nxs2.py import-dlc copies it from your dump of the DLC you bought into the build's dlc folder, one numbered folder per add-on
# content; only a folder that is really there is passed on (DM_AOC_DIR), and without one the game owns no add-on content
if [ -n "${DLC:-}" ] && [ -d "$DLC" ] && ls "$DLC" 2>/dev/null | grep -qE '^[1-9][0-9]*$'; then export DM_AOC_DIR=$DLC; fi
abs() { case $1 in /*) echo "$1" ;; *) (cd "$(dirname "$1")" && echo "$PWD/$(basename "$1")") ;; esac; }   # a new save folder need not exist yet
ARGS=("$(abs "$EXEFS")" --dll "$(abs "$GAME")" --romfs "$(abs "$ROMFS")" --save "$(abs "$SAVE")" --temp "$TEMP" --log "$LOGS" --gfx vulkan)
if [ -n "$DRYRUN" ]; then
  echo "exe: $(abs "$EXE")"; echo "work: $WORK"; echo "args: ${ARGS[*]}"
  echo "env: NXRT_VK_CACHE_DIR=$NXRT_VK_CACHE_DIR DM_AUDIO=$DM_AUDIO DM_PAD_CHECK=${DM_PAD_CHECK:-}"
  [ -n "$SHADERS" ] && { echo "shaders: NXRT_SHADER_CACHE=$NXRT_SHADER_CACHE NXRT_SHADER_BUILD=${NXRT_SHADER_BUILD:-} NXSHADER_TOOLS=${NXSHADER_TOOLS:-}"; echo "shader tool: ${NXRT_SHADER_TOOL:-}"; }
  [ -n "$FONTS$SEED" ] && echo "fonts: ${DM_FONT_DIR:-}; pipeline seed: ${NXRT_VK_PIPELINE_SEED:-}"
  [ -n "${DM_COOP:-}" ] && echo "link play: DM_COOP=$DM_COOP DM_COOP_JOIN=${DM_COOP_JOIN:-} DM_COOP_INTERNET=${DM_COOP_INTERNET:-} DM_COOP_INTERACT=${DM_COOP_INTERACT:-} DM_COOP_STORY=${DM_COOP_STORY:-}"
  [ -n "${DM_KEYMAP:-}" ] && echo "keys: DM_KEYMAP=$DM_KEYMAP"
  echo "display: DM_OPMODE=${DM_OPMODE:-} NXRT_VK_SCALE=${NXRT_VK_SCALE:-} NXRT_VK_RESOLUTION=${NXRT_VK_RESOLUTION:-} NXRT_VK_WINDOW=${NXRT_VK_WINDOW:-}" \
       "NXRT_VK_FULLSCREEN=${NXRT_VK_FULLSCREEN:-} NXRT_VK_UPSCALE=${NXRT_VK_UPSCALE:-} NXRT_VK_SHARPNESS=${NXRT_VK_SHARPNESS:-} DM_FPS=${DM_FPS:-}" \
       "NXRT_VK_PRESENT=${NXRT_VK_PRESENT:-} NXRT_VK_ANISO=${NXRT_VK_ANISO:-} NXRT_VK_LOD=${NXRT_VK_LOD:-} NXRT_FPS_COUNTER=${NXRT_FPS_COUNTER:-} NXRT_SETTINGS_DIR=$NXRT_SETTINGS_DIR"
  exit 0
fi
echo "Pokopia: keyboard WASD move, mouse look (click the picture; Esc releases the mouse), J/K/U/I = A/B/X/Y, Return = +, Tab = -, F11 fullscreen, F1 the menu."
echo "Controllers work as Switch controllers (Steam Deck, Xbox / PlayStation / Switch Pro). Save: $SAVE. Logs: $LOGS"
cd "$WORK"
exec nice -n 2 "$(abs "$EXE")" "${ARGS[@]}"
