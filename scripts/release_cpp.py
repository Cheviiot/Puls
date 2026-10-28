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
import unicodedata
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
    if not isinstance(path, str) or any(unicodedata.category(char) == "Cc" for char in path):
        raise ReleaseError(f"unsafe archive path: {path!r}")
    if "\\" in path or ":" in path or path.startswith("/"):
        raise ReleaseError(f"unsafe archive path: {path!r}")
    parts = [part for part in path.split("/") if part not in ("", ".")]
    if not parts or any(part == ".." for part in parts):
        raise ReleaseError(f"unsafe archive path: {path!r}")
    return PurePosixPath(*parts)


def safe_link(member: str, target: str, root_name: str) -> bool:
    try:
        safe_relative(member)
    except ReleaseError:
        return False
    if not isinstance(target, str) or any(unicodedata.category(char) == "Cc" for char in target):
        return False
    if not target or "\\" in target or ":" in target or target.startswith("/"):
        return False
    stack = list(PurePosixPath(member).parent.parts)
    if not stack or stack[0] != root_name:
        return False
    for part in target.split("/"):
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


def collect_stage(stage: Path, windows_target: bool = False) -> list[tuple[str, Path, int, str | None]]:
    root = stage.resolve(strict=True)
    if not root.is_dir():
        raise ReleaseError("stage must be a directory")
    entries: list[tuple[str, Path, int, str | None]] = []
    seen: set[str] = set()
    for current, dirs, files in os.walk(root, topdown=True, followlinks=False):
        current_path = Path(current)
        names = sorted(dirs + files)
        dirs[:] = sorted(d for d in dirs if not (current_path / d).is_symlink())
        for name in names:
            source = current_path / name
            relative = source.relative_to(root).as_posix()
            canonical = safe_relative(relative).as_posix()
            if windows_target and any(part.endswith((".", " ")) for part in canonical.split("/")):
                raise ReleaseError(f"Windows-ambiguous stage path: {relative}")
            key = canonical.casefold() if windows_target else canonical
            if key in seen:
                raise ReleaseError(f"duplicate stage path: {relative}")
            seen.add(key)
            info = source.lstat()
            if stat.S_ISDIR(info.st_mode):
                entries.append((relative + "/", source, 0o755, None))
            elif stat.S_ISREG(info.st_mode):
                mode = 0o755 if info.st_mode & 0o111 else 0o644
                entries.append((relative, source, mode, None))
            elif stat.S_ISLNK(info.st_mode):
                target = os.readlink(source)
                if not safe_link(relative, target, relative.split("/", 1)[0]):
                    raise ReleaseError(f"symlink escapes stage: {relative} -> {target}")
                try:
                    lexical_resolved = (source.parent / target).resolve(strict=False)
                except (OSError, RuntimeError) as exc:
                    raise ReleaseError(f"dangling or cyclic symlink: {relative} -> {target}") from exc
                if not inside(root, lexical_resolved):
                    raise ReleaseError(f"symlink escapes stage: {relative} -> {target}")
                try:
                    resolved = (source.parent / target).resolve(strict=True)
                except (OSError, RuntimeError) as exc:
                    raise ReleaseError(f"dangling or cyclic symlink: {relative} -> {target}") from exc
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
        if stat.S_ISREG(info.st_mode) and (os_name == "windows" or info.st_mode & 0o111):
            stage_root = stage.resolve(strict=True)
            if path.resolve(strict=True) != stage_root / path.relative_to(stage):
                raise ReleaseError("staged executable path traverses a symlink")
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
    entries = collect_stage(stage, windows_target=os_name == "windows")
    executable = expected_executable(stage, os_name)
    executable_relative = executable.relative_to(stage.resolve()).as_posix()
    entries = [(rel, source, 0o755 if rel == executable_relative else mode, link)
               for rel, source, mode, link in entries]
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
                        info.compress_type = zipfile.ZIP_DEFLATED
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
    case_insensitive = os_name == "windows"
    expected_keys = {name.casefold() if case_insensitive else name for name in expected_paths}
    seen: set[str] = set()
    executable_paths: set[str] = set()
    entries: dict[str, tuple[str, str | None]] = {}
    def check_member(name: str) -> None:
        p = safe_relative(name)
        if case_insensitive and any(part.endswith((".", " ")) for part in p.parts):
            raise ReleaseError(f"Windows-ambiguous archive path: {name}")
        if p.parts[0] != expected_root:
            raise ReleaseError(f"archive has unexpected root: {name}")
        key = p.as_posix().casefold() if case_insensitive else p.as_posix()
        if key in seen:
            raise ReleaseError(f"archive contains duplicate path: {name}")
        seen.add(key)
    if path.suffix == ".zip":
        with zipfile.ZipFile(path) as zf:
            for item in zf.infolist():
                check_member(item.filename)
                mode = item.external_attr >> 16
                p = safe_relative(item.filename)
                key = p.as_posix().casefold() if case_insensitive else p.as_posix()
                if stat.S_ISLNK(mode):
                    try:
                        target = zf.read(item).decode("utf-8")
                    except UnicodeDecodeError as exc:
                        raise ReleaseError(f"archive symlink target is not UTF-8: {item.filename}") from exc
                    if not safe_link(item.filename, target, expected_root):
                        raise ReleaseError(f"archive symlink escapes root: {item.filename}")
                    entries[key] = ("symlink", target)
                elif item.is_dir():
                    entries[key] = ("dir", None)
                else:
                    entries[key] = ("file", None)
                    path_key = p.as_posix().casefold() if case_insensitive else p.as_posix()
                    if path_key in expected_keys and stat.S_ISREG(mode) and mode & 0o111:
                        executable_paths.add(path_key)
    else:
        with tarfile.open(path, "r:gz") as tf:
            for item in tf:
                check_member(item.name)
                if not (item.isfile() or item.isdir() or item.issym()):
                    raise ReleaseError(f"archive contains special entry: {item.name}")
                p = safe_relative(item.name)
                key = p.as_posix().casefold() if case_insensitive else p.as_posix()
                if item.issym():
                    if not safe_link(item.name, item.linkname, expected_root):
                        raise ReleaseError(f"archive symlink escapes root: {item.name}")
                    entries[key] = ("symlink", item.linkname)
                elif item.isdir():
                    entries[key] = ("dir", None)
                else:
                    entries[key] = ("file", None)
                    path_key = p.as_posix().casefold() if case_insensitive else p.as_posix()
                    if path_key in expected_keys and item.mode & 0o111:
                        executable_paths.add(path_key)
    validate_archive_tree(entries, expected_root, case_insensitive)
    if not expected_keys.intersection(executable_paths):
        raise ReleaseError(f"archive is missing a regular executable with executable mode for {os_name}/{arch}")


def validate_archive_tree(entries: dict[str, tuple[str, str | None]], root_name: str,
                          case_insensitive: bool) -> None:
    def key_for(parts: list[str]) -> str:
        value = "/".join(parts)
        return value.casefold() if case_insensitive else value

    explicit = dict(entries)
    for path, (kind, _) in explicit.items():
        parts = path.split("/")
        if parts[0].casefold() != root_name.casefold():
            raise ReleaseError(f"archive has unexpected root: {path}")
        for length in range(1, len(parts)):
            parent = key_for(parts[:length])
            parent_kind = explicit.get(parent, ("dir", None))[0]
            if parent_kind != "dir":
                raise ReleaseError(f"archive path descends through non-directory: {path}")
            entries.setdefault(parent, ("dir", None))

    def resolve(parts: list[str], visiting: set[str]) -> list[str]:
        stack = parts[:1]
        pending = parts[1:]
        while pending:
            part = pending.pop(0)
            if part in ("", "."):
                continue
            if part == "..":
                if len(stack) <= 1:
                    raise ReleaseError("archive symlink chain escapes package root")
                stack.pop()
                continue
            stack.append(part)
            key = key_for(stack)
            kind, target = entries.get(key, ("missing", None))
            if kind == "symlink":
                if key in visiting:
                    raise ReleaseError("archive contains cyclic symlinks")
                visiting.add(key)
                link_parts = target.split("/") if target is not None else []
                stack.pop()
                pending = link_parts + pending
                if not pending:
                    # Resolve a link whose target is the current directory.
                    resolved = resolve(stack, visiting)
                    visiting.remove(key)
                    return resolved
        return stack

    for path, (kind, target) in explicit.items():
        parts = path.split("/")
        for length in range(1, len(parts)):
            if entries.get(key_for(parts[:length]), ("missing", None))[0] == "symlink":
                raise ReleaseError(f"archive path descends through symlink: {path}")
        if kind != "symlink":
            continue
        try:
            target_path = safe_relative("/".join(parts[:-1] + target.split("/")))
        except ReleaseError:
            # Parent-relative links are allowed if resolution stays inside root.
            target_path = PurePosixPath(*parts[:-1], *target.split("/"))
        resolved = resolve(list(target_path.parts), {path.casefold() if case_insensitive else path})
        resolved_key = key_for(resolved)
        if resolved_key not in entries:
            raise ReleaseError(f"archive contains dangling symlink: {path}")


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


def existing_bytes(path: Path) -> bytes | None:
    """Read an existing regular destination without following symlinks."""
    try:
        info = path.lstat()
    except FileNotFoundError:
        return None
    if not stat.S_ISREG(info.st_mode):
        raise ReleaseError(f"refusing to overwrite non-regular output: {path.name}")
    return path.read_bytes()


def owned_manifest(data: bytes) -> bool:
    try:
        doc = json.loads(data)
    except (json.JSONDecodeError, UnicodeDecodeError):
        return False
    if (not isinstance(doc, dict) or doc.get("schema_version") != 2 or doc.get("product") != "Puls"
            or not isinstance(doc.get("version"), str) or not VERSION_RE.fullmatch(doc["version"])
            or not isinstance(doc.get("assets"), list)):
        return False
    prefix = f"Puls_{doc['version']}_"
    return all(isinstance(asset, dict) and isinstance(asset.get("file"), str)
               and asset["file"].startswith(prefix) and Path(asset["file"]).name == asset["file"]
               and re.fullmatch(r"[0-9a-f]{64}", str(asset.get("sha256", "")))
               and asset.get("kind") in ("archive", "apk") for asset in doc["assets"])


def owned_checksums(data: bytes) -> bool:
    try:
        lines = data.decode("ascii").splitlines()
    except UnicodeDecodeError:
        return False
    if not lines:
        return False
    names: set[str] = set()
    for line in lines:
        match = re.fullmatch(r"([0-9a-f]{64})  ([A-Za-z0-9_.-]+)", line)
        if not match or match.group(2) in names:
            return False
        name = match.group(2)
        if name != MANIFEST and name not in ("install.sh", "install.ps1") and not (
                name.startswith("Puls_") and name.endswith((".zip", ".tar.gz", ".apk"))):
            return False
        names.add(name)
    return MANIFEST in names and any(name.startswith("Puls_") for name in names)


def assemble(version: str, input_dir: Path, output_dir: Path, install_sh: Path | None,
             install_ps1: Path | None, android_apk: Path | None) -> None:
    validate_version(version)
    input_dir = input_dir.resolve(strict=True)
    output_dir = output_dir.resolve(strict=False)
    if output_dir.exists() and not output_dir.is_dir():
        raise ReleaseError("output path must be a directory")
    installer_data: list[tuple[str, Path, bytes]] = []
    for source, name in ((install_sh, "install.sh"), (install_ps1, "install.ps1")):
        if source:
            data = source.read_bytes()
            if name == "install.ps1" and (data.startswith(b"\xef\xbb\xbf") or any(byte > 0x7f for byte in data)):
                raise ReleaseError("install.ps1 must be ASCII without BOM")
            installer_data.append((name, source, data))
    artifacts: list[dict[str, object]] = []
    pending_archives: list[tuple[str, Path, Path, str]] = []
    expected_names = {archive_name(version, os_name, arch) for os_name, arch in TARGETS}
    prefix = f"Puls_{version}_".casefold()
    candidates = {path.name for path in input_dir.iterdir()
                  if path.name.casefold().startswith(prefix)
                  and path.name.casefold().endswith((".zip", ".tar.gz", ".apk"))}
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
        source_digest = digest(source)
        old_dest = existing_bytes(dest)
        if old_dest is not None and source.resolve() != dest.resolve():
            if hashlib.sha256(old_dest).hexdigest() != source_digest:
                raise ReleaseError(f"refusing to overwrite unrelated output: {name}")
        pending_archives.append((name, source, dest, source_digest))
        caps = ["cli"] if (os_name, arch) == ("windows", "arm64") else ["cli", "gui"]
        artifacts.append({"os": os_name, "arch": arch, "file": name, "sha256": source_digest,
                          "kind": "archive", "capabilities": caps})
    if android_apk and (not android_apk.is_file() or android_apk.is_symlink()):
        raise ReleaseError("Android APK must be a regular file")
    if android_apk is None:
        candidate_apk = input_dir / f"Puls_{version}_android.apk"
        if candidate_apk.exists():
            android_apk = candidate_apk
    else:
        candidate_apk = input_dir / f"Puls_{version}_android.apk"
        if candidate_apk.exists() and candidate_apk.resolve() != android_apk.resolve():
            if candidate_apk.is_symlink() or not candidate_apk.is_file() or digest(candidate_apk) != digest(android_apk):
                raise ReleaseError("duplicate Android APK inputs have different or unsafe contents")
    apk_pending: tuple[str, Path, Path, str] | None = None
    if android_apk:
        name = f"Puls_{version}_android.apk"
        dest = output_dir / name
        source_digest = digest(android_apk)
        old_dest = existing_bytes(dest)
        if old_dest is not None and android_apk.resolve() != dest.resolve() and hashlib.sha256(old_dest).hexdigest() != source_digest:
            raise ReleaseError(f"refusing to overwrite unrelated output: {name}")
        apk_pending = (name, android_apk, dest, source_digest)
        artifacts.append({"os": "android", "arch": "universal", "file": name, "sha256": source_digest,
                          "kind": "apk", "capabilities": ["gui"]})
    installer_destinations: list[tuple[str, Path, bytes, Path]] = []
    for name, source, data in installer_data:
        dest = output_dir / name
        old_dest = existing_bytes(dest)
        if old_dest is not None and source.resolve() != dest.resolve() and old_dest != data:
            raise ReleaseError(f"refusing to overwrite unrelated output: {name}")
        installer_destinations.append((name, source, data, dest))
    manifest = {"schema_version": 2, "product": "Puls", "version": version,
                "assets": sorted(artifacts, key=lambda asset: str(asset["file"]))}
    manifest_bytes = (json.dumps(manifest, ensure_ascii=False, indent=2, separators=(",", ": ")) + "\n").encode()
    manifest_path = output_dir / MANIFEST
    old_manifest = existing_bytes(manifest_path)
    if old_manifest is not None and old_manifest != manifest_bytes and not owned_manifest(old_manifest):
        raise ReleaseError(f"refusing to overwrite unrelated output: {MANIFEST}")
    checksum_rows = [(name, file_hash) for name, _, _, file_hash in pending_archives]
    if apk_pending:
        checksum_rows.append((apk_pending[0], apk_pending[3]))
    checksum_rows.extend((name, hashlib.sha256(data).hexdigest()) for name, _, data, _ in installer_destinations)
    checksum_rows.append((MANIFEST, hashlib.sha256(manifest_bytes).hexdigest()))
    sums = "".join(f"{file_hash}  {name}\n" for name, file_hash in sorted(checksum_rows)).encode("ascii")
    sums_path = output_dir / SUMS
    old_sums = existing_bytes(sums_path)
    if old_sums is not None and old_sums != sums and not owned_checksums(old_sums):
        raise ReleaseError(f"refusing to overwrite unrelated output: {SUMS}")

    # All inputs and destinations are validated before the first write.
    output_dir.mkdir(parents=True, exist_ok=True)
    for name, source, dest, _ in pending_archives:
        if source.resolve() != dest.resolve() and not dest.exists():
            atomic_write(dest, source.read_bytes())
    if apk_pending:
        name, source, dest, _ = apk_pending
        if source.resolve() != dest.resolve() and not dest.exists():
            atomic_write(dest, source.read_bytes())
    for _, source, data, dest in installer_destinations:
        if source.resolve() != dest.resolve() and not dest.exists():
            atomic_write(dest, data)
    if not manifest_path.exists() or manifest_path.read_bytes() != manifest_bytes:
        atomic_write(manifest_path, manifest_bytes)
    if not sums_path.exists() or sums_path.read_bytes() != sums:
        atomic_write(sums_path, sums)


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
