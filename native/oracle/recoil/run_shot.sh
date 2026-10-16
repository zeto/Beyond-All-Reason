#!/bin/bash
# Headed Recoil HUD bookmark screenshot for M7 FLIP.
# Usage: run_shot.sh --shot NAME [--frames N] [--damage FRAC] [--builddef DEF]
# Writes PNG to /tmp/bar-oracle/screenshots/NAME.png (oracle write-dir only).
set -euo pipefail

SHOT="select"
FRAMES="100"
DAMAGE="0"
BUILDDEF=""
SELECT="1"
SHOTFRAME="60"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --shot) SHOT="${2:?}"; shift 2 ;;
    --frames) FRAMES="${2:?}"; shift 2 ;;
    --damage) DAMAGE="${2:?}"; shift 2 ;;
    --builddef) BUILDDEF="${2:?}"; shift 2 ;;
    --select) SELECT="${2:?}"; shift 2 ;;
    --shotframe) SHOTFRAME="${2:?}"; shift 2 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

if pgrep -x spring >/dev/null || pgrep -x bar-metal >/dev/null; then
  echo "run_shot: spring or bar-metal already running" >&2
  exit 1
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
mkdir -p "$ROOT/games" "$MOD/luarules/gadgets" "$MOD/luaui/widgets" "$ROOT/screenshots"
cp "$REPO/native/oracle/recoil/shot_gadget.lua" \
  "$MOD/luarules/gadgets/bar_metal_oracle_shot.lua"
cp "$REPO/native/oracle/recoil/shot_camera_widget.lua" \
  "$MOD/luaui/widgets/bar_metal_oracle_camera.lua"
cat > "$MOD/modinfo.lua" <<'MODEOF'
return {
  name = "BAR Metal Oracle",
  description = "Headed HUD bookmark shots for bar-metal FLIP",
  shortname = "BARO",
  version = "v1",
  mutator = "shot",
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
HangTimeout = 60
Fullscreen = 0
WindowBorderless = 0
XResolutionWindowed = 1280
YResolutionWindowed = 720
XResolution = 1280
YResolution = 720
WindowPosX = 80
WindowPosY = 60
MacRenderScale = 1
CamSpringTrackMapHeightMode = 0
CamSpringEnabled = 1
CamFreeEnabled = 1
CamFreeGroundOffset = 0
CamFreeGravity = 0
CamMode = 4
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
		barmetal_shot=${SHOT};
		barmetal_damage=${DAMAGE};
		barmetal_builddef=${BUILDDEF};
		barmetal_select=${SELECT};
		barmetal_shotframe=${SHOTFRAME};
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
export SPRING_MAC_NO_RETINA=1
unset EGL_PLATFORM

# Headed (no -hidden). Zink+MoltenVK required for EGL on this Mac.
"$HERE/spring" -write-dir "$ROOT" -config "$ROOT/springsettings.cfg" -window -nocolor "$ROOT/boot.txt"
