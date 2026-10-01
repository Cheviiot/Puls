#!/usr/bin/env python3
"""Packages Puls release archives and assembles the release metadata.

The package command turns a directory installed with
`cmake --install <build> --prefix <stage>` (PULS_PORTABLE_INSTALL=ON) into a
reproducible archive: fixed timestamps, owners and permissions, and entries
in sorted order. The assemble command checks the archives of a release,
writes RELEASE_MANIFEST.json (schema 3) and SHA256SUMS.txt, and copies the
installers next to them.

    release.py package --version 0.4.0 --os linux --arch amd64 \\
        --stage stage --output dist
    release.py assemble --version 0.4.0 --output dist
"""

import argparse
import gzip
import hashlib
import io
import json
import os
import re
import stat
import sys
import tarfile
import zipfile
from pathlib import Path

MANIFEST_NAME = "RELEASE_MANIFEST.json"
CHECKSUMS_NAME = "SHA256SUMS.txt"
SCHEMA_VERSION = 3
SYSTEMS = ("linux", "darwin", "windows")
ARCHITECTURES = ("amd64", "arm64")
# The released desktop targets and whether they include the graphical
# interface; Windows on ARM64 ships only the CLI.
DESKTOP_TARGETS = {
    ("linux", "amd64"): True,
    ("linux", "arm64"): True,
    ("darwin", "amd64"): True,
    ("darwin", "arm64"): True,
    ("windows", "amd64"): True,
    ("windows", "arm64"): False,
}
DOCUMENTS = ("CHANGELOG.md", "LICENSE", "README.md", "THIRD_PARTY_NOTICES.txt")
INSTALLERS = (("install.sh", 0o755), ("install.ps1", 0o644))
ZIP_TIMESTAMP = (1980, 1, 1, 0, 0, 0)


class ReleaseError(Exception):
    """A release input is missing or malformed."""


def check_version(version):
    if version in ("", ".", "..") or not re.fullmatch(r"[0-9A-Za-z._-]+", version):
        raise ReleaseError(f"версия {version!r} содержит недопустимые символы")


def check_target(system, architecture):
    if system not in SYSTEMS:
        raise ReleaseError(f"неизвестная система {system!r}")
    if architecture not in ARCHITECTURES:
        raise ReleaseError(f"неизвестная архитектура {architecture!r}")


def archive_name(version, system, architecture):
    extension = ".zip" if system == "windows" else ".tar.gz"
    return f"Puls_{version}_{system}_{architecture}{extension}"


def program_paths(system):
    """Relative paths of the CLI and the graphical application."""
    if system == "windows":
        return "puls.exe", "puls-gui.exe"
    if system == "darwin":
        return "puls", "Puls.app/Contents/MacOS/Puls"
    return "puls", "puls-gui"


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as file:
        for chunk in iter(lambda: file.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_atomically(path, data, mode=0o644):
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_bytes(data)
    os.chmod(temporary, mode)
    os.replace(temporary, path)


def staged_entries(stage, system):
    """Sorted (relative path, absolute path, is directory, executable)."""
    if not stage.is_dir():
        raise ReleaseError(f"каталог {stage} не найден")
    entries = []
    for root, directories, files in os.walk(stage):
        directories.sort()
        for name in sorted(directories + files):
            path = Path(root) / name
            relative = path.relative_to(stage).as_posix()
            if path.is_symlink():
                raise ReleaseError(f"{relative}: символические ссылки не поддерживаются")
            if path.is_dir():
                entries.append((relative, path, True, False))
            elif path.is_file():
                if system == "windows":
                    executable = name.lower().endswith(".exe")
                else:
                    executable = bool(path.stat().st_mode & stat.S_IXUSR)
                entries.append((relative, path, False, executable))
            else:
                raise ReleaseError(f"{relative}: неподдерживаемый тип файла")
    entries.sort(key=lambda entry: entry[0])
    return entries


def write_tar_gz(destination, top, entries):
    raw = io.BytesIO()
    with gzip.GzipFile(filename="", mode="wb", compresslevel=9, fileobj=raw, mtime=0) as packed:
        with tarfile.open(fileobj=packed, mode="w", format=tarfile.PAX_FORMAT) as archive:
            directory = tarfile.TarInfo(top)
            directory.type = tarfile.DIRTYPE
            directory.mode = 0o755
            directory.mtime = 0
            archive.addfile(directory)
            for relative, path, is_directory, executable in entries:
                info = tarfile.TarInfo(f"{top}/{relative}")
                info.mtime = 0
                info.uid = info.gid = 0
                info.uname = info.gname = ""
                if is_directory:
                    info.type = tarfile.DIRTYPE
                    info.mode = 0o755
                    archive.addfile(info)
                else:
                    info.mode = 0o755 if executable else 0o644
                    info.size = path.stat().st_size
                    with open(path, "rb") as file:
                        archive.addfile(info, file)
    write_atomically(destination, raw.getvalue())


def write_zip(destination, top, entries):
    raw = io.BytesIO()
    with zipfile.ZipFile(raw, "w") as archive:
        for relative, path, is_directory, executable in entries:
            if is_directory:
                continue
            info = zipfile.ZipInfo(f"{top}/{relative}", date_time=ZIP_TIMESTAMP)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3
            info.external_attr = ((0o755 if executable else 0o644) | stat.S_IFREG) << 16
            archive.writestr(info, path.read_bytes(), compresslevel=9)
    write_atomically(destination, raw.getvalue())


def package(version, system, architecture, stage, output):
    check_version(version)
    check_target(system, architecture)
    stage = Path(stage)
    entries = staged_entries(stage, system)
    files = {relative for relative, _, is_directory, _ in entries if not is_directory}
    cli, gui = program_paths(system)
    for required in (cli,) + DOCUMENTS:
        if required not in files:
            raise ReleaseError(f"в {stage} нет {required}")
    name = archive_name(version, system, architecture)
    top = name[: -len(".zip")] if name.endswith(".zip") else name[: -len(".tar.gz")]
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    destination = output / name
    if system == "windows":
        write_zip(destination, top, entries)
    else:
        write_tar_gz(destination, top, entries)
    return destination, gui in files


def archive_members(path):
    if path.name.endswith(".zip"):
        with zipfile.ZipFile(path) as archive:
            return set(archive.namelist())
    with tarfile.open(path, "r:gz") as archive:
        return {member.name for member in archive.getmembers() if member.isfile()}


def capabilities(path, system, top):
    members = archive_members(path)
    cli, gui = program_paths(system)
    if f"{top}/{cli}" not in members:
        raise ReleaseError(f"в {path.name} нет {cli}")
    return ["cli", "gui"] if f"{top}/{gui}" in members else ["cli"]


def parse_targets(text):
    targets = {}
    for item in text.split(","):
        parts = item.strip().split("/")
        if len(parts) != 2:
            raise ReleaseError(f"неверная цель {item!r}: ожидается система/архитектура")
        check_target(*parts)
        targets[tuple(parts)] = None
    return targets


def assemble(version, output, targets=None, require_android=False, root=None):
    """Writes the manifest and checksums for the archives in output.

    targets maps (system, architecture) to whether the graphical interface is
    required, or to None when either is accepted.
    """
    check_version(version)
    output = Path(output)
    root = Path(root) if root else Path(__file__).resolve().parent.parent
    targets = DESKTOP_TARGETS if targets is None else targets
    pattern = re.compile(
        rf"Puls_{re.escape(version)}_({'|'.join(SYSTEMS)})_({'|'.join(ARCHITECTURES)})"
        r"\.(tar\.gz|zip)")
    found = {}
    for path in sorted(output.iterdir()):
        match = pattern.fullmatch(path.name)
        if not match:
            continue
        system, architecture, _ = match.groups()
        if path.name != archive_name(version, system, architecture):
            raise ReleaseError(f"{path.name}: неверное расширение архива")
        found[(system, architecture)] = path
    assets = []
    for (system, architecture), gui_required in sorted(targets.items()):
        path = found.get((system, architecture))
        if path is None:
            raise ReleaseError(f"нет архива для {system}/{architecture}")
        top = path.name[: -len(".zip")] if path.name.endswith(".zip") else path.name[: -len(".tar.gz")]
        provided = capabilities(path, system, top)
        if gui_required is not None and ("gui" in provided) != gui_required:
            expected = "с графическим интерфейсом" if gui_required else "без графического интерфейса"
            raise ReleaseError(f"{path.name}: ожидался пакет {expected}")
        assets.append((path, system, architecture, "archive", provided))
    unexpected = sorted(set(found) - set(targets))
    if unexpected:
        raise ReleaseError("лишние архивы: " + ", ".join(f"{s}/{a}" for s, a in unexpected))
    apk = output / f"Puls_{version}_android.apk"
    if apk.is_file():
        assets.append((apk, "android", "universal", "apk", ["gui"]))
    elif require_android:
        raise ReleaseError(f"нет {apk.name}")

    assets.sort(key=lambda asset: asset[0].name)
    manifest = {
        "schema_version": SCHEMA_VERSION,
        "product": "Puls",
        "version": version,
        "assets": [
            {
                "os": system,
                "arch": architecture,
                "file": path.name,
                "sha256": sha256_file(path),
                "kind": kind,
                "capabilities": provided,
            }
            for path, system, architecture, kind, provided in assets
        ],
    }
    write_atomically(output / MANIFEST_NAME,
                     (json.dumps(manifest, indent=2) + "\n").encode("ascii"))
    checksummed = [path for path, *_ in assets] + [output / MANIFEST_NAME]
    for name, mode in INSTALLERS:
        destination = output / name
        source = root / "scripts" / name
        if source.resolve() != destination.resolve():
            write_atomically(destination, source.read_bytes(), mode)
        checksummed.append(destination)
    lines = "".join(f"{sha256_file(path)}  {path.name}\n"
                    for path in sorted(checksummed, key=lambda path: path.name))
    write_atomically(output / CHECKSUMS_NAME, lines.encode("ascii"))
    return manifest


def main(argv=None):
    parser = argparse.ArgumentParser(description="Сборка выпуска Puls")
    commands = parser.add_subparsers(dest="command", required=True)
    package_parser = commands.add_parser("package", help="упаковать установленные файлы")
    package_parser.add_argument("--version", required=True)
    package_parser.add_argument("--os", required=True, choices=SYSTEMS)
    package_parser.add_argument("--arch", required=True, choices=ARCHITECTURES)
    package_parser.add_argument("--stage", required=True,
                                help="каталог cmake --install с PULS_PORTABLE_INSTALL=ON")
    package_parser.add_argument("--output", default="dist")
    assemble_parser = commands.add_parser("assemble", help="собрать manifest и контрольные суммы")
    assemble_parser.add_argument("--version", required=True)
    assemble_parser.add_argument("--output", default="dist")
    assemble_parser.add_argument("--targets",
                                 help="цели система/архитектура через запятую вместо всех шести")
    assemble_parser.add_argument("--require-android", action="store_true")
    arguments = parser.parse_args(argv)
    try:
        if arguments.command == "package":
            path, gui = package(arguments.version, arguments.os, arguments.arch,
                                arguments.stage, arguments.output)
            kind = "CLI и GUI" if gui else "CLI"
            print(f"Готово: {path} ({kind})")
        else:
            targets = parse_targets(arguments.targets) if arguments.targets else None
            manifest = assemble(arguments.version, arguments.output, targets,
                                arguments.require_android)
            print(f"Готово: {len(manifest['assets'])} пакетов в {arguments.output}")
    except (ReleaseError, OSError, tarfile.TarError, zipfile.BadZipFile) as error:
        print(f"Ошибка сборки: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
