"""Integration tests of install.sh and install.ps1 against a local release.

The releases are packaged with release.py, so the tests also check that the
installers accept what the release tool produces.
"""

import hashlib
import http.server
import json
import os
import platform
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import release  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent.parent
INSTALL_SH = ROOT / "scripts" / "install.sh"
INSTALL_PS1 = ROOT / "scripts" / "install.ps1"
VERSION = "1.2.3"
MANAGED_MARKER = "<key>PulsInstallerManaged</key><true/>"


def host_target():
    system = {"Linux": "linux", "Darwin": "darwin", "Windows": "windows"}.get(platform.system())
    architecture = {"x86_64": "amd64", "amd64": "amd64", "arm64": "arm64",
                    "aarch64": "arm64"}.get(platform.machine().lower())
    return system, architecture


SYSTEM, ARCHITECTURE = host_target()
UNIX = SYSTEM in ("linux", "darwin") and ARCHITECTURE is not None
WINDOWS = SYSTEM == "windows" and ARCHITECTURE is not None


def write(path, content, mode=0o644):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content if isinstance(content, bytes) else content.encode())
    os.chmod(path, mode)


def plist(managed=True):
    marker = f"\n{MANAGED_MARKER}" if managed else ""
    return ("<plist><dict><key>CFBundleIdentifier</key>"
            f"<string>io.github.cheviiot.puls</string>{marker}\n</dict></plist>\n")


def build_release(directory, gui):
    """Packages fake programs for the host and returns the release files."""
    stage = directory / "stage"
    for document in release.DOCUMENTS:
        write(stage / document, f"{document}\n")
    if SYSTEM == "windows":
        write(stage / "puls.exe", b"fake puls")
        if gui:
            write(stage / "puls-gui.exe", b"fake puls-gui")
    else:
        write(stage / "puls", "#!/bin/sh\nprintf 'installed\\n'\n", 0o755)
        if gui and SYSTEM == "linux":
            write(stage / "puls-gui", "#!/bin/sh\nexit 0\n", 0o755)
            write(stage / "assets" / "Icon.png", b"icon")
        elif gui:
            write(stage / "Puls.app/Contents/MacOS/Puls", "#!/bin/sh\nexit 0\n", 0o755)
            write(stage / "Puls.app/Contents/Info.plist", plist())
            write(stage / "Puls.app/Contents/Resources/Puls.icns", b"icns")
    dist = directory / "dist"
    release.package(VERSION, SYSTEM, ARCHITECTURE, stage, dist)
    release.assemble(VERSION, dist, {(SYSTEM, ARCHITECTURE): None}, root=ROOT)
    return {path.name: path.read_bytes() for path in dist.iterdir()}


def manifest_naming(name, digest="0" * 64, schema=3):
    payload = {
        "schema_version": schema,
        "product": "Puls",
        "version": VERSION,
        "assets": [{
            "os": SYSTEM, "arch": ARCHITECTURE, "file": name, "sha256": digest,
            "kind": "archive", "capabilities": ["cli"],
        }],
    }
    return (json.dumps(payload, indent=2) + "\n").encode()


class ReleaseServer:
    """Serves release files by their base name, like GitHub release URLs."""

    def __init__(self, files):
        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                name = self.path.rsplit("/", 1)[-1]
                if name not in files:
                    self.send_error(404)
                    return
                content_type = "application/octet-stream"
                if name.endswith(".json"):
                    content_type = "application/json"
                elif name == "install.ps1":
                    content_type = "text/plain; charset=utf-8"
                self.send_response(200)
                self.send_header("Content-Type", content_type)
                self.send_header("Content-Length", str(len(files[name])))
                self.end_headers()
                self.wfile.write(files[name])

            def log_message(self, *arguments):
                pass

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.url = f"http://127.0.0.1:{self.server.server_address[1]}"

    def close(self):
        self.server.shutdown()
        self.server.server_close()


class InstallerTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.directory = Path(self.temporary.name)
        self.servers = []

    def tearDown(self):
        for server in self.servers:
            server.close()
        self.temporary.cleanup()

    def serve(self, files):
        server = ReleaseServer(files)
        self.servers.append(server)
        return server

    def environment(self, **values):
        environment = {key: value for key, value in os.environ.items()
                       if not key.startswith("PULS_")}
        environment["NO_PROXY"] = environment["no_proxy"] = "127.0.0.1,localhost"
        environment.update({key: str(value) for key, value in values.items()})
        return environment


@unittest.skipUnless(UNIX, "install.sh is intended for Linux and macOS")
class ShellInstallerTest(InstallerTest):
    def run_installer(self, arguments, **environment):
        return subprocess.run(["sh", str(INSTALL_SH), *arguments], capture_output=True,
                              text=True, env=self.environment(**environment))

    def test_installs_updates_and_uninstalls(self):
        server = self.serve(build_release(self.directory, gui=False))
        home = self.directory / "home"
        home.mkdir()
        install_dir = home / ".local" / "bin"
        profile = home / (".bash_profile" if SYSTEM == "darwin" else ".bashrc")
        initial = "# user configuration"
        write(profile, initial, 0o640)
        environment = {"HOME": home, "SHELL": "/bin/bash",
                       "PULS_INSTALL_REPOSITORY_URL": server.url}

        result = self.run_installer([], **environment)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        installed = install_dir / "puls"
        self.assertEqual(installed.stat().st_mode & 0o777, 0o755)
        self.assertEqual(subprocess.run([str(installed)], capture_output=True,
                                        text=True).stdout, "installed\n")
        self.assertFalse((install_dir / "puls-gui").exists())
        line = f"export PATH='{install_dir}':\"$PATH\" # Puls installer"
        self.assertEqual(profile.read_text(), f"{initial}\n{line}\n")

        result = self.run_installer([], **environment)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("обновлён", result.stdout)
        self.assertEqual(profile.read_text().count("# Puls installer"), 1)

        # A directory at the program path stops the uninstallation before
        # anything is changed.
        saved = install_dir / "puls.saved"
        installed.rename(saved)
        installed.mkdir()
        result = self.run_installer(["--uninstall"], **environment)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("# Puls installer", profile.read_text())
        installed.rmdir()
        saved.rename(installed)
        server.close()

        result = self.run_installer(["--uninstall"], **environment)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(installed.exists())
        self.assertEqual(profile.read_text(), initial + "\n")
        self.assertEqual(profile.stat().st_mode & 0o777, 0o640)
        result = self.run_installer(["--uninstall"], **environment)
        self.assertEqual(result.returncode, 0, result.stderr)
        installed.mkdir()
        result = self.run_installer(["--uninstall"], **environment)
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(installed.is_dir())

    def test_rejects_checksum_mismatch(self):
        name = release.archive_name(VERSION, SYSTEM, ARCHITECTURE)
        manifest = manifest_naming(name)
        server = self.serve({
            name: b"not an archive",
            release.MANIFEST_NAME: manifest,
            release.CHECKSUMS_NAME: (f"{'0' * 64}  {name}\n"
                                     f"{hashlib.sha256(manifest).hexdigest()}  "
                                     f"{release.MANIFEST_NAME}\n").encode(),
        })
        install_dir = self.directory / "bin"
        result = self.run_installer(["--version", VERSION, "--install-dir", str(install_dir)],
                                    PULS_INSTALL_REPOSITORY_URL=server.url)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("контрольная сумма архива не совпала", result.stderr)
        self.assertFalse((install_dir / "puls").exists())

    def test_rejects_unexpected_or_legacy_packages(self):
        name = release.archive_name(VERSION, SYSTEM, ARCHITECTURE)
        wrong = name.replace(ARCHITECTURE, ARCHITECTURE + "-wrong")
        install_dir = self.directory / "bin"
        cases = (
            (manifest_naming(wrong), "указывает неожиданный пакет"),
            (manifest_naming(name, schema=2), "собран в прежнем формате"),
        )
        for manifest, message in cases:
            server = self.serve({release.MANIFEST_NAME: manifest})
            result = self.run_installer(["--version", VERSION, "--install-dir", str(install_dir)],
                                        PULS_INSTALL_REPOSITORY_URL=server.url)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(message, result.stderr)
            self.assertFalse((install_dir / "puls").exists())

    def test_installs_and_removes_the_graphical_application(self):
        server = self.serve(build_release(self.directory, gui=True))
        home = self.directory / "home"
        data_home = home / ".local" / "share"
        install_dir = home / ".local" / "bin"
        home.mkdir()
        environment = {"HOME": home, "XDG_DATA_HOME": data_home,
                       "PULS_INSTALL_DIR": install_dir,
                       "PULS_INSTALL_REPOSITORY_URL": server.url}
        arguments = ["--version", VERSION, "--no-path-update"]
        if SYSTEM == "darwin":
            linked = self.directory / "linked-applications"
            linked.mkdir()
            applications = home / "Applications"
            applications.symlink_to(linked)
            result = self.run_installer(arguments, **environment)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("не является безопасным каталогом", result.stderr)
            self.assertFalse((linked / "Puls.app").exists())
            self.assertFalse((install_dir / "puls").exists())
            applications.unlink()

            unmanaged = home / "Applications" / "Puls.app" / "Contents" / "Info.plist"
            write(unmanaged, plist(managed=False))
            result = self.run_installer(arguments, **environment)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("не принадлежит установщику Puls", result.stderr)
            self.assertEqual(unmanaged.read_text(), plist(managed=False))
            self.assertFalse((install_dir / "puls").exists())
            shutil.rmtree(home / "Applications" / "Puls.app")

        result = self.run_installer(arguments, **environment)
        self.assertEqual(result.returncode, 0, result.stderr)
        managed = [install_dir / "puls"]
        if SYSTEM == "linux":
            gui = install_dir / "puls-gui"
            self.assertEqual(gui.stat().st_mode & 0o777, 0o755)
            desktop = data_home / "applications" / "io.github.cheviiot.puls.desktop"
            entry = desktop.read_text()
            self.assertIn(f'Exec="{gui}"\n', entry)
            self.assertIn("X-Puls-Managed=true", entry)
            icon = data_home / "icons/hicolor/512x512/apps/io.github.cheviiot.puls.png"
            self.assertEqual(icon.read_bytes(), b"icon")
            managed += [gui, desktop, icon]
        else:
            bundle = home / "Applications" / "Puls.app"
            self.assertEqual((bundle / "Contents/MacOS/Puls").read_text(), "#!/bin/sh\nexit 0\n")
            self.assertEqual((bundle / "Contents/MacOS/Puls").stat().st_mode & 0o777, 0o755)
            self.assertIn(MANAGED_MARKER, (bundle / "Contents/Info.plist").read_text())
            self.assertEqual(sorted(path.name for path in bundle.parent.iterdir()), ["Puls.app"])
            managed.append(bundle)

        # Reinstalling replaces the managed application.
        result = self.run_installer(arguments, **environment)
        self.assertEqual(result.returncode, 0, result.stderr)
        result = self.run_installer(["--uninstall", "--no-path-update"], **environment)
        self.assertEqual(result.returncode, 0, result.stderr)
        for path in managed:
            self.assertFalse(path.exists(), path)

    @unittest.skipUnless(SYSTEM == "darwin", "macOS bundle ownership")
    def test_preserves_unmanaged_macos_application(self):
        home = self.directory / "home"
        bundle = home / "Applications" / "Puls.app"
        write(bundle / "Contents" / "Info.plist", plist(managed=False))
        environment = {"HOME": home, "PULS_INSTALL_DIR": home / ".local" / "bin"}
        result = self.run_installer(["--uninstall", "--no-path-update"], **environment)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((bundle / "Contents" / "Info.plist").read_text(), plist(managed=False))

        shutil.rmtree(bundle)
        external = self.directory / "external-contents"
        (external / "MacOS").mkdir(parents=True)
        (external / "Resources").mkdir()
        write(external / "Info.plist", plist())
        bundle.mkdir(parents=True)
        (bundle / "Contents").symlink_to(external)
        result = self.run_installer(["--uninstall", "--no-path-update"], **environment)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((bundle / "Contents").is_symlink())
        self.assertEqual((external / "Info.plist").read_text(), plist())


class PowerShellScriptTest(unittest.TestCase):
    def test_installer_is_ascii_without_bom(self):
        content = INSTALL_PS1.read_bytes()
        self.assertFalse(content.startswith(b"\xef\xbb\xbf"),
                         "a BOM breaks irm | iex in Windows PowerShell 5")
        for offset, value in enumerate(content):
            self.assertLess(value, 0x80, f"byte {offset} is not ASCII")


@unittest.skipUnless(WINDOWS, "install.ps1 is intended for Windows")
class PowerShellInstallerTest(InstallerTest):
    def setUp(self):
        super().setUp()
        self.powershell = shutil.which("powershell.exe") or shutil.which("pwsh.exe")
        if self.powershell is None:
            self.skipTest("PowerShell is unavailable")

    def powershell_command(self, command, **environment):
        # Windows PowerShell writes to pipes in the OEM code page by default.
        return subprocess.run(
            [self.powershell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command",
             "[Console]::OutputEncoding = [Text.Encoding]::UTF8; " + command],
            capture_output=True, text=True, encoding="utf-8", errors="replace",
            env=self.environment(**environment))

    def run_script(self, arguments, **environment):
        def quote(value):
            return "'" + str(value).replace("'", "''") + "'"

        # Parameter names stay unquoted; quoted text is a positional value.
        command = " ".join(
            argument if str(argument).startswith("-") else quote(argument)
            for argument in arguments)
        return self.powershell_command(f"& {quote(INSTALL_PS1)} {command}", **environment)

    def test_installs_updates_and_uninstalls(self):
        result = self.run_script(["-Help"])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Установка и удаление Puls", result.stdout)

        server = self.serve(build_release(self.directory, gui=True))
        install_dir = self.directory / "bin"
        shortcut_dir = self.directory / "shortcuts"
        environment = {"PULS_INSTALL_DIR": install_dir, "PULS_INSTALL_REPOSITORY_URL": server.url,
                       "PULS_SHORTCUT_DIR": shortcut_dir}
        install = f"Invoke-RestMethod -UseBasicParsing -Uri '{server.url}/install.ps1' | Invoke-Expression"
        result = self.powershell_command(install, **environment)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        self.assertEqual((install_dir / "puls.exe").read_bytes(), b"fake puls")
        self.assertEqual((install_dir / "puls-gui.exe").read_bytes(), b"fake puls-gui")
        shortcut = shortcut_dir / "Puls.lnk"
        self.assertTrue(shortcut.is_file())
        target = self.powershell_command(
            "(New-Object -ComObject WScript.Shell).CreateShortcut("
            f"'{shortcut}').TargetPath")
        # The shortcut keeps the long form of a temporary path that may be
        # given in the 8.3 form.
        self.assertTrue(os.path.samefile(target.stdout.strip(), install_dir / "puls-gui.exe"))

        result = self.powershell_command(install, **environment)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("обновлён", result.stdout)
        server.close()

        result = self.run_script(["-Uninstall", "-InstallDir", install_dir],
                                 PULS_SHORTCUT_DIR=shortcut_dir)
        self.assertEqual(result.returncode, 0, result.stderr)
        for path in (install_dir / "puls.exe", install_dir / "puls-gui.exe", shortcut):
            self.assertFalse(path.exists(), path)

        (install_dir / "puls.exe").mkdir(parents=True)
        result = self.run_script(["-Uninstall", "-InstallDir", install_dir, "-NoPathUpdate"])
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue((install_dir / "puls.exe").is_dir())

    def test_rejects_unexpected_or_legacy_packages(self):
        name = release.archive_name(VERSION, SYSTEM, ARCHITECTURE)
        wrong = name.replace(ARCHITECTURE, ARCHITECTURE + "-wrong")
        install_dir = self.directory / "bin"
        cases = ((manifest_naming(wrong), wrong), (manifest_naming(name, schema=2), "1.2.3"))
        for manifest, message in cases:
            server = self.serve({release.MANIFEST_NAME: manifest})
            result = self.run_script(["-Version", VERSION, "-InstallDir", install_dir,
                                      "-NoPathUpdate", "-RepositoryUrl", server.url])
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(message, result.stdout + result.stderr)
            self.assertFalse((install_dir / "puls.exe").exists())


if __name__ == "__main__":
    unittest.main()
