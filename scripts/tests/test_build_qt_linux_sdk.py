import hashlib
import importlib.util
from pathlib import Path
import tarfile
import tempfile
import unittest


SCRIPT = Path(__file__).parents[1] / "build_qt_linux_sdk.py"
SCRATCH = Path(__file__).parents[2] / ".scratch" / "qt-sdk-test-temp"
SCRATCH.mkdir(parents=True, exist_ok=True)
SPEC = importlib.util.spec_from_file_location("build_qt_linux_sdk", SCRIPT)
assert SPEC and SPEC.loader
qt_builder = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(qt_builder)


class QtLinuxSdkHelperTests(unittest.TestCase):
    def test_sha256_file(self):
        with tempfile.TemporaryDirectory(dir=SCRATCH) as temp_dir:
            source = Path(temp_dir) / "payload"
            source.write_bytes(b"verified fixture")
            self.assertEqual(qt_builder.sha256_file(source), hashlib.sha256(b"verified fixture").hexdigest())

    def test_extracts_minimal_source_fixture(self):
        with tempfile.TemporaryDirectory(dir=SCRATCH) as temp_dir:
            root = Path(temp_dir)
            archive = root / "qt.tar.xz"
            source_root = root / qt_builder.SOURCE_ROOT
            (source_root / "qtbase").mkdir(parents=True)
            (source_root / "qtwebsockets").mkdir()
            (source_root / "qtwayland").mkdir()
            for relative in (
                "configure",
                "CMakeLists.txt",
                "qtbase/.cmake.conf",
                "qtbase/CMakeLists.txt",
                "qtwebsockets/CMakeLists.txt",
                "qtwayland/CMakeLists.txt",
            ):
                (source_root / relative).write_text("fixture", encoding="utf-8")
            with tarfile.open(archive, "w:xz") as bundle:
                bundle.add(source_root, arcname=source_root.name)

            extracted = qt_builder.extract_source(archive, root / "extracted")
            self.assertEqual(extracted.name, qt_builder.SOURCE_ROOT)
            self.assertTrue((extracted / "CMakeLists.txt").is_file())
            self.assertTrue((extracted / "qtbase/.cmake.conf").is_file())
            self.assertTrue((extracted / "qtbase/CMakeLists.txt").is_file())
            self.assertTrue((extracted / "qtwebsockets/CMakeLists.txt").is_file())
            self.assertTrue((extracted / "qtwayland/CMakeLists.txt").is_file())

    def test_rejects_archive_path_traversal(self):
        with tempfile.TemporaryDirectory(dir=SCRATCH) as temp_dir:
            root = Path(temp_dir)
            archive = root / "malicious.tar.xz"
            fixture = root / "payload"
            fixture.write_text("unsafe", encoding="utf-8")
            with tarfile.open(archive, "w:xz") as bundle:
                info = tarfile.TarInfo(f"{qt_builder.SOURCE_ROOT}/../../escape")
                info.size = fixture.stat().st_size
                with fixture.open("rb") as payload:
                    bundle.addfile(info, payload)

            with self.assertRaises(qt_builder.BuildError):
                qt_builder.extract_source(archive, root / "extracted")
            self.assertFalse((root / "escape").exists())


if __name__ == "__main__":
    unittest.main()
