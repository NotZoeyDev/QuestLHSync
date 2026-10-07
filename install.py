"""QuestLHSync installer for a source checkout (the release zip needs no Python: see README.md).

  python install.py          PC (SteamVR closed): register driver/questlhsync with SteamVR, in place
  python install.py headset  Quest Pro, 3 or 3S over adb: install the Magisk module and start lhsyncd now (no reboot)
  python install.py remove   PC (SteamVR closed): unregister the driver (the data folder is kept)
"""
import json
import os
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "magisk"))
from build_module import VERSION  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
DRIVER = os.path.join(HERE, "driver", "questlhsync")
MODULE = os.path.join(HERE, "out", f"QuestLHSync-quest-module-{VERSION.lstrip('v')}.zip")


WINDOWS = sys.platform == "win32"


def vrpathreg():
    # SteamVR keeps its own install path in openvrpaths.vrpath
    if WINDOWS:
        vrpath = os.path.join(os.environ["LOCALAPPDATA"], "openvr", "openvrpaths.vrpath")
    else:
        vrpath = os.path.join(os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config"), "openvr", "openvrpaths.vrpath")
    try:
        paths = json.load(open(vrpath, encoding="utf-8"))
    except (OSError, ValueError, KeyError):
        paths = {}
    for rt in paths.get("runtime", []):
        exe = os.path.join(rt, "bin", "win64", "vrpathreg.exe") if WINDOWS else os.path.join(rt, "bin", "vrpathreg.sh")
        if os.path.exists(exe):
            return exe
    sys.exit("SteamVR not found: run it once, then try again")


def steamvr_running():
    if WINDOWS:
        out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq vrserver.exe"], capture_output=True, text=True).stdout
        return "vrserver.exe" in out
    return subprocess.run(["pgrep", "-x", "vrserver"], capture_output=True).returncode == 0


def pc():
    if steamvr_running():
        sys.exit("close SteamVR first")
    lib = os.path.join("win64", "driver_questlhsync.dll") if WINDOWS else os.path.join("linux64", "driver_questlhsync.so")
    if not os.path.exists(os.path.join(DRIVER, "bin", lib)):
        sys.exit("build the driver first: " + ("build.bat" if WINDOWS else "./build.sh"))
    subprocess.run([vrpathreg(), "adddriver", DRIVER], check=True)
    print("driver registered:", DRIVER)
    print("done: start SteamVR")


def remove():
    if steamvr_running():
        sys.exit("close SteamVR first")
    subprocess.run([vrpathreg(), "removedriver", DRIVER], check=True)
    print("QuestLHSync driver unregistered")


def adb_exe():
    sdk = os.environ.get("ANDROID_HOME") or os.path.join(os.environ.get("LOCALAPPDATA", ""), "Android", "Sdk")
    exe = shutil.which("adb") or os.path.join(sdk, "platform-tools", "adb.exe")
    if not os.path.exists(exe):
        sys.exit("adb not found: install Android platform-tools and put adb on PATH")
    return exe


def headset():
    if not os.path.exists(MODULE):
        sys.exit(f"build the module first: python magisk\\build_module.py ({MODULE} missing)")
    adb_path = adb_exe()

    def adb(*args, check=True, **kw):
        return subprocess.run([adb_path, *args], capture_output=True, text=True, check=check, **kw)

    devs = [l.split()[0] for l in adb("devices").stdout.splitlines()[1:] if l.strip().endswith("device")]
    names = {"seacliff": "Quest Pro", "eureka": "Quest 3", "panther": "Quest 3S"}
    quest = []
    for dev in devs:
        code = adb("-s", dev, "shell", "getprop ro.product.device", check=False).stdout.strip()
        quest += [(dev, names[k]) for k in names if code.startswith(k)]
    if not quest:
        sys.exit(f"no Quest Pro, 3 or 3S on adb ({len(devs)} other device(s))")
    d, name = quest[0]
    if "uid=0" not in adb("-s", d, "shell", "su -c id", check=False).stdout:
        sys.exit("su doesn't work on the headset (Magisk root needed; allow Shell in Magisk's superuser list)")
    dst = "/sdcard/Download/QuestLHSync-quest-module.zip"
    print(f"{name}: pushing the module")
    adb("-s", d, "push", MODULE, dst)
    r = adb("-s", d, "shell", f"su -c 'magisk --install-module {dst}'", check=False)
    print(r.stdout.strip())
    adb("-s", d, "shell", f"rm -f {dst}", check=False)
    if r.returncode:
        sys.exit("module install failed")
    # run the new lhsyncd now; at the next boot Magisk moves modules_update/questlhsync into place and starts it itself
    # service.sh replaces a running copy itself (setsid: a plain background child dies with the adb shell)
    adb("-s", d, "shell", "su -c 'setsid sh /data/adb/modules_update/questlhsync/service.sh </dev/null >/dev/null 2>&1 &'",
        check=False, timeout=30)
    time.sleep(6)
    print(adb("-s", d, "shell", "su -c 'pidof lhsyncd'", check=False).stdout.strip() and "lhsyncd running" or "lhsyncd NOT running")


if __name__ == "__main__":
    {"headset": headset, "remove": remove}.get(sys.argv[1] if len(sys.argv) > 1 else "", pc)()
