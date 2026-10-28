import hashlib
import io
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
        scratch = SCRIPT.parent.parent / ".scratch"
        scratch.mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix="puls-release-test-", dir=scratch)
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

    def make_six_archives(self, input_dir, version="2.0"):
        input_dir.mkdir(parents=True, exist_ok=True)
        for os_name, arch in release_cpp.TARGETS:
            stage = self.stage(f"stage-{version}-{os_name}-{arch}", os_name)
            release_cpp.create_archive(stage, input_dir, version, os_name, arch,
                                       (os_name, arch) == ("windows", "arm64"))

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
        (stage / "bin" / "puls.exe").chmod(0o644)
        path = release_cpp.create_archive(stage, self.root / "out", "1.2.3", "windows", "amd64", False)
        with zipfile.ZipFile(path) as archive:
            binary = next(i for i in archive.infolist() if i.filename.endswith("/bin/puls.exe"))
            self.assertEqual(binary.date_time, (1980, 1, 1, 0, 0, 0))
            self.assertEqual((binary.external_attr >> 16) & 0o777, 0o755)
            self.assertEqual(binary.compress_type, zipfile.ZIP_DEFLATED)

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
        release_cpp.validate_archive(archive, "Puls_1_linux_amd64", "linux", "amd64")
        with tarfile.open(archive, "r:gz") as tar:
            link = next(item for item in tar.getmembers() if item.name.endswith("/alias.so"))
            self.assertTrue(link.issym())
            self.assertEqual(link.linkname, "libQt.so")

    def test_internal_symlink_chain_is_preserved_but_dangling_and_cycles_fail(self):
        stage = self.stage()
        (stage / "lib" / "alias.so").symlink_to("libQt.so")
        (stage / "lib" / "alias2.so").symlink_to("alias.so")
        release_cpp.collect_stage(stage)
        (stage / "lib" / "dangling").symlink_to("missing")
        with self.assertRaisesRegex(release_cpp.ReleaseError, "dangling or cyclic"):
            release_cpp.collect_stage(stage)
        (stage / "lib" / "dangling").unlink()
        (stage / "lib" / "cycle-a").symlink_to("cycle-b")
        (stage / "lib" / "cycle-b").symlink_to("cycle-a")
        with self.assertRaisesRegex(release_cpp.ReleaseError, "dangling or cyclic"):
            release_cpp.collect_stage(stage)

    def test_archive_paths_reject_windows_separators_ads_and_controls(self):
        for value in ("root\\file", "root/file:stream", "root/file\x00", "root/file\n"):
            with self.subTest(value=value), self.assertRaises(release_cpp.ReleaseError):
                release_cpp.safe_relative(value)
        self.assertEqual(release_cpp.safe_relative("root//./dir/").as_posix(), "root/dir")

    def test_duplicate_canonical_and_casefolded_zip_members_are_rejected(self):
        for index, second in enumerate(("Puls_1_windows_amd64/bin/./puls.exe",
                                        "Puls_1_windows_amd64/BIN/puls.exe")):
            path = self.root / f"duplicate-{index}.zip"
            with zipfile.ZipFile(path, "w") as archive:
                for name in ("Puls_1_windows_amd64/bin/puls.exe", second):
                    info = zipfile.ZipInfo(name)
                    info.create_system = 3
                    info.external_attr = (stat.S_IFREG | 0o755) << 16
                    archive.writestr(info, b"puls")
            with self.subTest(second=second), self.assertRaisesRegex(release_cpp.ReleaseError, "duplicate path"):
                release_cpp.validate_archive(path, "Puls_1_windows_amd64", "windows", "amd64")

    def test_duplicate_tar_members_are_rejected(self):
        path = self.root / "duplicate.tar.gz"
        with tarfile.open(path, "w:gz") as archive:
            for name in ("Puls_1_linux_amd64/bin/puls", "Puls_1_linux_amd64/bin//./puls"):
                info = tarfile.TarInfo(name)
                info.mode, info.size = 0o755, 1
                archive.addfile(info, io.BytesIO(b"x"))
        with self.assertRaisesRegex(release_cpp.ReleaseError, "duplicate path"):
            release_cpp.validate_archive(path, "Puls_1_linux_amd64", "linux", "amd64")

    def test_malicious_zip_and_tar_member_names_are_rejected(self):
        bad_names = ("Puls_1_linux_amd64/bin\\puls", "Puls_1_linux_amd64/bin/puls:stream",
                     "Puls_1_linux_amd64/bin/puls\x01")
        for index, name in enumerate(bad_names):
            zip_path = self.root / f"unsafe-{index}.zip"
            with zipfile.ZipFile(zip_path, "w") as archive:
                archive.writestr(name, b"x")
            with self.subTest(kind="zip", name=repr(name)), self.assertRaises(release_cpp.ReleaseError):
                release_cpp.validate_archive(zip_path, "Puls_1_linux_amd64", "linux", "amd64")
            tar_path = self.root / f"unsafe-{index}.tar.gz"
            with tarfile.open(tar_path, "w:gz") as archive:
                info = tarfile.TarInfo(name)
                info.size = 1
                archive.addfile(info, io.BytesIO(b"x"))
            with self.subTest(kind="tar", name=repr(name)), self.assertRaises(release_cpp.ReleaseError):
                release_cpp.validate_archive(tar_path, "Puls_1_linux_amd64", "linux", "amd64")

    def test_archive_symlink_chain_cannot_escape_after_resolution(self):
        for suffix, windows in (("zip", True), ("tar.gz", False)):
            path = self.root / f"chain.{suffix}"
            root = "Puls_1_windows_amd64" if windows else "Puls_1_linux_amd64"
            exe = f"{root}/bin/puls.exe" if windows else f"{root}/bin/puls"
            if windows:
                with zipfile.ZipFile(path, "w") as archive:
                    binary = zipfile.ZipInfo(exe)
                    binary.create_system = 3
                    binary.external_attr = (stat.S_IFREG | 0o755) << 16
                    archive.writestr(binary, b"x")
                    for name, target in ((f"{root}/a", "."), (f"{root}/deep/link", "../a/../outside")):
                        info = zipfile.ZipInfo(name)
                        info.create_system = 3
                        info.external_attr = (stat.S_IFLNK | 0o777) << 16
                        archive.writestr(info, target)
            else:
                with tarfile.open(path, "w:gz") as archive:
                    binary = tarfile.TarInfo(exe)
                    binary.mode, binary.size = 0o755, 1
                    archive.addfile(binary, io.BytesIO(b"x"))
                    for name, target in ((f"{root}/a", "."), (f"{root}/deep/link", "../a/../outside")):
                        info = tarfile.TarInfo(name)
                        info.type, info.linkname = tarfile.SYMTYPE, target
                        archive.addfile(info)
            with self.subTest(suffix=suffix), self.assertRaisesRegex(release_cpp.ReleaseError, "escapes package root"):
                release_cpp.validate_archive(path, root, "windows" if windows else "linux", "amd64")

    def test_archive_executable_must_be_a_regular_file(self):
        for suffix, windows in (("zip", True), ("tar.gz", False)):
            path = self.root / f"exe-link.{suffix}"
            root = "Puls_1_windows_amd64" if windows else "Puls_1_linux_amd64"
            exe = f"{root}/bin/puls.exe" if windows else f"{root}/bin/puls"
            real = f"{root}/bin/real"
            if windows:
                with zipfile.ZipFile(path, "w") as archive:
                    target = zipfile.ZipInfo(real)
                    target.create_system = 3
                    target.external_attr = (stat.S_IFREG | 0o755) << 16
                    archive.writestr(target, b"puls")
                    link = zipfile.ZipInfo(exe)
                    link.create_system = 3
                    link.external_attr = (stat.S_IFLNK | 0o777) << 16
                    archive.writestr(link, "real")
            else:
                with tarfile.open(path, "w:gz") as archive:
                    target = tarfile.TarInfo(real)
                    target.mode, target.size = 0o755, 1
                    archive.addfile(target, io.BytesIO(b"x"))
                    link = tarfile.TarInfo(exe)
                    link.type, link.linkname = tarfile.SYMTYPE, "real"
                    archive.addfile(link)
            with self.subTest(suffix=suffix), self.assertRaisesRegex(release_cpp.ReleaseError, "regular executable"):
                release_cpp.validate_archive(path, root, "windows" if windows else "linux", "amd64")

    def test_escaping_symlink_and_special_file_are_rejected(self):
        stage = self.stage()
        (stage / "escape").symlink_to("../../outside")
        with self.assertRaisesRegex(release_cpp.ReleaseError, "escapes stage"):
            release_cpp.collect_stage(stage)
        (stage / "escape").unlink()
        (stage / "shortcut").symlink_to(".")
        outside = stage.parent / "outside"
        outside.write_text("outside")
        (stage / "deep").mkdir()
        (stage / "deep" / "chain-escape").symlink_to("../shortcut/../outside")
        with self.assertRaisesRegex(release_cpp.ReleaseError, "escapes stage"):
            release_cpp.collect_stage(stage)
        (stage / "deep" / "chain-escape").unlink()
        (stage / "shortcut").unlink()
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
        self.make_six_archives(inp)
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
        release_cpp.assemble("2.0", inp, out, sh, ps1, None)

    def test_assemble_preflights_all_outputs_before_writing(self):
        inp, out = self.root / "input", self.root / "published"
        self.make_six_archives(inp)
        out.mkdir()
        (out / "install.sh").write_bytes(b"unrelated existing file")
        sh = self.root / "install.sh"
        sh.write_bytes(b"#!/bin/sh\ntrue\n")
        with self.assertRaisesRegex(release_cpp.ReleaseError, "unrelated output"):
            release_cpp.assemble("2.0", inp, out, sh, None, None)
        self.assertEqual(list(out.iterdir()), [out / "install.sh"])

    def test_assemble_rejects_matching_archive_symlink_destination(self):
        inp, out = self.root / "input", self.root / "published"
        self.make_six_archives(inp)
        out.mkdir()
        name = release_cpp.archive_name("2.0", "linux", "amd64")
        (out / name).symlink_to(inp / name)
        with self.assertRaisesRegex(release_cpp.ReleaseError, "non-regular output"):
            release_cpp.assemble("2.0", inp, out, None, None, None)
        self.assertEqual(len(list(out.iterdir())), 1)

    def test_assemble_refuses_unowned_manifest_without_copying_archives(self):
        inp, out = self.root / "input", self.root / "published"
        self.make_six_archives(inp)
        out.mkdir()
        (out / release_cpp.MANIFEST).write_text("unrelated")
        with self.assertRaisesRegex(release_cpp.ReleaseError, "unrelated output"):
            release_cpp.assemble("2.0", inp, out, None, None, None)
        self.assertEqual(list(out.iterdir()), [out / release_cpp.MANIFEST])

    def test_assemble_requires_all_archives_and_rejects_ps1_bom(self):
        inp = self.root / "input"
        inp.mkdir()
        with self.assertRaisesRegex(release_cpp.ReleaseError, "required archive missing"):
            release_cpp.assemble("1", inp, self.root / "out", None, None, None)
        self.assertFalse((self.root / "out").exists())
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
