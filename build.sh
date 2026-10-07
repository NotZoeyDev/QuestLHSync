#!/bin/sh
# Linux builds, one entry point (the Windows ones are build.bat). The scripts it runs are in scripts/.
#   ./build.sh                   the SteamVR driver and its dashboard app (same as --driver)
#   ./build.sh --driver          driver/questlhsync/bin/linux64/: driver_questlhsync.so and the dashboard app QuestLHSync
#   ./build.sh --installer       out/QuestLHSync-steamvr-installer.AppImage (builds the driver first)
#   ./build.sh --wivrn           out/questlhsync-xr and out/QuestLHSync-wivrn.AppImage (WiVRn/Monado; no driver needed)
#   ./build.sh --all             the three above, the driver once
#   ./build.sh --frame           the Steam Frame's driver and installer: out/QuestLHSync-frame-<version>.tar.gz and
#                                out/QuestLHSync-frame-installer-<version>.flatpak. Run it ON the Steam Frame (or another
#                                arm64 Linux): it builds for the machine it runs on, so it refuses anywhere else. Not in --all.
# Flags combine: ./build.sh --installer --wivrn. QLHS_HOST_BUILD=1 builds with the host's compiler, not Valve's SDK image.
set -e
cd "$(dirname "$0")"

usage() {
  sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'
}

driver=0 installer=0 wivrn=0 frame=0
[ $# -eq 0 ] && driver=1
for a in "$@"; do
  case "$a" in
    --driver) driver=1 ;;
    --installer) installer=1 ;;
    --wivrn) wivrn=1 ;;
    --frame) frame=1 ;;
    --all) driver=1 installer=1 wivrn=1 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "build.sh: unknown option $a" >&2; usage >&2; exit 2 ;;
  esac
done

# the Frame builds nothing the others share, and refuses a machine that isn't arm64: check it before spending time on the rest
if [ $frame -eq 1 ]; then
  case "$(uname -s)/$(uname -m)" in
    Linux/aarch64|Linux/arm64) ;;
    *) scripts/build_frame.sh; exit 1 ;;  # says why
  esac
fi

# the SteamVR installer builds the driver itself unless it was just built here
if [ $driver -eq 1 ]; then
  scripts/build_driver.sh
  export QLHS_DRIVER_BUILT=1
fi
[ $installer -eq 1 ] && scripts/build_steamvr.sh
[ $wivrn -eq 1 ] && scripts/build_wivrn.sh
[ $frame -eq 1 ] && scripts/build_frame.sh
exit 0
