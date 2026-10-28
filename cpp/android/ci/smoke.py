#!/usr/bin/env python3
"""Launch the signed Qt app on an explicitly selected disposable emulator."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import time


APP = "io.github.cheviiot.puls"
ACTIVITY = APP + "/org.qtproject.qt.android.bindings.QtActivity"


def smoke(serial: str, apk: Path, output: Path) -> None:
    if not serial.startswith("emulator-"):
        raise ValueError("smoke tests require an explicitly selected emulator")
    output.mkdir(parents=True, exist_ok=True)
    prefix = ["adb", "-s", serial]

    def adb(*arguments: str) -> str:
        return subprocess.check_output(prefix + list(arguments), text=True, timeout=30)

    adb("install", "-r", str(apk))
    adb("logcat", "-b", "crash", "-c")
    launched = adb("shell", "am", "start", "-W", "-n", ACTIVITY)
    if "Status: ok" not in launched:
        raise RuntimeError("Qt activity failed to launch")
    deadline = time.monotonic() + 40
    resumed = ""
    while time.monotonic() < deadline:
        activities = adb("shell", "dumpsys", "activity", "activities")
        resumed = "\n".join(line.strip() for line in activities.splitlines()
                            if APP in line and ("mResumedActivity" in line or "topResumedActivity" in line))
        if resumed and adb("shell", "pidof", APP).strip():
            break
        time.sleep(1)
    if not resumed:
        raise RuntimeError("Qt activity did not become the foreground activity")
    time.sleep(5)
    if not adb("shell", "pidof", APP).strip():
        raise RuntimeError("Qt process exited before drawing the dashboard")
    crash = adb("logcat", "-b", "crash", "-d", "-v", "brief")
    if APP in crash:
        raise RuntimeError("Qt application has a crash log")
    screenshot = subprocess.check_output(prefix + ["exec-out", "screencap", "-p"], timeout=30)
    if not screenshot.startswith(b"\x89PNG\r\n\x1a\n") or len(screenshot) < 1024:
        raise RuntimeError("emulator did not provide a rendered screenshot")
    (output / "dashboard.png").write_bytes(screenshot)
    # Returning from background exercises the Android activity lifecycle without
    # starting a public measurement or collecting connection information.
    adb("shell", "input", "keyevent", "KEYCODE_HOME")
    adb("shell", "am", "start", "-W", "-n", ACTIVITY)
    if not adb("shell", "pidof", APP).strip():
        raise RuntimeError("Qt application failed to resume")
    report = {"package": APP, "activity_resumed": True, "process_alive": True,
              "background_resume": True, "screenshot": "dashboard.png", "network_measurement_started": False}
    (output / "smoke.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, sort_keys=True))
    adb("shell", "am", "force-stop", APP)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--apk", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    smoke(args.serial, args.apk, args.output)


if __name__ == "__main__":
    main()
