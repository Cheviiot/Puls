"""Tests of the release packaging tool."""

import hashlib
import json
import os
import sys
import tarfile
import tempfile
import time
import unittest
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import release  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent.parent


def write(path, content, executable=False):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content if isinstance(content, bytes) else content.encode())
    os.chmod(path, 0o755 if executable else 0o644)


def make_stage(directory, system, gui=True):
    """Creates the layout of cmake --install with PULS_PORTABLE_INSTALL."""
    stage = Path(directory)
    for document in release.DOCUMENTS:
        write(stage / document, f"{document}\n")
    cli, gui_path = release.program_paths(system)
    write(stage / cli, b"#!/bin/sh\nexit 0\n", executable=True)
    if gui:
        write(stage / gui_path, b"#!/bin/sh\nexit 0\n", executable=True)
        if system == "darwin":
            write(stage / "Puls.app/Contents/Info.plist", "<plist/>\n")
        elif system == "linux":
            write(stage / "assets/Icon.png", b"png")
    return stage


class PackageTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.directory = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def test_tar_archive_is_reproducible(self):
        stage = make_stage(self.directory / "stage", "linux")
        first, gui = release.package("1.2.3", "linux", "amd64", stage, self.directory / "a")
        self.assertTrue(gui)
        # Timestamps of the inputs do not change the archive.
        for path in stage.rglob("*"):
            os.utime(path, (time.time() - 1000, time.time() - 1000))
        second, _ = release.package("1.2.3", "linux", "amd64", stage, self.directory / "b")
        self.assertEqual(first.name, "Puls_1.2.3_linux_amd64.tar.gz")
        self.assertEqual(first.read_bytes(), second.read_bytes())
        self.assertEqual(first.read_bytes()[4:8], b"\0\0\0\0")

        with tarfile.open(first, "r:gz") as archive:
            members = archive.getmembers()
        names = [member.name for member in members]
        top = "Puls_1.2.3_linux_amd64"
        self.assertEqual(names, sorted(names))
        self.assertEqual(names[0], top)
        modes = {member.name: member.mode for member in members}
        self.assertEqual(modes[f"{top}/puls"], 0o755)
        self.assertEqual(modes[f"{top}/puls-gui"], 0o755)
        self.assertEqual(modes[f"{top}/README.md"], 0o644)
        self.assertEqual(modes[f"{top}/assets"], 0o755)
        for member in members:
            self.assertEqual((member.mtime, member.uid, member.gid), (0, 0, 0))
            self.assertEqual((member.uname, member.gname), ("", ""))

    def test_macos_archive_keeps_the_bundle(self):
        stage = make_stage(self.directory / "stage", "darwin")
        path, gui = release.package("1.2.3", "darwin", "arm64", stage, self.directory)
        self.assertTrue(gui)
        with tarfile.open(path, "r:gz") as archive:
            bundle = archive.getmember("Puls_1.2.3_darwin_arm64/Puls.app/Contents/MacOS/Puls")
        self.assertEqual(bundle.mode, 0o755)

    def test_zip_archive_for_windows(self):
        stage = make_stage(self.directory / "stage", "windows")
        first, gui = release.package("1.2.3", "windows", "amd64", stage, self.directory / "a")
        second, _ = release.package("1.2.3", "windows", "amd64", stage, self.directory / "b")
        self.assertTrue(gui)
        self.assertEqual(first.read_bytes(), second.read_bytes())
        with zipfile.ZipFile(first) as archive:
            infos = {info.filename: info for info in archive.infolist()}
        top = "Puls_1.2.3_windows_amd64"
        self.assertIn(f"{top}/puls.exe", infos)
        self.assertIn(f"{top}/puls-gui.exe", infos)
        for info in infos.values():
            self.assertEqual(info.date_time, release.ZIP_TIMESTAMP)

    def test_cli_only_package(self):
        stage = make_stage(self.directory / "stage", "windows", gui=False)
        _, gui = release.package("1.2.3", "windows", "arm64", stage, self.directory)
        self.assertFalse(gui)

    def test_rejects_incomplete_stage_and_symlinks(self):
        stage = make_stage(self.directory / "stage", "linux")
        (stage / "THIRD_PARTY_NOTICES.txt").unlink()
        with self.assertRaisesRegex(release.ReleaseError, "THIRD_PARTY_NOTICES"):
            release.package("1.2.3", "linux", "amd64", stage, self.directory)
        stage = make_stage(self.directory / "stage2", "linux")
        if hasattr(os, "symlink"):
            try:
                os.symlink("puls", stage / "link")
            except OSError:
                return
            with self.assertRaisesRegex(release.ReleaseError, "символические"):
                release.package("1.2.3", "linux", "amd64", stage, self.directory)

    def test_rejects_unsafe_versions(self):
        for version in ("", ".", "..", "1.0/../x", "1 0"):
            with self.assertRaises(release.ReleaseError):
                release.check_version(version)
        release.check_version("1.2.3-rc.1_dev")


class AssembleTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.directory = Path(self.temporary.name)
        self.dist = self.directory / "dist"
        for (system, architecture), gui in release.DESKTOP_TARGETS.items():
            stage = make_stage(self.directory / f"stage-{system}-{architecture}", system, gui)
            release.package("1.2.3", system, architecture, stage, self.dist)

    def tearDown(self):
        self.temporary.cleanup()

    def test_writes_manifest_checksums_and_installers(self):
        write(self.dist / "Puls_1.2.3_android.apk", b"apk")
        manifest = release.assemble("1.2.3", self.dist, require_android=True, root=ROOT)
        written = json.loads((self.dist / release.MANIFEST_NAME).read_text())
        self.assertEqual(manifest, written)
        self.assertEqual(written["schema_version"], 3)
        self.assertEqual(written["product"], "Puls")
        files = [asset["file"] for asset in written["assets"]]
        self.assertEqual(files, sorted(files))
        self.assertEqual(len(files), 7)
        by_target = {(asset["os"], asset["arch"]): asset for asset in written["assets"]}
        self.assertEqual(by_target[("windows", "arm64")]["capabilities"], ["cli"])
        self.assertEqual(by_target[("darwin", "amd64")]["capabilities"], ["cli", "gui"])
        self.assertEqual(by_target[("android", "universal")]["kind"], "apk")

        # The installers parse the manifest line by line.
        text = (self.dist / release.MANIFEST_NAME).read_text()
        self.assertIn('  "assets": [\n', text)
        self.assertIn('      "capabilities": [\n        "cli",\n        "gui"\n      ]', text)

        checksums = {}
        for line in (self.dist / release.CHECKSUMS_NAME).read_text().splitlines():
            digest, name = line.split("  ")
            checksums[name] = digest
        expected = set(files) | {release.MANIFEST_NAME, "install.sh", "install.ps1"}
        self.assertEqual(set(checksums), expected)
        self.assertEqual(list(checksums), sorted(checksums))
        for name, digest in checksums.items():
            self.assertEqual(hashlib.sha256((self.dist / name).read_bytes()).hexdigest(), digest)
        for asset in written["assets"]:
            self.assertEqual(asset["sha256"], checksums[asset["file"]])
        self.assertEqual((self.dist / "install.sh").read_bytes(),
                         (ROOT / "scripts" / "install.sh").read_bytes())
        if os.name != "nt":
            self.assertEqual((self.dist / "install.sh").stat().st_mode & 0o777, 0o755)

    def test_requires_every_target_with_expected_interface(self):
        (self.dist / "Puls_1.2.3_linux_arm64.tar.gz").unlink()
        with self.assertRaisesRegex(release.ReleaseError, "linux/arm64"):
            release.assemble("1.2.3", self.dist, root=ROOT)
        stage = make_stage(self.directory / "cli-linux", "linux", gui=False)
        release.package("1.2.3", "linux", "arm64", stage, self.dist)
        with self.assertRaisesRegex(release.ReleaseError, "с графическим интерфейсом"):
            release.assemble("1.2.3", self.dist, root=ROOT)

    def test_selected_targets_and_missing_android(self):
        targets = release.parse_targets("linux/amd64")
        with self.assertRaisesRegex(release.ReleaseError, "лишние архивы"):
            release.assemble("1.2.3", self.dist, targets, root=ROOT)
        with self.assertRaisesRegex(release.ReleaseError, "android"):
            release.assemble("1.2.3", self.dist, require_android=True, root=ROOT)
        with self.assertRaises(release.ReleaseError):
            release.parse_targets("linux")


if __name__ == "__main__":
    unittest.main()
