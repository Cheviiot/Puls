#!/usr/bin/env python3
"""Verify the signed Qt APK without exposing signing material or user data."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import zipfile


def run(*command: str) -> str:
    return subprocess.check_output(command, text=True)


def verify(apk: Path, tools: Path, version: str, version_code: int) -> dict:
    run(str(tools / "apksigner"), "verify", "--verbose", str(apk))
    run(str(tools / "zipalign"), "-c", "-P", "16", "4", str(apk))
    badging = run(str(tools / "aapt2"), "dump", "badging", str(apk))
    if not re.search(r"package: name='io\.github\.cheviiot\.puls'", badging):
        raise ValueError("APK package identity differs from Puls")
    if f"versionName='{version}'" not in badging or f"versionCode='{version_code}'" not in badging:
        raise ValueError("APK version differs from the configured release")
    if "application-debuggable" in badging:
        raise ValueError("release APK must not be debuggable")
    permissions = run(str(tools / "aapt2"), "dump", "permissions", str(apk))
    requested = set(re.findall(r"uses-permission(?:-sdk-\d+)?: name='([^']+)'", permissions))
    if requested != {"android.permission.INTERNET"}:
        raise ValueError(f"unexpected APK permissions: {sorted(requested)}")
    abis = {"arm64-v8a": 183, "x86_64": 62}
    modules = ("Qt6Core", "Qt6Gui", "Qt6Widgets", "Qt6Network", "Qt6WebSockets")
    with zipfile.ZipFile(apk) as archive:
        names = set(archive.namelist())
        packaged_abis = {name.split("/")[1] for name in names if name.startswith("lib/") and name.endswith(".so")}
        if packaged_abis != set(abis):
            raise ValueError(f"unexpected APK ABI set: {sorted(packaged_abis)}")
        for abi, machine in abis.items():
            required = [f"lib/{abi}/libpuls_{abi}.so", f"lib/{abi}/libcrypto_3.so", f"lib/{abi}/libssl_3.so"]
            for module in modules:
                required.append(f"lib/{abi}/lib{module}_{abi}.so")
            for name in required:
                if name not in names:
                    raise ValueError(f"APK is missing {name}")
                with archive.open(name) as source:
                    header = source.read(64)
                if len(header) < 64 or header[:6] != b"\x7fELF\x02\x01" or struct.unpack_from("<H", header, 18)[0] != machine:
                    raise ValueError(f"APK contains an invalid or wrong-architecture ELF: {name}")
            if not any(name.startswith(f"lib/{abi}/libplugins_tls_") and "openssl" in name for name in names):
                raise ValueError(f"APK is missing the Qt OpenSSL TLS plugin for {abi}")
        for name in ("res/raw/openssl_license.txt", "res/raw/puls_license.txt", "res/raw/qt_lgpl_3_0_only.txt"):
            if name not in names:
                raise ValueError(f"APK is missing license notice: {name}")
    digest = hashlib.sha256()
    with apk.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return {"package": "io.github.cheviiot.puls", "version": version, "version_code": version_code,
            "abis": sorted(abis), "permissions": sorted(requested), "signature_verified": True,
            "sha256": digest.hexdigest()}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--apk", required=True, type=Path)
    parser.add_argument("--build-tools", required=True, type=Path)
    parser.add_argument("--version", required=True)
    parser.add_argument("--version-code", required=True, type=int)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    report = verify(args.apk, args.build_tools, args.version, args.version_code)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps(report, sort_keys=True))


if __name__ == "__main__":
    main()
