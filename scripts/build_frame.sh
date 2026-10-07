#!/bin/sh
# Builds the Steam Frame's driver and installer: out/QuestLHSync-frame-<version>.tar.gz (lhsyncd and the
# questlhsync_frame SteamVR driver, frame/build.py) and out/QuestLHSync-frame-installer-<version>.flatpak (the installer
# app, frame/installer/build.py). Run it through build.sh (build.sh --frame), on the Steam Frame itself or another arm64
# Linux: both build for the machine they run on, so anywhere else the result is for the wrong CPU. The Flatpak needs
# flatpak-builder and the GNOME SDK: flatpak install --user flathub org.flatpak.Builder org.gnome.Sdk//50
set -e
cd "$(dirname "$0")/.."

case "$(uname -s)/$(uname -m)" in
  Linux/aarch64|Linux/arm64) ;;
  *)
    echo "build.sh --frame: run this on the Steam Frame (or another arm64 Linux); this is $(uname -s)/$(uname -m)." >&2
    echo "The package alone can be cross-compiled elsewhere with Zig: python3 frame/build.py" >&2
    exit 1
    ;;
esac

python3 frame/build.py
python3 frame/installer/build.py
