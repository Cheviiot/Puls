#!/usr/bin/env python3
"""Build a minimal Qt SDK from the verified 6.12.0 source archive on Ubuntu 22.04."""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path, PurePosixPath
import subprocess
import sys
import tarfile
import urllib.error
import urllib.request

QT_VERSION = "6.12.0"
SOURCE_NAME = f"qt-everywhere-src-{QT_VERSION}.tar.xz"
SOURCE_URL = (
    "https://download.qt.io/official_releases/qt/6.12/6.12.0/single/"
    + SOURCE_NAME
)
SOURCE_SHA256 = "98ff4f44bac6ec3e1e62ee2a4316ae0e3d15badb015d753cc0268a20db52f165"
SOURCE_ROOT = f"qt-everywhere-src-{QT_VERSION}"


class BuildError(Exception):
    pass


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def download_source(cache_dir: Path) -> Path:
    cache_dir.mkdir(parents=True, exist_ok=True)
    archive = cache_dir / SOURCE_NAME
    if archive.exists():
        actual = sha256_file(archive)
        if actual != SOURCE_SHA256:
            raise BuildError(f"cached source SHA-256 mismatch: {archive}")
        return archive

    temporary = archive.with_suffix(archive.suffix + ".part")
    digest = hashlib.sha256()
    try:
        request = urllib.request.Request(SOURCE_URL, headers={"User-Agent": "Puls-CI"})
        with urllib.request.urlopen(request, timeout=60) as response, temporary.open("wb") as output:
            while block := response.read(1024 * 1024):
                digest.update(block)
                output.write(block)
        actual = digest.hexdigest()
        if actual != SOURCE_SHA256:
            raise BuildError(f"Qt source SHA-256 mismatch: got {actual}")
        temporary.replace(archive)
    except Exception:
        temporary.unlink(missing_ok=True)
        raise
    return archive


def extract_source(archive: Path, destination: Path) -> Path:
    if destination.exists():
        raise BuildError(f"source extraction directory already exists: {destination}")
    destination.mkdir(parents=True)
    try:
        with tarfile.open(archive, mode="r:xz") as bundle:
            members = bundle.getmembers()
            for member in members:
                member_path = PurePosixPath(member.name)
                if (member_path.is_absolute() or ".." in member_path.parts
                        or not member_path.parts or member_path.parts[0] != SOURCE_ROOT):
                    raise BuildError(f"unsafe path in Qt source archive: {member.name}")
            bundle.extractall(destination, members=members, filter="data")
    except Exception:
        # Destination is a task-specific scratch directory, never a user path.
        import shutil

        shutil.rmtree(destination, ignore_errors=True)
        raise

    source = destination / SOURCE_ROOT
    required = (
        "configure",
        "CMakeLists.txt",
        "qtbase/.cmake.conf",
        "qtbase/CMakeLists.txt",
        "qtwebsockets/CMakeLists.txt",
        "qtwayland/CMakeLists.txt",
    )
    missing = [name for name in required if not (source / name).is_file()]
    if missing:
        raise BuildError(f"Qt source archive is missing required files: {', '.join(missing)}")
    return source


def build_sdk(source: Path, build_dir: Path, prefix: Path, jobs: int) -> None:
    build_dir.mkdir(parents=True, exist_ok=False)
    prefix.parent.mkdir(parents=True, exist_ok=True)
    if prefix.exists():
        raise BuildError(f"Qt install prefix already exists: {prefix}")
    run(
        [
            str(source / "configure"),
            "-prefix",
            str(prefix),
            "-release",
            "-opensource",
            "-confirm-license",
            "-nomake",
            "tests",
            "-nomake",
            "examples",
            "-submodules",
            "qtbase,qtwebsockets,qtwayland",
            "-no-feature-gtk3",
            "-openssl-runtime",
        ],
        cwd=build_dir,
    )
    run(["cmake", "--build", str(build_dir), "--parallel", str(jobs)])
    run(["cmake", "--install", str(build_dir)])
    config = prefix / "lib" / "cmake" / "Qt6" / "Qt6Config.cmake"
    if not config.is_file():
        raise BuildError(f"Qt SDK install did not create {config}")


def run(command: list[str], cwd: Path | None = None) -> None:
    print("+", " ".join(command), flush=True)
    subprocess.run(command, cwd=cwd, check=True)


def write_github_environment(prefix: Path) -> None:
    environment_file = os.environ.get("GITHUB_ENV")
    path_file = os.environ.get("GITHUB_PATH")
    if environment_file:
        with open(environment_file, "a", encoding="utf-8") as stream:
            stream.write(f"QT_ROOT_DIR={prefix}\n")
            stream.write(f"CMAKE_PREFIX_PATH={prefix}\n")
    if path_file:
        with open(path_file, "a", encoding="utf-8") as stream:
            stream.write(f"{prefix / 'bin'}\n")


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--cache-dir", required=True, type=Path, help="directory for the verified Qt source archive")
    result.add_argument("--work-dir", required=True, type=Path, help="fresh directory for source extraction and build files")
    result.add_argument("--prefix", required=True, type=Path, help="fresh Qt SDK install prefix")
    result.add_argument("--jobs", type=int, default=2, help="maximum parallel Qt build jobs (default: 2)")
    return result


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    try:
        if args.jobs < 1 or args.jobs > 8:
            raise BuildError("--jobs must be from 1 through 8")
        archive = download_source(args.cache_dir)
        source = extract_source(archive, args.work_dir / "source")
        build_sdk(source, args.work_dir / "build", args.prefix, args.jobs)
        write_github_environment(args.prefix.resolve())
        return 0
    except (BuildError, OSError, tarfile.TarError, urllib.error.URLError, subprocess.CalledProcessError) as exc:
        print(f"build_qt_linux_sdk: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
