#!/usr/bin/env python3
"""Local integration coverage for the shell runtime-tree installer."""

from __future__ import annotations

import hashlib
import http.server
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import threading
import unittest


ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "tests" / "fixtures" / "cpp_runtime"
VERSION = "1.0.0"
ARCHIVE = f"Puls_{VERSION}_linux_amd64.tar.gz"


def make_archive(version: str, payload: str, *, legacy: bool = False, traversal: bool = False, escaping_link: bool = False) -> bytes:
    name = f"Puls_{version}_linux_amd64"
    stream = io.BytesIO()
    with tarfile.open(fileobj=stream, mode="w:gz", format=tarfile.PAX_FORMAT) as archive:
        if legacy:
            files = {"puls": b"#!/bin/sh\nprintf 'legacy\\n'\n", "assets/Icon.png": b"legacy icon"}
        else:
            files = {}
            for item in FIXTURE.rglob("*"):
                if item.is_file():
                    relative = item.relative_to(FIXTURE).as_posix()
                    files[relative] = item.read_bytes()
            files["bin/puls"] = f"#!/bin/sh\nprintf '{payload}\\n'\n".encode()
        for relative, data in files.items():
            info = tarfile.TarInfo(f"{name}/{relative}")
            info.mode = 0o755 if relative.endswith("/puls") else 0o644
            info.size = len(data)
            archive.addfile(info, io.BytesIO(data))
        if not legacy:
            link = tarfile.TarInfo(f"{name}/lib/libfixture.so")
            link.type = tarfile.SYMTYPE
            link.linkname = "../../outside" if escaping_link else "runtime.data"
            archive.addfile(link)
        if traversal:
            hostile = tarfile.TarInfo(f"{name}/../escape.txt")
            hostile_data = b"escape"
            hostile.size = len(hostile_data)
            archive.addfile(hostile, io.BytesIO(hostile_data))
    return stream.getvalue()


def release_files(version: str, archive_name: str, payload: bytes) -> dict[str, bytes]:
    digest = hashlib.sha256(payload).hexdigest()
    manifest = json.dumps(
        {
            "schema_version": 2,
            "product": "Puls",
            "version": version,
            "assets": [
                {
                    "os": "linux",
                    "arch": "amd64",
                    "file": archive_name,
                    "sha256": digest,
                    "kind": "archive",
                    "capabilities": ["cli", "gui"],
                }
            ],
        },
        indent=2,
    ).encode() + b"\n"
    manifest_digest = hashlib.sha256(manifest).hexdigest()
    checksums = f"{digest}  {archive_name}\n{manifest_digest}  RELEASE_MANIFEST.json\n".encode()
    return {archive_name: payload, "RELEASE_MANIFEST.json": manifest, "SHA256SUMS.txt": checksums}


class ReleaseHandler(http.server.BaseHTTPRequestHandler):
    releases: dict[str, dict[str, bytes]] = {}

    def do_GET(self) -> None:  # noqa: N802 - stdlib handler name
        parts = self.path.strip("/").split("/")
        filename = parts[-1]
        version = parts[-2].removeprefix("v") if len(parts) >= 2 else ""
        files = self.releases.get(version)
        if files is None or filename not in files:
            self.send_error(404)
            return
        body = files[filename]
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_args: object) -> None:
        return


class RuntimeInstallerTests(unittest.TestCase):
    def setUp(self) -> None:
        for command in ("sh", "curl", "tar"):
            if shutil.which(command) is None:
                self.skipTest(f"{command} is required")
        scratch = ROOT / "scripts" / ".scratch"
        scratch.mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix="cpp-installer-", dir=scratch)
        self.root = Path(self.temp.name)
        self.home = self.root / "home"
        self.install = self.root / "custom install"
        self.home.mkdir()
        self.install.mkdir()
        (self.install / "keep.txt").write_text("user data\n", encoding="utf-8")
        self.releases = {
            "1.0.0": release_files("1.0.0", "Puls_1.0.0_linux_amd64.tar.gz", make_archive("1.0.0", "runtime one")),
            "2.0.0": release_files("2.0.0", "Puls_2.0.0_linux_amd64.tar.gz", make_archive("2.0.0", "runtime two")),
            "3.0.0": release_files("3.0.0", "Puls_3.0.0_linux_amd64.tar.gz", make_archive("3.0.0", "runtime three")),
            "0.9.0": release_files("0.9.0", "Puls_0.9.0_linux_amd64.tar.gz", make_archive("0.9.0", "unused", legacy=True)),
            "4.0.0": release_files("4.0.0", "Puls_4.0.0_linux_amd64.tar.gz", make_archive("4.0.0", "unused", traversal=True)),
            "5.0.0": release_files("5.0.0", "Puls_5.0.0_linux_amd64.tar.gz", make_archive("5.0.0", "unused", escaping_link=True)),
        }
        ReleaseHandler.releases = self.releases
        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), ReleaseHandler)
        self.server_thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.server_thread.start()
        self.url = f"http://127.0.0.1:{self.server.server_port}"

    def tearDown(self) -> None:
        self.server.shutdown()
        self.server.server_close()
        self.server_thread.join(timeout=2)
        self.temp.cleanup()

    def run_installer(self, version: str, *extra: str, expect_ok: bool = True) -> subprocess.CompletedProcess[str]:
        env = os.environ.copy()
        env.update(
            HOME=str(self.home),
            SHELL="/bin/bash",
            XDG_DATA_HOME=str(self.home / ".local" / "share"),
            PULS_INSTALL_DIR=str(self.install),
            PULS_INSTALL_REPOSITORY_URL=self.url,
        )
        command = ["sh", str(ROOT / "scripts" / "install.sh"), "--version", version, "--no-path-update", *extra]
        result = subprocess.run(command, env=env, text=True, capture_output=True, check=False)
        if expect_ok and result.returncode:
            self.fail(f"installer failed ({result.returncode}):\n{result.stdout}\n{result.stderr}")
        if not expect_ok and result.returncode == 0:
            self.fail(f"installer unexpectedly succeeded:\n{result.stdout}\n{result.stderr}")
        return result

    def test_runtime_install_update_rollback_and_uninstall(self) -> None:
        self.run_installer("1.0.0", "--no-shortcut")
        launcher = self.install / "puls"
        self.assertTrue(launcher.is_symlink())
        self.assertEqual(os.readlink(launcher), ".puls-runtime/current/bin/puls")
        self.assertEqual(subprocess.check_output([launcher], text=True), "runtime one\n")
        self.assertEqual((self.install / ".puls-runtime/versions/1.0.0/lib/runtime.data").read_text(), "fixture library payload\n")
        self.assertTrue((self.install / ".puls-runtime/versions/1.0.0/lib/libfixture.so").is_symlink())
        self.assertFalse((self.home / ".local/share/applications/io.github.cheviiot.puls.desktop").exists())

        self.run_installer("2.0.0", "--no-shortcut")
        self.assertEqual((self.install / ".puls-runtime/current").resolve().name, "2.0.0")
        self.assertEqual(subprocess.check_output([launcher], text=True), "runtime two\n")
        self.assertFalse((self.install / ".puls-runtime/versions/1.0.0").exists())

        launcher.unlink()
        launcher.mkdir()
        result = self.run_installer("3.0.0", "--no-shortcut", expect_ok=False)
        self.assertIn("не является обычным файлом", result.stderr)
        self.assertEqual((self.install / ".puls-runtime/current").resolve().name, "2.0.0")
        self.assertTrue(launcher.is_dir())
        launcher.rmdir()
        self.install.joinpath("puls").symlink_to(".puls-runtime/current/bin/puls")

        env = os.environ.copy()
        env.update(
            HOME=str(self.home),
            SHELL="/bin/bash",
            PULS_INSTALL_DIR=str(self.install),
            PULS_INSTALL_REPOSITORY_URL=self.url,
        )
        result = subprocess.run(["sh", str(ROOT / "scripts" / "install.sh"), "--version", "1.0.0"], env=env, text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(str(self.install), (self.home / ".bashrc").read_text())
        self.assertTrue((self.home / ".local/share/applications/io.github.cheviiot.puls.desktop").is_file())

        result = subprocess.run(["sh", str(ROOT / "scripts" / "install.sh"), "--uninstall", "--no-path-update"], env=env, text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.install / "keep.txt").read_text(), "user data\n")
        self.assertFalse((self.install / "puls").exists())
        self.assertFalse((self.install / ".puls-runtime").exists())

    def test_legacy_archive_still_installs_as_single_binary(self) -> None:
        self.run_installer("0.9.0", "--no-shortcut")
        self.assertFalse((self.install / "puls").is_symlink())
        self.assertEqual(subprocess.check_output([self.install / "puls"], text=True), "legacy\n")

    def test_runtime_archive_traversal_is_rejected_before_install(self) -> None:
        result = self.run_installer("4.0.0", "--no-shortcut", expect_ok=False)
        self.assertIn("архив содержит небезопасные пути", result.stderr)
        self.assertFalse((self.install / "puls").exists())

    def test_runtime_symlink_cannot_escape_version_directory(self) -> None:
        result = self.run_installer("5.0.0", "--no-shortcut", expect_ok=False)
        self.assertIn("runtime содержит ссылку за пределы каталога", result.stderr)
        self.assertFalse((self.install / ".puls-runtime/versions/5.0.0").exists())
        self.assertFalse((self.root / "outside").exists())


if __name__ == "__main__":
    unittest.main()
