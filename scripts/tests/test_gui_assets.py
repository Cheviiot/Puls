"""Tests of the generator of the interface icons and fonts."""

import io
import json
import re
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import gui_assets  # noqa: E402


class ElementPathTest(unittest.TestCase):
    def test_relative_moveto_starts_absolute_lines(self):
        path = gui_assets.element_path
        self.assertEqual(path("path", {"d": "m12 19-7-7 7-7"}), "M12 19l-7-7 7-7")
        self.assertEqual(path("path", {"d": "m4.9 4.9 1.4 1.4"}), "M4.9 4.9l1.4 1.4")
        # A command after the moveto stays as it is.
        self.assertEqual(path("path", {"d": "m9 18h6"}), "M9 18h6")
        self.assertEqual(path("path", {"d": "M3 12h18"}), "M3 12h18")

    def test_shapes_become_paths(self):
        self.assertEqual(
            gui_assets.element_path("line", {"x1": "12", "y1": "2", "x2": "12", "y2": "4.5"}),
            "M12 2L12 4.5",
        )
        self.assertEqual(
            gui_assets.element_path("polyline", {"points": "20 6 9 17 4 12"}), "M20 6L9 17L4 12"
        )
        self.assertEqual(
            gui_assets.element_path("polygon", {"points": "6,3 20,12 6,21"}), "M6 3L20 12L6 21Z"
        )
        self.assertEqual(
            gui_assets.element_path("circle", {"cx": "12", "cy": "12", "r": "10", "key": "k"}),
            "M2 12A10 10 0 1 0 22 12A10 10 0 1 0 2 12Z",
        )
        self.assertEqual(
            gui_assets.element_path("rect", {"x": "2", "y": "2", "width": "20", "height": "8"}),
            "M2 2H22V10H2Z",
        )
        self.assertEqual(
            gui_assets.element_path(
                "rect", {"x": "2", "y": "2", "width": "20", "height": "8", "rx": "2"}
            ),
            "M4 2H20A2 2 0 0 1 22 4V8A2 2 0 0 1 20 10H4A2 2 0 0 1 2 8V4A2 2 0 0 1 4 2Z",
        )
        with self.assertRaises(ValueError):
            gui_assets.element_path("text", {})


class IconsTest(unittest.TestCase):
    def nodes(self):
        return {icon: [["path", {"d": "M0 0h1"}]] for icon in gui_assets.ICONS.values()}

    def test_qml_declares_every_icon(self):
        qml = gui_assets.icons_qml(self.nodes(), "9.9.9")
        self.assertTrue(qml.startswith("pragma Singleton\nimport QtQml\n"))
        self.assertIn("Lucide 9.9.9", qml)
        for name in gui_assets.ICONS:
            self.assertIn(f'    readonly property string {name}: "M0 0h1"\n', qml)

    def test_committed_icons_match_the_list(self):
        qml = (gui_assets.QML_DIR / "Icons.qml").read_text()
        names = re.findall(r"readonly property string (\w+):", qml)
        self.assertEqual(names, list(gui_assets.ICONS))
        self.assertIn(f"Lucide {gui_assets.LUCIDE_VERSION}", qml)

    def test_used_icons_exist(self):
        used = set()
        for path in gui_assets.QML_DIR.glob("*.qml"):
            used.update(re.findall(r"\bIcons\.(\w+)", path.read_text()))
        self.assertTrue(used)
        self.assertLessEqual(used, set(gui_assets.ICONS))

    def test_generation_checks_the_archive(self):
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory) / "lucide.tgz"
            buffer = io.BytesIO()
            with tarfile.open(fileobj=buffer, mode="w:gz") as package:
                for name, content in (
                    ("package/icon-nodes.json", json.dumps(self.nodes()).encode()),
                    ("package/LICENSE", b"ISC License\n"),
                ):
                    info = tarfile.TarInfo(name)
                    info.size = len(content)
                    package.addfile(info, io.BytesIO(content))
            archive.write_bytes(buffer.getvalue())
            with self.assertRaisesRegex(SystemExit, "SHA-256"):
                gui_assets.generate_icons(archive)

            output = Path(directory) / "qml"
            output.mkdir()
            digest = gui_assets.hashlib.sha256(archive.read_bytes()).hexdigest()
            with mock.patch.object(gui_assets, "LUCIDE_SHA256", digest), mock.patch.object(
                gui_assets, "QML_DIR", output
            ):
                gui_assets.generate_icons(archive)
            self.assertIn("readonly property string settings", (output / "Icons.qml").read_text())
            self.assertEqual((output / "LICENSE.lucide.txt").read_text(), "ISC License\n")


class FontsTest(unittest.TestCase):
    def test_committed_fonts_and_license_exist(self):
        fonts = gui_assets.QML_DIR / "fonts"
        for name in gui_assets.FONTS:
            self.assertGreater((fonts / name).stat().st_size, 10_000, name)
        self.assertIn("SIL OPEN FONT LICENSE", (fonts / "LICENSE.txt").read_text())

    def test_fonts_are_resources_of_the_module(self):
        cmake = (gui_assets.QML_DIR / "CMakeLists.txt").read_text()
        for name in gui_assets.FONTS:
            self.assertIn(f"fonts/{name}", cmake)


if __name__ == "__main__":
    unittest.main()
