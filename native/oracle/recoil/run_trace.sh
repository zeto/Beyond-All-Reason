#!/bin/bash
# Run the pinned BAR Launcher spring and echo a commander trace.
# Usage:
#   run_trace.sh [frames] [move]                         # legacy boot/move
#   run_trace.sh --scenario NAME [--frames N] [--move F:X:Z]
#                [--build F:DEF:X:Z] [--damage F:FRAC]
# The engine log is /tmp/bar-oracle/infolog.txt. Lines land in
# /tmp/bar-oracle/trace.jsonl when the unsynced IO bridge works.
set -euo pipefail

SCENARIO="boot"
FRAMES="30"
MOVE=""
BUILD=""
DAMAGE=""

if [[ "${1:-}" == "--scenario" ]]; then
  shift
  SCENARIO="${1:?scenario}"
  shift || true
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --frames) FRAMES="${2:?}"; shift 2 ;;
      --move) MOVE="${2:?}"; shift 2 ;;
      --build) BUILD="${2:?}"; shift 2 ;;
      --damage) DAMAGE="${2:?}"; shift 2 ;;
      *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
  done
else
  FRAMES="${1:-30}"
  MOVE="${2:-}"
  if [[ -n "$MOVE" ]]; then
    SCENARIO="move"
  fi
fi

ROOT="/tmp/bar-oracle"
HERE="/Applications/BAR Launcher.app/Contents/MacOS"
RES="/Applications/BAR Launcher.app/Contents/Resources"
FRAMEWORKS="/Applications/BAR Launcher.app/Contents/Frameworks"
DATA="$HOME/Library/Application Support/Beyond-All-Reason-mac"
ICD="$RES/vulkan/icd.d/moltenvk_icd.json"
MOD="/tmp/bar-oracle-mod"
REPO="$(cd "$(dirname "$0")/../../.." && pwd)"

rm -rf "$ROOT" "$MOD"
mkdir -p "$ROOT/games" "$MOD/luarules/gadgets"
cp "$REPO/native/oracle/recoil/trace_gadget.lua" \
  "$MOD/luarules/gadgets/bar_metal_oracle_trace.lua"
cat > "$MOD/modinfo.lua" <<'MODEOF'
return {
  name = "BAR Metal Oracle",
  description = "Dump a Recoil trace for bar-metal",
  shortname = "BARO",
  version = "v1",
  mutator = "trace",
  game = "Beyond All Reason",
  shortGame = "BYAR",
  modtype = 1,
  depend = {
    "Beyond All Reason test-31578-8247b61",
  },
}
MODEOF
( cd "$MOD" && /usr/bin/zip -qr "$ROOT/games/bar-metal-oracle.sdz" . )
ln -s "$DATA/maps" "$ROOT/maps"
ln -s "$DATA/packages" "$ROOT/packages"
ln -s "$DATA/pool" "$ROOT/pool"
ln -s "$DATA/rapid" "$ROOT/rapid"
cat > "$ROOT/springsettings.cfg" <<'CFGEOF'
DevLua = 1
LogFlush = 1
VSync = 0
Shadows = 0
Water = 0
MaxSounds = 0
snd_volmaster = 0
HangTimeout = 0
Fullscreen = 0
CFGEOF
cat > "$ROOT/boot.txt" <<BOOTEOF
[GAME]
{
	MapName=Supreme Isthmus v2.1;
	GameType=BAR Metal Oracle v1;
	StartPosType=0;
	GameStartDelay=0;
	IsHost=1;
	OnlyLocal=1;
	HostIP=127.0.0.1;
	HostPort=0;
	MyPlayerName=Player;
	NumPlayers=1;
	NumUsers=1;
	NoHelperAIs=1;
	[MODOPTIONS]
	{
		barmetal_frames=${FRAMES};
		barmetal_move=${MOVE};
		barmetal_scenario=${SCENARIO};
		barmetal_build=${BUILD};
		barmetal_damage=${DAMAGE};
	}
	[ALLYTEAM0]
	{
		NumAllies=0;
	}
	[TEAM0]
	{
		AllyTeam=0;
		TeamLeader=0;
		Side=Armada;
		RgbColor=0.90 0.15 0.12;
		StartPosX=7600;
		StartPosZ=4900;
	}
	[PLAYER0]
	{
		Team=0;
		Name=Player;
	}
}
BOOTEOF

export EGL_PLATFORM=surfaceless
export VK_ICD_FILENAMES="$ICD"
export VK_DRIVER_FILES="$ICD"
export GALLIUM_DRIVER=zink
export MESA_LOADER_DRIVER_OVERRIDE=zink
export MESA_GL_VERSION_OVERRIDE=4.6
export MVK_CONFIG_LOG_LEVEL=1
export MVK_CONFIG_USE_COMMAND_POOLING=0
export MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=0
export DYLD_FALLBACK_LIBRARY_PATH="$FRAMEWORKS"
export SPRING_DATADIR="$RES"
export SPRING_WRITEDIR="$ROOT"
export BAR_INFOLOG="$ROOT/infolog.txt"

exec "$HERE/spring" -write-dir "$ROOT" -config "$ROOT/springsettings.cfg" -hidden -nocolor "$ROOT/boot.txt"
