#!/usr/bin/env python3
"""Validate a relocated native Qt stage by running the staged Puls binary."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import platform
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile


class ValidationError(Exception):
    """A stage failed a runtime or packaging invariant."""


_MACHINE_NAMES = {
    "x86_64": "x86_64",
    "amd64": "x86_64",
    "aarch64": "arm64",
    "arm64": "arm64",
}
_OS_NAMES = {"darwin": "darwin", "macos": "darwin", "windows": "windows", "linux": "linux"}
_ELF_MACHINES = {"x86_64": 62, "arm64": 183}
_PE_MACHINES = {"x86_64": 0x8664, "arm64": 0xAA64}
_MACHO_CPUS = {"x86_64": 0x01000007, "arm64": 0x0100000C}
_QT_REQUIRED = ("Qt6Core", "Qt6Network", "Qt6WebSockets")
_QT_GUI = ("Qt6Gui", "Qt6Widgets")
_ENV_REMOVE = {
    "QT_PLUGIN_PATH",
    "QT_QPA_PLATFORM_PLUGIN_PATH",
    "QT_QPA_PLATFORMTHEME",
    "QT_QPA_PLATFORM",
    "QML2_IMPORT_PATH",
    "LD_LIBRARY_PATH",
    "DYLD_LIBRARY_PATH",
    "DYLD_FRAMEWORK_PATH",
    "PULS_GUI_PREVIEW_DIR",
    "PULS_RUNTIME_PROBE",
    "QT_ROOT_DIR",
    "QTDIR",
}
_SCALE_ENV = {
    "QT_SCALE_FACTOR",
    "QT_AUTO_SCREEN_SCALE_FACTOR",
    "QT_SCREEN_SCALE_FACTORS",
    "QT_DEVICE_PIXEL_RATIO",
    "QT_ENABLE_HIGHDPI_SCALING",
}
_ANSI = re.compile(r"\x1b")
_QT_VERSION_BIN = re.compile(r"(?:^|[/\\])qt[/\\]\d+\.\d+(?:\.\d+)?(?:[/\\][^/\\]+)*[/\\]bin$", re.IGNORECASE)


def parse_target(value: str) -> tuple[str, str]:
    """Parse and normalize a release target such as linux/amd64."""
    pieces = value.lower().split("/")
    if len(pieces) != 2 or pieces[0] not in _OS_NAMES or pieces[1] not in _MACHINE_NAMES:
        raise ValidationError("target must be an OS/architecture pair (linux|windows|darwin and amd64|arm64)")
    return _OS_NAMES[pieces[0]], _MACHINE_NAMES[pieces[1]]


def _host_target() -> tuple[str, str]:
    host_os = {"Linux": "linux", "Windows": "windows", "Darwin": "darwin"}.get(platform.system())
    host_arch = _MACHINE_NAMES.get(platform.machine().lower())
    if not host_os or not host_arch:
        raise ValidationError("cannot infer the current target; pass --target OS/architecture")
    return host_os, host_arch


def _read(path: Path, size: int, offset: int = 0) -> bytes:
    with path.open("rb") as stream:
        stream.seek(offset)
        return stream.read(size)


def binary_format(path: Path) -> tuple[str, set[str]]:
    """Inspect native executable headers and return (format, supported arches)."""
    if not path.is_file():
        raise ValidationError("staged executable is not a regular file")
    header = _read(path, 64)
    if header.startswith(b"\x7fELF"):
        if len(header) < 20 or header[4] != 2 or header[5] not in (1, 2):
            raise ValidationError("staged ELF executable is not a valid 64-bit binary")
        byteorder = "little" if header[5] == 1 else "big"
        machine = int.from_bytes(header[18:20], byteorder)
        arches = {name for name, number in _ELF_MACHINES.items() if number == machine}
        if not arches:
            raise ValidationError("staged ELF executable uses an unsupported architecture")
        return "ELF64", arches

    if header.startswith(b"MZ"):
        if len(header) < 64:
            raise ValidationError("staged PE executable has a truncated DOS header")
        pe_offset = int.from_bytes(header[0x3C:0x40], "little")
        pe = _read(path, 24, pe_offset)
        if len(pe) != 24 or pe[:4] != b"PE\0\0":
            raise ValidationError("staged executable has an invalid PE header")
        machine = int.from_bytes(pe[4:6], "little")
        optional_size = int.from_bytes(pe[20:22], "little")
        if optional_size < 2 or pe_offset + 24 + optional_size > path.stat().st_size:
            raise ValidationError("staged PE executable has a truncated optional header")
        if _read(path, 2, pe_offset + 24) != b"\x0b\x02":
            raise ValidationError("staged PE executable is not a 64-bit image")
        arches = {name for name, number in _PE_MACHINES.items() if number == machine}
        if not arches:
            raise ValidationError("staged PE executable uses an unsupported architecture")
        return "PE", arches

    fat_orders = {
        b"\xca\xfe\xba\xbe": ("big", False),
        b"\xca\xfe\xba\xbf": ("big", True),
        b"\xbe\xba\xfe\xca": ("little", False),
        b"\xbf\xba\xfe\xca": ("little", True),
    }
    if len(header) >= 8 and header[:4] in fat_orders:
        byteorder, is_64 = fat_orders[header[:4]]
        count = int.from_bytes(header[4:8], byteorder)
        entry_size = 32 if is_64 else 20
        if count < 1 or count > 32 or path.stat().st_size < 8 + count * entry_size:
            raise ValidationError("staged Mach-O universal binary has an invalid architecture table")
        arches: set[str] = set()
        with path.open("rb") as stream:
            stream.seek(8)
            for _ in range(count):
                cpu = int.from_bytes(stream.read(4), byteorder)
                arches.update(name for name, number in _MACHO_CPUS.items() if number == cpu)
                stream.seek(entry_size - 4, os.SEEK_CUR)
        if not arches:
            raise ValidationError("staged Mach-O executable has no supported 64-bit architecture")
        return "Mach-O universal", arches

    thin_magic = header[:4]
    if thin_magic in (b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xcf"):
        if len(header) < 8:
            raise ValidationError("staged Mach-O executable has a truncated header")
        byteorder = "little" if thin_magic == b"\xcf\xfa\xed\xfe" else "big"
        cpu = int.from_bytes(header[4:8], byteorder)
        arches = {name for name, number in _MACHO_CPUS.items() if number == cpu}
        if not arches:
            raise ValidationError("staged Mach-O executable uses an unsupported architecture")
        return "Mach-O64", arches

    raise ValidationError("staged executable is not an ELF, PE, or 64-bit Mach-O binary")


def executable_path(stage: Path, target_os: str) -> Path:
    if target_os == "darwin":
        candidates = [
            stage / bundle / "Contents" / "MacOS" / binary
            for bundle in ("puls.app", "Puls.app")
            for binary in ("puls", "Puls")
        ]
    else:
        candidates = [stage / "bin" / ("puls.exe" if target_os == "windows" else "puls")]
    for candidate in candidates:
        if candidate.is_file():
            if target_os != "windows" and not os.access(candidate, os.X_OK):
                raise ValidationError("staged executable does not have executable permission")
            return candidate.resolve(strict=True)
    raise ValidationError("staged Puls executable was not found")


def is_within(root: Path, candidate: Path) -> bool:
    try:
        candidate.resolve(strict=False).relative_to(root.resolve(strict=False))
        return True
    except (ValueError, OSError):
        return False


def _normalized(path: str | Path) -> str:
    return os.path.normcase(str(Path(path).expanduser().resolve(strict=False)))


def _is_qt_sdk_bin(entry: str, roots: list[str], stage_bin: str) -> bool:
    if not entry:
        return False
    resolved = _normalized(entry)
    if resolved == stage_bin or resolved.startswith(stage_bin + os.sep):
        return False
    if Path(resolved).name.lower() != "bin":
        return False
    for root in roots:
        if resolved == root or resolved.startswith(root + os.sep):
            return True
    return bool(_QT_VERSION_BIN.search(entry.rstrip("/\\")))


def sanitize_environment(source: dict[str, str], stage_bin: Path) -> dict[str, str]:
    """Remove ambient Qt/plugin/scaling settings and SDK bin entries from PATH."""
    env = dict(source)
    roots = [
        _normalized(env[key])
        for key in ("QT_ROOT_DIR", "QTDIR")
        if env.get(key)
    ]
    for key in list(env):
        if key in _ENV_REMOVE or key in _SCALE_ENV or key.startswith("QT_SCALE_"):
            env.pop(key, None)
    path_value = env.get("PATH", "")
    stage_bin_norm = _normalized(stage_bin)
    entries = [part for part in path_value.split(os.pathsep)
               if part and not _is_qt_sdk_bin(part, roots, stage_bin_norm)]
    env["PATH"] = os.pathsep.join(entries)
    return env


def _run(command: list[str], env: dict[str, str], timeout: int = 15) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(command, env=env, text=True, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, timeout=timeout, check=False)
    except subprocess.TimeoutExpired as exc:
        raise ValidationError("staged application exceeded its runtime check timeout") from exc
    except OSError as exc:
        raise ValidationError("could not execute a required runtime check") from exc


def _check_no_ansi(output: str, what: str) -> None:
    if _ANSI.search(output):
        raise ValidationError(f"{what} output contains ANSI control sequences")


def _check_cli(binary: Path, env: dict[str, str], version: str, gui: bool) -> None:
    for args, label in ((["--help"], "help"), (["version"], "version")):
        result = _run([str(binary), *args], env)
        if result.returncode != 0:
            raise ValidationError(f"staged {label} command failed")
        combined = result.stdout + result.stderr
        _check_no_ansi(combined, label)
        if label == "version" and version not in result.stdout.split():
            raise ValidationError("staged version output does not match the requested label")
    invalid = _run([str(binary), "--puls-runtime-validator-invalid-option"], env)
    if invalid.returncode != 2:
        raise ValidationError("staged CLI does not return exit code 2 for an invalid option")
    _check_no_ansi(invalid.stdout + invalid.stderr, "invalid-option")
    if not gui:
        gui_result = _run([str(binary), "gui"], env, timeout=10)
        if gui_result.returncode != 2:
            raise ValidationError("GUI-less staged executable must reject `puls gui` with exit code 2")
        _check_no_ansi(gui_result.stdout + gui_result.stderr, "GUI-unavailable")


def _check_json_smoke(binary: Path, env: dict[str, str]) -> None:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as held:
        held.bind(("127.0.0.1", 0))
        port = held.getsockname()[1]
        result = _run([str(binary), "speedtest", "--server", f"127.0.0.1:{port}",
                       "--only", "ping", "--json"], env, timeout=25)
    if result.returncode != 1:
        raise ValidationError("local JSON smoke command did not return exit code 1")
    try:
        envelope = json.loads(result.stdout)
    except json.JSONDecodeError as exc:
        raise ValidationError("local JSON smoke command returned an invalid schema") from exc
    if not isinstance(envelope, dict):
        raise ValidationError("local JSON smoke command returned an invalid schema")
    results = envelope.get("results")
    if not isinstance(results, list) or not results or not isinstance(results[0], dict):
        raise ValidationError("local JSON smoke command returned an invalid schema")
    phases = results[0].get("phases")
    if not isinstance(phases, dict) or not isinstance(phases.get("ping"), dict):
        raise ValidationError("local JSON smoke command returned an invalid schema")
    ping = phases["ping"]
    if (type(envelope.get("schema_version")) is not int or envelope.get("schema_version") != 1 or
            envelope.get("status") != "error" or "connection" not in envelope or
            envelope.get("connection") is not None or not results or
            results[0].get("service") != "speedtest" or results[0].get("status") != "error" or
            ping.get("status") != "error" or not isinstance(ping.get("error"), dict) or
            ping.get("value_ms") is not None or ping.get("mbps") is not None):
        raise ValidationError("local JSON smoke command did not preserve the expected error envelope")
    for name in ("download", "upload"):
        phase = phases.get(name)
        if not isinstance(phase, dict) or phase.get("mbps") is not None:
            raise ValidationError("local JSON smoke command unexpectedly returned a speed value")
    _check_no_ansi(result.stdout + result.stderr, "JSON")


def _runtime_probe(binary: Path, env: dict[str, str], stage: Path) -> dict:
    probe_env = dict(env)
    probe_env["PULS_RUNTIME_PROBE"] = "1"
    result = _run([str(binary), "version"], probe_env)
    if result.returncode != 0:
        raise ValidationError("private Qt runtime probe failed")
    try:
        probe = json.loads(result.stdout)
    except json.JSONDecodeError as exc:
        raise ValidationError("private Qt runtime probe returned invalid JSON") from exc
    if not isinstance(probe, dict):
        raise ValidationError("private Qt runtime probe returned invalid JSON")
    if (probe.get("ssl_available") is not True or not isinstance(probe.get("ssl_backend"), str) or
            not probe["ssl_backend"].strip() or not isinstance(probe.get("qt_version"), str) or
            not probe["qt_version"].strip()):
        raise ValidationError("relocated Qt runtime has no working SSL backend")
    paths = probe.get("plugin_paths")
    prefix = probe.get("qt_prefix")
    if not isinstance(paths, list) or not paths or not isinstance(prefix, str) or not prefix:
        raise ValidationError("private Qt runtime probe omitted plugin paths or Qt prefix")
    if not Path(prefix).is_absolute() or not is_within(stage, Path(prefix)) or not Path(prefix).is_dir():
        raise ValidationError("Qt prefix resolves outside the relocated stage")
    for value in paths:
        if (not isinstance(value, str) or not Path(value).is_absolute() or
                not is_within(stage, Path(value)) or
                not Path(value).is_dir()):
            raise ValidationError("a Qt plugin path resolves outside the relocated stage")
    return probe


def _require_tools(*names: str) -> None:
    if any(shutil.which(name) is None for name in names):
        raise ValidationError("a platform inspection tool required for runtime validation is unavailable")


def _check_linux(stage: Path, binary: Path, gui: bool, env: dict[str, str]) -> dict[str, str]:
    _require_tools("ldd")
    for item in stage.rglob("*"):
        name = item.name.lower()
        if (name.startswith(("libgtk", "libgdk", "libglib", "libgobject", "ld-linux", "ld64.so", "libnss_")) or
                name in ("libc.so.6", "libm.so.6", "libpthread.so.0", "libdl.so.2", "librt.so.1",
                         "libresolv.so.2", "libutil.so.1", "libanl.so.1")):
            raise ValidationError("stage bundles a system GTK, glibc, or loader library")
    result = _run(["ldd", str(binary)], env)
    if result.returncode != 0:
        raise ValidationError("ldd could not inspect the staged executable")
    required = list(_QT_REQUIRED) + (list(_QT_GUI) if gui else [])
    resolved: dict[str, str] = {}
    for name in required:
        match = re.search(rf"(^|\s)lib{re.escape(name)}\.so(?:\.\d+)*\s+=>\s+(\S+)", result.stdout, re.MULTILINE)
        if not match or match.group(2) == "not":
            raise ValidationError(f"staged {name} library did not resolve")
        path = Path(match.group(2))
        if not is_within(stage, path):
            raise ValidationError(f"staged {name} library resolves outside the relocated stage")
        resolved[name] = str(path.resolve(strict=False))
    return resolved


def _check_windows(stage: Path, gui: bool) -> dict[str, str]:
    names = list(_QT_REQUIRED) + (list(_QT_GUI) if gui else [])
    resolved = {}
    for name in names:
        path = stage / "bin" / f"{name}.dll"
        if not path.is_file():
            raise ValidationError(f"stage is missing {name}.dll")
        resolved[name] = str(path.resolve(strict=True))
    return resolved


def _check_macos(stage: Path, binary: Path, gui: bool) -> dict[str, str]:
    _require_tools("otool")
    bundle = binary.parents[1]
    frameworks = bundle / "Frameworks"
    names = ["QtCore", "QtNetwork", "QtWebSockets"] + (["QtGui", "QtWidgets"] if gui else [])
    resolved = {}
    for name in names:
        path = frameworks / f"{name}.framework"
        if not path.is_dir() or not is_within(stage, path):
            raise ValidationError(f"stage is missing {name}.framework")
        resolved[name] = str(path.resolve(strict=True))
    result = _run(["otool", "-L", str(binary)], os.environ.copy())
    if result.returncode != 0:
        raise ValidationError("otool could not inspect the staged executable")
    qt_refs = [line.strip().split(" (", 1)[0] for line in result.stdout.splitlines()[1:]
               if ".framework/" in line and re.search(r"/Qt(?:Core|Network|WebSockets|Gui|Widgets)\.framework/", line)]
    for ref in qt_refs:
        if ref.startswith("/") and not is_within(stage, Path(ref)):
            raise ValidationError("a Qt framework reference points to an absolute SDK path outside the stage")
        if not ref.startswith(("/", "@rpath/", "@loader_path/", "@executable_path/")):
            raise ValidationError("a Qt framework reference is not relocatable")
    if not qt_refs:
        raise ValidationError("staged executable has no relocatable Qt framework references")
    return resolved


def _check_preview(binary: Path, env: dict[str, str], scratch: Path) -> int:
    previews = scratch / "previews"
    previews.mkdir(parents=True, exist_ok=False)
    gui_env = dict(env)
    gui_env["QT_QPA_PLATFORM"] = "offscreen"
    gui_env["PULS_GUI_PREVIEW_DIR"] = str(previews)
    result = _run([str(binary), "gui"], gui_env, timeout=45)
    if result.returncode != 0:
        raise ValidationError("staged GUI preview command failed")
    files = sorted(previews.rglob("*.png"))
    if len(files) != 42:
        raise ValidationError("GUI preview did not generate the complete 42-image matrix")
    expected = {(360, 294), (360, 640), (900, 600), (1280, 800)}
    found: set[tuple[int, int]] = set()
    for path in files:
        data = path.read_bytes()
        if len(data) < 24 or data[:8] != b"\x89PNG\r\n\x1a\n" or data[12:16] != b"IHDR":
            raise ValidationError("GUI preview contains an invalid PNG image")
        dimensions = struct.unpack(">II", data[16:24])
        if dimensions not in expected:
            raise ValidationError("GUI preview has an unexpected image size")
        found.add(dimensions)
    if found != expected:
        raise ValidationError("GUI preview omitted a required window size")
    return len(files)


def validate_stage(stage_arg: Path, version: str, gui: bool,
                   target_arg: str | None, scratch_parent: Path) -> dict:
    stage = stage_arg.resolve(strict=True)
    if not stage.is_dir():
        raise ValidationError("stage must be a directory")
    if not version or any(char.isspace() for char in version):
        raise ValidationError("version label must be nonempty and contain no whitespace")
    target = parse_target(target_arg) if target_arg else _host_target()
    binary = executable_path(stage, target[0])
    if not is_within(stage, binary):
        raise ValidationError("staged executable resolves outside the relocated stage")
    fmt, arches = binary_format(binary)
    if target[1] not in arches:
        raise ValidationError("staged executable architecture does not match --target")
    format_os = {"ELF64": "linux", "PE": "windows", "Mach-O64": "darwin", "Mach-O universal": "darwin"}[fmt]
    if format_os != target[0]:
        raise ValidationError("staged executable format does not match --target operating system")

    child_env = sanitize_environment(dict(os.environ), binary.parent)
    _check_cli(binary, child_env, version, gui)
    _check_json_smoke(binary, child_env)
    probe = _runtime_probe(binary, child_env, stage)
    if target[0] == "linux":
        dependencies = _check_linux(stage, binary, gui, child_env)
    elif target[0] == "windows":
        dependencies = _check_windows(stage, gui)
    else:
        dependencies = _check_macos(stage, binary, gui)

    previews = 0
    if gui:
        scratch_parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="puls-runtime-", dir=scratch_parent) as directory:
            previews = _check_preview(binary, child_env, Path(directory))
    return {
        "status": "ok",
        "version": version,
        "target": f"{target[0]}/{target[1]}",
        "binary_format": fmt,
        "qt_version": probe["qt_version"],
        "ssl_backend": probe["ssl_backend"],
        "qt_dependencies": dependencies,
        "gui_previews": previews,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", required=True, type=Path)
    parser.add_argument("--version", required=True)
    parser.add_argument("--gui", action="store_true", help="require and exercise the GUI build")
    parser.add_argument("--target", help="native release target, for example linux/amd64")
    args = parser.parse_args(argv)
    scratch_parent = Path(__file__).resolve().parents[1] / ".scratch"
    try:
        proof = validate_stage(args.stage, args.version, args.gui, args.target, scratch_parent)
    except ValidationError as exc:
        print(json.dumps({"status": "error", "error": str(exc)}, separators=(",", ":")), file=sys.stderr)
        return 1
    except OSError:
        print(json.dumps({"status": "error", "error": "could not inspect the runtime stage"},
                         separators=(",", ":")), file=sys.stderr)
        return 1
    print(json.dumps(proof, separators=(",", ":"), sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
