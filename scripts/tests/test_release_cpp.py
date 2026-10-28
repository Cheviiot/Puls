import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zipfile

SCRIPT = Path(__file__).resolve().parents[1] / "release_cpp.py"
sys.path.insert(0, str(SCRIPT.parent))
import release_cpp


class ReleaseCppTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="puls-release-test-")
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def stage(self, name="stage", os_name="linux"):
        root = self.root / name
        exe = root / "bin" / ("puls.exe" if os_name == "windows" else "puls")
        exe.parent.mkdir(parents=True)
        exe.write_bytes(b"puls executable")
        exe.chmod(0o755)
        (root / "lib").mkdir()
        (root / "lib" / "libQt.so").write_bytes(b"qt runtime")
        return root

    def test_archives_are_reproducible_and_normalize_modes(self):
        stage = self.stage()
        output = self.root / "out"
        first = release_cpp.create_archive(stage, output, "1.2.3", "linux", "amd64", False)
        self.assertEqual(stat.S_IMODE(first.stat().st_mode), 0o644)
        payload = first.read_bytes()
        with tarfile.open(first, "r:gz") as archive:
            members = archive.getmembers()
            binary = next(m for m in members if m.name.endswith("/bin/puls"))
            self.assertEqual(binary.mode, 0o755)
            self.assertEqual(binary.uid, 0)
            self.assertEqual(binary.gid, 0)
            self.assertEqual(binary.mtime, 0)
            self.assertEqual(members, sorted(members, key=lambda m: m.name))
        first.unlink()
        second = release_cpp.create_archive(stage, output, "1.2.3", "linux", "amd64", False)
        self.assertEqual(payload, second.read_bytes())

    def test_zip_metadata_is_normalized(self):
        stage = self.stage(os_name="windows")
        path = release_cpp.create_archive(stage, self.root / "out", "1.2.3", "windows", "amd64", False)
        with zipfile.ZipFile(path) as archive:
            binary = next(i for i in archive.infolist() if i.filename.endswith("/bin/puls.exe"))
            self.assertEqual(binary.date_time, (1980, 1, 1, 0, 0, 0))
            self.assertEqual((binary.external_attr >> 16) & 0o777, 0o755)

    def test_source_date_epoch_sets_tar_mtime(self):
        stage = self.stage()
        previous = os.environ.get("SOURCE_DATE_EPOCH")
        os.environ["SOURCE_DATE_EPOCH"] = "123456789"
        try:
            path = release_cpp.create_archive(stage, self.root / "epoch", "1", "linux", "amd64", False)
        finally:
            if previous is None:
                os.environ.pop("SOURCE_DATE_EPOCH", None)
            else:
                os.environ["SOURCE_DATE_EPOCH"] = previous
        with tarfile.open(path, "r:gz") as archive:
            self.assertTrue(all(item.mtime == 123456789 for item in archive.getmembers()))

    def test_safe_internal_symlink_is_preserved(self):
        stage = self.stage()
        (stage / "lib" / "alias.so").symlink_to("libQt.so")
        archive = release_cpp.create_archive(stage, self.root / "out", "1", "linux", "amd64", False)
        with tarfile.open(archive, "r:gz") as tar:
            link = next(item for item in tar.getmembers() if item.name.endswith("/alias.so"))
            self.assertTrue(link.issym())
            self.assertEqual(link.linkname, "libQt.so")

    def test_escaping_symlink_and_special_file_are_rejected(self):
        stage = self.stage()
        (stage / "escape").symlink_to("../../outside")
        with self.assertRaisesRegex(release_cpp.ReleaseError, "escapes stage"):
            release_cpp.collect_stage(stage)
        (stage / "escape").unlink()
        if hasattr(os, "mkfifo"):
            os.mkfifo(stage / "pipe")
            with self.assertRaisesRegex(release_cpp.ReleaseError, "special file"):
                release_cpp.collect_stage(stage)

    def test_invalid_version_target_and_missing_executable(self):
        stage = self.stage()
        with self.assertRaises(release_cpp.ReleaseError):
            release_cpp.create_archive(stage, self.root / "out", "../1", "linux", "amd64", False)
        with self.assertRaises(release_cpp.ReleaseError):
            release_cpp.create_archive(stage, self.root / "out", "1", "freebsd", "amd64", False)
        with self.assertRaisesRegex(release_cpp.ReleaseError, "CLI-only"):
            release_cpp.create_archive(stage, self.root / "out", "1", "windows", "arm64", False)
        with self.assertRaisesRegex(release_cpp.ReleaseError, "only supported"):
            release_cpp.create_archive(stage, self.root / "out", "1", "linux", "amd64", True)
        (stage / "bin" / "puls").unlink()
        with self.assertRaisesRegex(release_cpp.ReleaseError, "missing executable"):
            release_cpp.create_archive(stage, self.root / "out", "1", "linux", "amd64", False)

    def test_assemble_six_archives_manifest_checksums_and_nogui(self):
        inp, out = self.root / "input", self.root / "published"
        inp.mkdir()
        for os_name, arch in release_cpp.TARGETS:
            stage = self.stage(f"stage-{os_name}-{arch}", os_name)
            release_cpp.create_archive(stage, inp, "2.0", os_name, arch, (os_name, arch) == ("windows", "arm64"))
        sh = self.root / "install.sh"
        ps1 = self.root / "install.ps1"
        sh.write_bytes(b"#!/bin/sh\necho ok\n")
        ps1.write_bytes(b"Write-Output 'ok'\n")
        release_cpp.assemble("2.0", inp, out, sh, ps1, None)
        manifest = json.loads((out / release_cpp.MANIFEST).read_text())
        self.assertEqual(stat.S_IMODE((out / release_cpp.MANIFEST).stat().st_mode), 0o644)
        self.assertEqual(manifest["schema_version"], 2)
        self.assertEqual(len(manifest["assets"]), 6)
        arm = next(a for a in manifest["assets"] if a["os"] == "windows" and a["arch"] == "arm64")
        self.assertEqual(arm["capabilities"], ["cli"])
        entries = [line.split("  ", 1)[1] for line in (out / release_cpp.SUMS).read_text().splitlines()]
        self.assertEqual(entries, sorted(entries))
        for line in (out / release_cpp.SUMS).read_text().splitlines():
            expected, name = line.split("  ", 1)
            self.assertEqual(hashlib.sha256((out / name).read_bytes()).hexdigest(), expected)

    def test_assemble_requires_all_archives_and_rejects_ps1_bom(self):
        inp = self.root / "input"
        inp.mkdir()
        with self.assertRaisesRegex(release_cpp.ReleaseError, "required archive missing"):
            release_cpp.assemble("1", inp, self.root / "out", None, None, None)
        ps1 = self.root / "bad.ps1"
        ps1.write_bytes(b"\xef\xbb\xbfWrite-Output 'bad'")
        with self.assertRaisesRegex(release_cpp.ReleaseError, "ASCII without BOM"):
            release_cpp.assemble("1", inp, self.root / "out", None, ps1, None)

    def test_cli(self):
        result = subprocess.run([sys.executable, str(SCRIPT), "package", "--version", "1", "--target", "linux/amd64",
                                 "--stage", str(self.stage()), "--output", str(self.root / "out")],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
