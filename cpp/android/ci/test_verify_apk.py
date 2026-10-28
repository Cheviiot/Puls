import importlib.util
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import zipfile


SPEC = importlib.util.spec_from_file_location("verify_apk", Path(__file__).with_name("verify_apk.py"))
VERIFY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VERIFY)


class ApkVerificationTests(unittest.TestCase):
    def setUp(self):
        scratch = Path(__file__).resolve().parents[3] / ".scratch"
        scratch.mkdir(exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="android-verifier-", dir=scratch)
        self.addCleanup(self.temporary.cleanup)
        self.apk = Path(self.temporary.name) / "puls.apk"
        self.badging = "package: name='io.github.cheviiot.puls' versionCode='4000' versionName='0.4.0-cpp-dev'\n"
        self.permissions = "uses-permission: name='android.permission.INTERNET'\n"

    def package(self, missing=None, wrong_machine=False, alignment=16384, empty_notice=False):
        with zipfile.ZipFile(self.apk, "w") as archive:
            for abi, machine in (("arm64-v8a", 183), ("x86_64", 62)):
                header = bytearray(64)
                header[:6] = b"\x7fELF\x02\x01"
                struct.pack_into("<H", header, 18, 183 if wrong_machine else machine)
                struct.pack_into("<Q", header, 32, 64)
                struct.pack_into("<HH", header, 54, 56, 1)
                program = struct.pack("<IIQQQQQQ", 1, 5, 0, 0, 0, 120, 120, alignment)
                libraries = [f"puls_{abi}", "crypto_3", "ssl_3"] + [f"{name}_{abi}" for name in (
                    "Qt6Core", "Qt6Gui", "Qt6Widgets", "Qt6Network", "Qt6WebSockets", "plugins_tls_qopensslbackend")]
                for library in libraries:
                    name = f"lib/{abi}/lib{library}.so"
                    if missing and missing in name:
                        continue
                    archive.writestr(name, header + program)
            for notice in ("OpenSSL.txt", "Puls.txt", "Qt/README.txt", "Qt/LGPL-3.0-only.txt",
                           "Qt/LGPL-2.1-or-later.txt", "Qt/GPL-3.0-only.txt",
                           "Qt/GPL-2.0-only.txt", "Qt/BSD-3-Clause.txt"):
                if missing and missing in notice:
                    continue
                archive.writestr(f"assets/licenses/{notice}", "" if empty_notice else "synthetic notice")

    def command(self, *arguments):
        if "permissions" in arguments:
            return self.permissions
        if "badging" in arguments:
            return self.badging
        return ""

    def verify(self):
        with patch.object(VERIFY, "run", side_effect=self.command):
            return VERIFY.verify(self.apk, Path("sdk-tools"), "0.4.0-cpp-dev", 4000)

    def test_complete_signed_multi_abi_package(self):
        self.package()
        result = self.verify()
        self.assertEqual(result["abis"], ["arm64-v8a", "x86_64"])
        self.assertEqual(result["permissions"], ["android.permission.INTERNET"])
        self.assertTrue(result["signature_verified"])
        self.assertEqual(result["native_page_alignment"], 16384)

    def test_additional_permission_is_rejected(self):
        self.package()
        self.permissions += "uses-permission: name='android.permission.READ_EXTERNAL_STORAGE'\n"
        with self.assertRaisesRegex(ValueError, "permissions"):
            self.verify()

    def test_missing_tls_runtime_is_rejected(self):
        self.package(missing="libssl_3")
        with self.assertRaisesRegex(ValueError, "missing"):
            self.verify()

    def test_missing_or_empty_license_is_rejected(self):
        for missing in ("OpenSSL.txt", "Puls.txt", "Qt/LGPL-3.0-only.txt", "Qt/BSD-3-Clause.txt"):
            with self.subTest(missing=missing):
                self.package(missing=missing)
                with self.assertRaisesRegex(ValueError, "license notice"):
                    self.verify()
        self.package(empty_notice=True)
        with self.assertRaisesRegex(ValueError, "empty license"):
            self.verify()

    def test_wrong_native_architecture_is_rejected(self):
        self.package(wrong_machine=True)
        with self.assertRaisesRegex(ValueError, "wrong-architecture"):
            self.verify()

    def test_4k_native_library_is_rejected(self):
        self.package(alignment=4096)
        with self.assertRaisesRegex(ValueError, "16 KiB"):
            self.verify()

    def test_elf_without_loadable_segments_is_rejected(self):
        self.package()
        with zipfile.ZipFile(self.apk) as archive:
            original = {name: archive.read(name) for name in archive.namelist()}
        name = "lib/x86_64/libQt6Network_x86_64.so"
        broken = bytearray(original[name])
        struct.pack_into("<I", broken, 64, 0)  # PT_NULL
        original[name] = broken
        with zipfile.ZipFile(self.apk, "w") as archive:
            for name, contents in original.items():
                archive.writestr(name, contents)
        with self.assertRaisesRegex(ValueError, "no loadable"):
            self.verify()

    def test_debug_package_and_wrong_version_are_rejected(self):
        self.package()
        self.badging += "application-debuggable\n"
        with self.assertRaisesRegex(ValueError, "debuggable"):
            self.verify()
        self.badging = self.badging.replace("versionCode='4000'", "versionCode='3003'")
        with self.assertRaisesRegex(ValueError, "version"):
            self.verify()

    def test_signature_verifier_failure_is_not_ignored(self):
        self.package()
        with patch.object(VERIFY, "run", side_effect=subprocess.CalledProcessError(1, "apksigner")):
            with self.assertRaises(subprocess.CalledProcessError):
                VERIFY.verify(self.apk, Path("sdk-tools"), "0.4.0-cpp-dev", 4000)


if __name__ == "__main__":
    unittest.main()
