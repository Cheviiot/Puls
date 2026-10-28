import importlib.util
import json
from pathlib import Path
import unittest


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


if __name__ == "__main__":
    unittest.main()
