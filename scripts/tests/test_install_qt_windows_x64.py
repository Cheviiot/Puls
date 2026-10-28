import importlib.util
from pathlib import Path
import unittest


SCRIPT = Path(__file__).parents[1] / "install_qt_windows_x64.py"
SPEC = importlib.util.spec_from_file_location("install_qt_windows_x64", SCRIPT)
assert SPEC and SPEC.loader
qt_installer = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(qt_installer)


class QtWindowsX64MetadataTests(unittest.TestCase):
    def test_uses_verified_qt_6120_msvc_folder(self):
        self.assertEqual(
            qt_installer.metadata_folder(
                "windows", "desktop", "6.12.0", "win64_msvc2022_64", "qt6_6120/qt6_6120"
            ),
            "qt6_6120/qt6_6120_msvc2022_64",
        )

    def test_rejects_unverified_target_or_path(self):
        cases = (
            ("windows", "desktop", "6.12.0", "win64_msvc2022_arm64", "qt6_6120/qt6_6120"),
            ("windows_arm64", "desktop", "6.12.0", "win64_msvc2022_64", "qt6_6120/qt6_6120"),
            ("windows", "desktop", "6.11.3", "win64_msvc2022_64", "qt6_6113/qt6_6113"),
            ("windows", "desktop", "6.12.0", "win64_msvc2022_64", "qt6_6120/other"),
        )
        for case in cases:
            with self.subTest(case=case), self.assertRaises(RuntimeError):
                qt_installer.metadata_folder(*case)


if __name__ == "__main__":
    unittest.main()
