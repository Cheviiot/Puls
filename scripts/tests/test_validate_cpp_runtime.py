from __future__ import annotations

import contextlib
import io
import os
import struct
from pathlib import Path
import sys
import subprocess
import tempfile
import unittest
from unittest import mock

SCRIPT = Path(__file__).resolve().parents[1] / "validate_cpp_runtime.py"
sys.path.insert(0, str(SCRIPT.parent))
import validate_cpp_runtime as runtime


class RuntimeValidatorTests(unittest.TestCase):
    def setUp(self):
        scratch = SCRIPT.parent.parent / ".scratch"
        scratch.mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix="puls-runtime-validator-", dir=scratch)
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def executable(self, name="puls"):
        path = self.root / name
        path.write_bytes(b"\0" * 64)
        path.chmod(0o755)
        return path

    @staticmethod
    def elf_header(machine=62, byteorder="little", elf_class=2):
        header = bytearray(64)
        header[:4] = b"\x7fELF"
        header[4] = elf_class
        header[5] = 1 if byteorder == "little" else 2
        header[18:20] = machine.to_bytes(2, byteorder)
        return bytes(header)

    @staticmethod
    def pe_header(machine=0x8664):
        header = bytearray(64 + 24 + 240)
        header[:2] = b"MZ"
        header[0x3C:0x40] = (64).to_bytes(4, "little")
        header[64:70] = b"PE\0\0" + machine.to_bytes(2, "little")
        header[64 + 24 - 4:64 + 24 - 2] = (240).to_bytes(2, "little")
        header[64 + 24:64 + 26] = b"\x0b\x02"
        return bytes(header)

    @staticmethod
    def macho_header(cpu=0x01000007, byteorder="little"):
        header = bytearray(32)
        header[:4] = b"\xcf\xfa\xed\xfe" if byteorder == "little" else b"\xfe\xed\xfa\xcf"
        header[4:8] = cpu.to_bytes(4, byteorder)
        return bytes(header)

    def test_detects_native_64_bit_headers_and_target_architectures(self):
        cases = (
            (self.elf_header(), "ELF64", {"x86_64"}),
            (self.elf_header(183), "ELF64", {"arm64"}),
            (self.pe_header(), "PE", {"x86_64"}),
            (self.pe_header(0xAA64), "PE", {"arm64"}),
            (self.macho_header(), "Mach-O64", {"x86_64"}),
            (self.macho_header(0x0100000C), "Mach-O64", {"arm64"}),
        )
        for index, (contents, wanted_format, wanted_arches) in enumerate(cases):
            path = self.executable(f"native-{index}")
            path.write_bytes(contents)
            self.assertEqual(runtime.binary_format(path), (wanted_format, wanted_arches))

    def test_detects_fat_macho_architectures(self):
        header = bytearray(8 + 2 * 20)
        header[:4] = b"\xca\xfe\xba\xbe"
        header[4:8] = (2).to_bytes(4, "big")
        header[8:12] = (0x01000007).to_bytes(4, "big")
        header[28:32] = (0x0100000C).to_bytes(4, "big")
        path = self.executable("universal")
        path.write_bytes(header)
        self.assertEqual(runtime.binary_format(path), ("Mach-O universal", {"x86_64", "arm64"}))

    def test_rejects_wrappers_truncated_headers_and_wrong_target(self):
        path = self.executable()
        path.write_bytes(b"#!/bin/sh\necho fake success\n")
        with self.assertRaisesRegex(runtime.ValidationError, "not an ELF"):
            runtime.binary_format(path)
        path.write_bytes(self.elf_header(62, elf_class=1))
        with self.assertRaisesRegex(runtime.ValidationError, "64-bit"):
            runtime.binary_format(path)
        path.write_bytes(self.elf_header(62))
        self.assertNotIn("arm64", runtime.binary_format(path)[1])
        with self.assertRaises(runtime.ValidationError):
            runtime.parse_target("linux/mips")

    def test_stage_binary_symlink_is_rejected(self):
        stage = self.root / "stage"
        binary_dir = stage / "bin"
        binary_dir.mkdir(parents=True)
        external = self.executable("external-puls")
        external.write_bytes(self.elf_header())
        try:
            (binary_dir / "puls").symlink_to(external)
        except OSError:
            self.skipTest("symlinks unavailable")
        with self.assertRaisesRegex(runtime.ValidationError, "regular file"):
            runtime.validate_stage(stage, "0.4.0", False, "linux/amd64", self.root)

    def test_environment_removes_qt_influences_without_dropping_tools(self):
        sdk_bin = self.root / "Qt" / "6.11.2" / "gcc_64" / "bin"
        sdk_bin.mkdir(parents=True)
        stage_bin = self.root / "stage" / "bin"
        stage_bin.mkdir(parents=True)
        python_bin = self.root / "python-tools"
        python_bin.mkdir()
        source = {
            "PATH": os.pathsep.join((str(sdk_bin), str(stage_bin), str(python_bin))),
            "QTDIR": str(self.root / "Qt"),
            "QT_PLUGIN_PATH": "/wrong/plugins",
            "QT_QPA_PLATFORM_PLUGIN_PATH": "/wrong/platforms",
            "QT_QPA_PLATFORMTHEME": "wrong-theme",
            "QML2_IMPORT_PATH": "/wrong/qml",
            "LD_LIBRARY_PATH": "/wrong/lib",
            "DYLD_LIBRARY_PATH": "/wrong/dylib",
            "DYLD_FRAMEWORK_PATH": "/wrong/frameworks",
            "PULS_GUI_PREVIEW_DIR": "/old/previews",
            "PULS_RUNTIME_PROBE": "1",
            "QT_SCALE_FACTOR": "4",
            "QT_SCREEN_SCALE_FACTORS": "4",
            "KEEP_ME": "value",
        }
        clean = runtime.sanitize_environment(source, stage_bin)
        entries = clean["PATH"].split(os.pathsep)
        self.assertEqual(entries, [str(stage_bin), str(python_bin)])
        self.assertEqual(clean["KEEP_ME"], "value")
        for name in source:
            if name.startswith(("QT_", "QML2_", "DYLD_", "LD_LIBRARY_", "PULS_")) and name != "PATH":
                self.assertNotIn(name, clean)

    def test_stage_containment_resolves_symlinks_and_parent_segments(self):
        stage = self.root / "stage"
        inside = stage / "plugins"
        outside = self.root / "plugins"
        inside.mkdir(parents=True)
        outside.mkdir()
        self.assertTrue(runtime.is_within(stage, inside / "platforms"))
        self.assertFalse(runtime.is_within(stage, stage / ".." / "plugins"))
        link = stage / "external"
        try:
            link.symlink_to(outside, target_is_directory=True)
        except OSError:
            self.skipTest("symlinks unavailable")
        self.assertFalse(runtime.is_within(stage, link))

    def test_fake_stage_fails_before_any_child_runtime_can_claim_success(self):
        stage = self.root / "stage"
        binary = stage / "bin" / "puls"
        binary.parent.mkdir(parents=True)
        binary.write_text("#!/bin/sh\necho '{\"ssl_available\":true}'\n", encoding="utf-8")
        binary.chmod(0o755)
        stderr = io.StringIO()
        with mock.patch.object(runtime, "_run", side_effect=AssertionError("fake executable was launched")):
            with contextlib.redirect_stderr(stderr):
                exit_code = runtime.main(["--stage", str(stage), "--version", "0.4.0", "--target", "linux/amd64"])
        self.assertEqual(exit_code, 1)
        self.assertIn('"status":"error"', stderr.getvalue())
        self.assertNotIn("ssl_available", stderr.getvalue())

        missing_stderr = io.StringIO()
        with contextlib.redirect_stderr(missing_stderr):
            missing_code = runtime.main(["--stage", str(self.root / "missing"), "--version", "0.4.0",
                                         "--target", "linux/amd64"])
        self.assertEqual(missing_code, 1)
        self.assertIn('"status":"error"', missing_stderr.getvalue())
        self.assertNotIn("Traceback", missing_stderr.getvalue())


    def test_glib_dependencies_and_relocated_paths_with_spaces_are_supported(self):
        stage = self.root / "relocated runtime"
        library_dir = stage / "lib"
        library_dir.mkdir(parents=True)
        (library_dir / "libglib-2.0.so.0").write_bytes(b"GLib is not GTK")
        (library_dir / "libgobject-2.0.so.0").write_bytes(b"GObject is not GTK")
        lines = []
        for name in runtime._QT_REQUIRED:
            library = library_dir / f"lib{name}.so.6"
            library.write_bytes(b"fixture")
            lines.append(f"lib{name}.so.6 => {library} (0x00001234)")
        output = subprocess.CompletedProcess([], 0, "\n".join(lines), "")
        with mock.patch.object(runtime, "_require_tools"), mock.patch.object(runtime, "_run", return_value=output):
            resolved = runtime._check_linux(stage, stage / "bin" / "puls", False, {})
            self.assertEqual(set(resolved), set(runtime._QT_REQUIRED))
            (library_dir / "libgtk-3.so.0").write_bytes(b"GTK")
            with self.assertRaises(runtime.ValidationError):
                runtime._check_linux(stage, stage / "bin" / "puls", False, {})

    def test_retina_preview_preserves_logical_size_and_rejects_wrong_aspect_ratio(self):
        sizes = ((360, 294), (360, 640), (900, 600), (1280, 800))
        invalid = False

        def render(command, env, timeout):
            self.assertEqual(env["QT_QPA_PLATFORM"], "offscreen")
            directory = Path(env["PULS_GUI_PREVIEW_DIR"])
            for index in range(42):
                width, height = sizes[index % len(sizes)]
                pixels = (width * 2, height * 2 + (10 if invalid else 0))
                header = b"\x89PNG\r\n\x1a\n" + b"\0\0\0\rIHDR" + struct.pack(">II", *pixels)
                (directory / f"light-{index}-{width}x{height}.png").write_bytes(header)
            return subprocess.CompletedProcess(command, 0, "", "")

        with mock.patch.object(runtime, "_run", side_effect=render):
            self.assertEqual(runtime._check_preview(self.root / "puls", {}, self.root / "retina"), 42)
            invalid = True
            with self.assertRaisesRegex(runtime.ValidationError, "pixel size"):
                runtime._check_preview(self.root / "puls", {}, self.root / "wrong-aspect")


if __name__ == "__main__":
    unittest.main()
