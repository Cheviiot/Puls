import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


SPEC = importlib.util.spec_from_file_location("android_smoke", Path(__file__).with_name("smoke.py"))
SMOKE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SMOKE)


class RuntimeProbeTests(unittest.TestCase):
    def record(self, **overrides):
        capabilities = {"ssl_available": True, "ssl_backend": "openssl", "qt_version": "6.12.0"}
        capabilities.update(overrides)
        return SMOKE.RUNTIME_MARKER + json.dumps(capabilities)

    def test_extracts_only_capabilities(self):
        log = "unrelated app message\nI/Qt: " + self.record(unrelated="discarded") + "\n"
        self.assertEqual(SMOKE.parse_runtime_probe(log), {
            "ssl_available": True, "ssl_backend": "openssl", "qt_version": "6.12.0"})

    def test_missing_marker_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "missing"):
            SMOKE.parse_runtime_probe("unrelated app message")

    def test_malformed_probe_is_rejected(self):
        for payload in ("{", "[]", "null"):
            with self.subTest(payload=payload), self.assertRaisesRegex(RuntimeError, "malformed"):
                SMOKE.parse_runtime_probe(SMOKE.RUNTIME_MARKER + payload)

    def test_unavailable_tls_and_wrong_backend_are_rejected(self):
        for overrides in ({"ssl_available": False}, {"ssl_available": 1}, {"ssl_backend": "cert-only"}):
            with self.subTest(overrides=overrides), self.assertRaisesRegex(RuntimeError, "OpenSSL"):
                SMOKE.parse_runtime_probe(self.record(**overrides))

    def test_duplicate_marker_and_invalid_version_are_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "ambiguous"):
            SMOKE.parse_runtime_probe(self.record() + "\n" + self.record())
        with self.assertRaisesRegex(RuntimeError, "Qt version"):
            SMOKE.parse_runtime_probe(self.record(qt_version=None))

    def test_smoke_requires_private_intent_and_reads_only_process_tag(self):
        calls = []

        def command(arguments, **options):
            self.assertEqual(arguments[:3], ["adb", "-s", "emulator-5554"])
            calls.append(arguments[3:])
            if arguments[3:5] == ["exec-out", "screencap"]:
                return b"\x89PNG\r\n\x1a\n" + bytes(1024)
            if "pidof" in arguments:
                return "4321\n"
            if "dumpsys" in arguments:
                return "mResumedActivity: " + SMOKE.APP
            if "start" in arguments:
                return "Status: ok\n"
            if "logcat" in arguments:
                return "" if "crash" in arguments else self.record(unrelated="discarded")
            return ""

        scratch = Path(__file__).resolve().parents[3] / ".scratch"
        scratch.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="android-smoke-", dir=scratch) as directory:
            output = Path(directory)
            with patch.object(SMOKE.subprocess, "check_output", side_effect=command), \
                    patch.object(SMOKE.time, "sleep"), patch("builtins.print"):
                SMOKE.smoke("emulator-5554", output / "puls.apk", output)
            report = json.loads((output / "smoke.json").read_text())
            self.assertEqual(report["ssl_backend"], "openssl")
            self.assertNotIn("unrelated", report)
            self.assertEqual({path.name for path in output.iterdir()}, {"smoke.json", "dashboard.png"})
        self.assertIn(["shell", "am", "start", "-W", "-n", SMOKE.ACTIVITY,
                       "--ez", "puls.runtime_probe", "true"], calls)
        log_reads = [call for call in calls if call[0] == "logcat"]
        self.assertTrue(log_reads)
        for arguments in log_reads:
            self.assertEqual(arguments[arguments.index("--pid") + 1], "4321")
        self.assertIn(["logcat", "-b", "main", "--pid", "4321", "-d", "-v", "raw",
                       "PulsRuntimeProbe:I", "*:S"], log_reads)


if __name__ == "__main__":
    unittest.main()
