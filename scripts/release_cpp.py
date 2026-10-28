#!/usr/bin/env python3
"""Deterministic packager and release assembler for the Puls C++ application."""
from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import sys
import tarfile
import tempfile
import time
import zipfile

TARGETS = (
    ("linux", "amd64"),
    ("linux", "arm64"),
    ("windows", "amd64"),
    ("windows", "arm64"),
    ("darwin", "amd64"),
    ("darwin", "arm64"),
)
VERSION_RE = re.compile(r"^[A-Za-z0-9._-]+$")
DRIVE_RE = re.compile(r"^[A-Za-z]:")
MANIFEST = "RELEASE_MANIFEST.json"
SUMS = "SHA256SUMS.txt"


def source_date_epoch() -> int:
    raw = os.environ.get("SOURCE_DATE_EPOCH", "0")
    try:
        value = int(raw)
    except ValueError as exc:
        raise ReleaseError("SOURCE_DATE_EPOCH must be a non-negative integer") from exc
    if value < 0:
        raise ReleaseError("SOURCE_DATE_EPOCH must be a non-negative integer")
    return value


class ReleaseError(Exception):
    pass


def validate_version(version: str) -> None:
    if version in ("", ".", "..") or not VERSION_RE.fullmatch(version):
        raise ReleaseError(f"invalid version: {version!r}")


def archive_name(version: str, os_name: str, arch: str) -> str:
    suffix = ".zip" if os_name == "windows" else ".tar.gz"
    return f"Puls_{version}_{os_name}_{arch}{suffix}"


def safe_relative(path: str) -> PurePosixPath:
    p = PurePosixPath(path)
    if p.is_absolute() or DRIVE_RE.match(path) or not p.parts or any(part in ("", ".", "..") for part in p.parts):
        raise ReleaseError(f"unsafe archive path: {path!r}")
    return p


def safe_link(member: str, target: str, root_name: str) -> bool:
    target_path = PurePosixPath(target)
    if target_path.is_absolute() or DRIVE_RE.match(target):
        return False
    stack = list(PurePosixPath(member).parent.parts)
    if not stack or stack[0] != root_name:
        return False
    for part in target_path.parts:
        if part in ("", "."):
            continue
        if part == "..":
            if len(stack) <= 1:
                return False
            stack.pop()
        else:
            stack.append(part)
    return True


def inside(root: Path, path: Path) -> bool:
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def collect_stage(stage: Path) -> list[tuple[str, Path, int, str | None]]:
    root = stage.resolve(strict=True)
    if not root.is_dir():
        raise ReleaseError("stage must be a directory")
    entries: list[tuple[str, Path, int, str | None]] = []
    for current, dirs, files in os.walk(root, topdown=True, followlinks=False):
        current_path = Path(current)
        names = sorted(dirs + files)
        dirs[:] = sorted(d for d in dirs if not (current_path / d).is_symlink())
        for name in names:
            source = current_path / name
            relative = source.relative_to(root).as_posix()
            safe_relative(relative)
            info = source.lstat()
            if stat.S_ISDIR(info.st_mode):
                entries.append((relative + "/", source, 0o755, None))
            elif stat.S_ISREG(info.st_mode):
                mode = 0o755 if info.st_mode & 0o111 else 0o644
                entries.append((relative, source, mode, None))
            elif stat.S_ISLNK(info.st_mode):
                target = os.readlink(source)
                if os.path.isabs(target):
                    raise ReleaseError(f"absolute symlink is not allowed: {relative}")
                resolved = (source.parent / target).resolve(strict=False)
                if not inside(root, resolved):
                    raise ReleaseError(f"symlink escapes stage: {relative} -> {target}")
                entries.append((relative, source, 0o777, target))
            else:
                raise ReleaseError(f"special file is not allowed: {relative}")
    return sorted(entries, key=lambda entry: entry[0])


def expected_executable(stage: Path, os_name: str) -> Path:
    candidates = [stage / "bin" / ("puls.exe" if os_name == "windows" else "puls")]
    if os_name == "darwin":
        candidates += [stage / "Puls.app" / "Contents" / "MacOS" / "puls",
                       stage / "puls.app" / "Contents" / "MacOS" / "puls"]
    for path in candidates:
        try:
            info = path.lstat()
        except FileNotFoundError:
            continue
        if stat.S_ISREG(info.st_mode) and info.st_mode & 0o111:
            return path
    raise ReleaseError(f"stage is missing executable {candidates[0].relative_to(stage)}")


def _tar_info(name: str, mode: int, size: int, kind: str, link: str | None = None) -> tarfile.TarInfo:
    info = tarfile.TarInfo(name)
    info.uid = info.gid = 0
    info.uname = info.gname = ""
    info.mtime = 0
    info.mode = mode
    info.size = size
    if kind == "dir":
        info.type = tarfile.DIRTYPE
    elif kind == "symlink":
        info.type = tarfile.SYMTYPE
        info.linkname = link or ""
        info.size = 0
    return info


def create_archive(stage: Path, output: Path, version: str, os_name: str, arch: str, nogui: bool) -> Path:
    validate_version(version)
    if (os_name, arch) not in TARGETS:
        raise ReleaseError(f"unsupported target: {os_name}/{arch}")
    cli_only_target = os_name == "windows" and arch == "arm64"
    if cli_only_target and not nogui:
        raise ReleaseError("windows/arm64 is CLI-only; pass --nogui")
    if nogui and not cli_only_target:
        raise ReleaseError("--nogui is only supported for windows/arm64")
    entries = collect_stage(stage)
    executable = expected_executable(stage, os_name)
    epoch = source_date_epoch()
    output.mkdir(parents=True, exist_ok=True)
    final = output / archive_name(version, os_name, arch)
    if final.exists() or final.is_symlink():
        raise ReleaseError(f"refusing to overwrite existing output: {final.name}")
    root_name = final.name.removesuffix(".tar.gz").removesuffix(".zip")
    try:
        fd, temporary = tempfile.mkstemp(prefix=f".{final.name}.", suffix=".tmp", dir=output)
        os.close(fd)
        temp_path = Path(temporary)
        if os_name == "windows":
            with zipfile.ZipFile(temp_path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
                folder = zipfile.ZipInfo(root_name + "/")
                zip_epoch = min(max(epoch, 315532800), 4354819198)
                folder.date_time = time.gmtime(zip_epoch)[:6]
                folder.external_attr = (stat.S_IFDIR | 0o755) << 16
                zf.writestr(folder, b"")
                for rel, source, mode, link in entries:
                    name = root_name + "/" + rel
                    info = zipfile.ZipInfo(name, time.gmtime(zip_epoch)[:6])
                    info.create_system = 3
                    if link is not None:
                        info.external_attr = (stat.S_IFLNK | 0o777) << 16
                        zf.writestr(info, link.encode("utf-8"))
                    elif rel.endswith("/"):
                        info.external_attr = (stat.S_IFDIR | mode) << 16
                        zf.writestr(info, b"")
                    else:
                        info.external_attr = (stat.S_IFREG | mode) << 16
                        zf.writestr(info, source.read_bytes())
        else:
            with open(temp_path, "wb") as raw:
                with gzip.GzipFile(filename="", mode="wb", fileobj=raw, compresslevel=9, mtime=0) as gz:
                    with tarfile.open(fileobj=gz, mode="w", format=tarfile.PAX_FORMAT) as tf:
                        root_info = _tar_info(root_name + "/", 0o755, 0, "dir")
                        root_info.mtime = epoch
                        tf.addfile(root_info)
                        for rel, source, mode, link in entries:
                            name = root_name + "/" + rel
                            if link is not None:
                                info = _tar_info(name, mode, 0, "symlink", link)
                                info.mtime = epoch
                                tf.addfile(info)
                            elif rel.endswith("/"):
                                info = _tar_info(name, mode, 0, "dir")
                                info.mtime = epoch
                                tf.addfile(info)
                            else:
                                size = source.stat().st_size
                                with source.open("rb") as content:
                                    info = _tar_info(name, mode, size, "file")
                                    info.mtime = epoch
                                    tf.addfile(info, content)
                raw.flush()
                os.fsync(raw.fileno())
        if not executable.is_file():
            raise ReleaseError("staged executable disappeared during packaging")
        os.chmod(temp_path, 0o644)
        os.replace(temp_path, final)
    except Exception:
        try:
            temp_path.unlink()
        except (UnboundLocalError, FileNotFoundError):
            pass
        raise
    return final


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def validate_archive(path: Path, expected_root: str, os_name: str, arch: str) -> None:
    expected_bin = "puls.exe" if os_name == "windows" else "puls"
    expected_paths = {f"{expected_root}/bin/{expected_bin}"}
    if os_name == "darwin":
        expected_paths |= {f"{expected_root}/Puls.app/Contents/MacOS/puls",
                           f"{expected_root}/puls.app/Contents/MacOS/puls"}
    seen: set[str] = set()
    executable_paths: set[str] = set()
    def check_member(name: str) -> None:
        p = safe_relative(name.rstrip("/"))
        if p.parts[0] != expected_root:
            raise ReleaseError(f"archive has unexpected root: {name}")
        if name in seen:
            raise ReleaseError(f"archive contains duplicate path: {name}")
        seen.add(name)
    if path.suffix == ".zip":
        with zipfile.ZipFile(path) as zf:
            for item in zf.infolist():
                check_member(item.filename)
                mode = item.external_attr >> 16
                if item.filename in expected_paths and mode & 0o111:
                    executable_paths.add(item.filename)
                if stat.S_ISLNK(mode):
                    target = zf.read(item).decode("utf-8")
                    if not safe_link(item.filename, target, expected_root):
                        raise ReleaseError(f"archive symlink escapes root: {item.filename}")
    else:
        with tarfile.open(path, "r:gz") as tf:
            for item in tf:
                check_member(item.name)
                if not (item.isfile() or item.isdir() or item.issym()):
                    raise ReleaseError(f"archive contains special entry: {item.name}")
                if item.isfile() and item.name in expected_paths and item.mode & 0o111:
                    executable_paths.add(item.name)
                if item.issym():
                    if not safe_link(item.name, item.linkname, expected_root):
                        raise ReleaseError(f"archive symlink escapes root: {item.name}")
    if not expected_paths.intersection(executable_paths):
        raise ReleaseError(f"archive is missing executable with executable mode for {os_name}/{arch}")


def atomic_write(path: Path, content: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=f".{path.name}.", suffix=".tmp", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, 0o644)
        os.replace(temporary, path)
    except Exception:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass
        raise


def assemble(version: str, input_dir: Path, output_dir: Path, install_sh: Path | None,
             install_ps1: Path | None, android_apk: Path | None) -> None:
    validate_version(version)
    input_dir = input_dir.resolve(strict=True)
    output_dir.mkdir(parents=True, exist_ok=True)
    output_dir = output_dir.resolve(strict=True)
    installer_data: list[tuple[str, Path, bytes]] = []
    for source, name in ((install_sh, "install.sh"), (install_ps1, "install.ps1")):
        if source:
            data = source.read_bytes()
            if name == "install.ps1" and (data.startswith(b"\xef\xbb\xbf") or any(byte > 0x7f for byte in data)):
                raise ReleaseError("install.ps1 must be ASCII without BOM")
            installer_data.append((name, source, data))
    artifacts: list[dict[str, object]] = []
    files: dict[str, Path] = {}
    pending_archives: list[tuple[str, Path, Path]] = []
    expected_names = {archive_name(version, os_name, arch) for os_name, arch in TARGETS}
    candidates = {path.name for path in input_dir.iterdir()
                  if path.name.startswith(f"Puls_{version}_") and path.name.endswith((".zip", ".tar.gz", ".apk"))}
    unexpected = candidates - expected_names - {f"Puls_{version}_android.apk"}
    if unexpected:
        raise ReleaseError(f"unexpected release artifacts: {', '.join(sorted(unexpected))}")
    for os_name, arch in TARGETS:
        name = archive_name(version, os_name, arch)
        source = input_dir / name
        if not source.is_file() or source.is_symlink():
            raise ReleaseError(f"required archive missing or invalid: {name}")
        root = name.removesuffix(".tar.gz").removesuffix(".zip")
        validate_archive(source, root, os_name, arch)
        dest = output_dir / name
        if source.resolve() != dest.resolve() and (dest.exists() or dest.is_symlink()):
            if not dest.is_file() or digest(dest) != digest(source):
                raise ReleaseError(f"refusing to overwrite unrelated output: {name}")
        pending_archives.append((name, source, dest))
        caps = ["cli"] if (os_name, arch) == ("windows", "arm64") else ["cli", "gui"]
        files[name] = dest
        artifacts.append({"os": os_name, "arch": arch, "file": name, "sha256": digest(source),
                          "kind": "archive", "capabilities": caps})
    if android_apk and (not android_apk.is_file() or android_apk.is_symlink()):
        raise ReleaseError("Android APK must be a regular file")
    for name, source, dest in pending_archives:
        if source.resolve() != dest.resolve() and not dest.exists():
            atomic_write(dest, source.read_bytes())
    if android_apk:
        if not android_apk.is_file() or android_apk.is_symlink():
            raise ReleaseError("Android APK must be a regular file")
        name = f"Puls_{version}_android.apk"
        dest = output_dir / name
        if android_apk.resolve() != dest.resolve():
            if dest.exists() or dest.is_symlink():
                raise ReleaseError(f"refusing to overwrite unrelated output: {name}")
            atomic_write(dest, android_apk.read_bytes())
        files[name] = dest
        artifacts.append({"os": "android", "arch": "universal", "file": name, "sha256": digest(dest),
                          "kind": "apk", "capabilities": ["gui"]})
    installers: list[Path] = []
    for name, source, data in installer_data:
        dest = output_dir / name
        if source.resolve() != dest.resolve():
            atomic_write(dest, data)
        installers.append(dest)
    manifest = {"schema_version": 2, "product": "Puls", "version": version,
                "assets": sorted(artifacts, key=lambda asset: str(asset["file"]))}
    manifest_bytes = (json.dumps(manifest, ensure_ascii=False, indent=2, separators=(",", ": ")) + "\n").encode()
    atomic_write(output_dir / MANIFEST, manifest_bytes)
    checksum_paths = list(files.values()) + [output_dir / MANIFEST] + installers
    sums = "".join(f"{digest(path)}  {path.name}\n" for path in sorted(checksum_paths, key=lambda p: p.name)).encode("ascii")
    atomic_write(output_dir / SUMS, sums)


def parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="Deterministic Puls C++ release packager")
    subs = p.add_subparsers(dest="command", required=True)
    package = subs.add_parser("package")
    package.add_argument("--version", required=True)
    package.add_argument("--target", required=True, help="os/arch")
    package.add_argument("--stage", required=True, type=Path)
    package.add_argument("--output", required=True, type=Path)
    package.add_argument("--nogui", action="store_true")
    assembly = subs.add_parser("assemble")
    assembly.add_argument("--version", required=True)
    assembly.add_argument("--input", required=True, type=Path)
    assembly.add_argument("--output", required=True, type=Path)
    assembly.add_argument("--install-sh", type=Path)
    assembly.add_argument("--install-ps1", type=Path)
    assembly.add_argument("--android-apk", type=Path)
    return p


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    try:
        if args.command == "package":
            parts = args.target.lower().split("/")
            if len(parts) != 2:
                raise ReleaseError("target must be os/arch")
            result = create_archive(args.stage, args.output, args.version, parts[0], parts[1], args.nogui)
            print(result)
        else:
            assemble(args.version, args.input, args.output, args.install_sh, args.install_ps1, args.android_apk)
        return 0
    except (ReleaseError, OSError, tarfile.TarError, zipfile.BadZipFile) as exc:
        print(f"release_cpp: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
