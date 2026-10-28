#!/usr/bin/env python3
"""Install Qt 6.12 for Windows x64 using the current Qt SDK metadata layout."""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

QT_VERSION = "6.12.0"
QT_HOST = "windows"
QT_TARGET = "desktop"
QT_ARCH = "win64_msvc2022_64"
OLD_FOLDER = "qt6_6120/qt6_6120"
NEW_FOLDER = "qt6_6120/qt6_6120_msvc2022_64"


def metadata_folder(host: str, target: str, version: str, arch: str, name: str) -> str:
    """Map only the confirmed 6.12.0 MSVC x64 folder rename."""
    if (host, target, version, arch) != (QT_HOST, QT_TARGET, QT_VERSION, QT_ARCH):
        raise RuntimeError("Qt metadata compatibility is limited to Qt 6.12.0 Windows MSVC x64")
    if name != OLD_FOLDER:
        raise RuntimeError(f"unsupported aqt metadata path for Qt 6.12.0 Windows x64: {name!r}")
    return NEW_FOLDER


def install_qt(output_dir: Path, *, dry_run: bool = False, timeout: int = 30) -> int:
    """Delegate parsing, hashes, downloads, and extraction to pinned aqtinstall."""
    from aqt.archives import QtArchives
    from aqt import main as aqt_main
    from aqt.version import __version__

    if __version__ != "3.3.0":
        raise RuntimeError(f"expected aqtinstall 3.3.0, got {__version__}")

    original = QtArchives._get_archives_base

    def get_archives_base(self, name, target_packages):
        corrected_name = metadata_folder(
            self.os_name, self.target, str(self.version), self.arch, name
        )
        return original(self, corrected_name, target_packages)

    QtArchives._get_archives_base = get_archives_base
    sys.argv = [
        "aqt",
        "install-qt",
        QT_HOST,
        QT_TARGET,
        QT_VERSION,
        QT_ARCH,
        "--outputdir",
        str(output_dir),
        "--modules",
        "qtwebsockets",
        "--timeout",
        str(timeout),
    ]
    if dry_run:
        sys.argv.append("--dry-run")
    return aqt_main()


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--outputdir", required=True, type=Path)
    parser.add_argument("--timeout", type=int, default=30)
    parser.add_argument("--dry-run", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    if args.timeout < 1:
        raise SystemExit("--timeout must be positive")
    return install_qt(args.outputdir, dry_run=args.dry_run, timeout=args.timeout)


if __name__ == "__main__":
    raise SystemExit(main())
