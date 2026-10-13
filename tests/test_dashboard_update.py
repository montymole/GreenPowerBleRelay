import gzip
import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from email.parser import BytesParser
from email.policy import default

ROOT = Path(__file__).resolve().parent.parent


def module(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "scripts" / (name + ".py"))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


ota = module("update_over_wifi")
assets = module("build_dashboard")


class DashboardUpdateTest(unittest.TestCase):
    def test_compressed_assets_preserve_sources_and_are_reproducible(self):
        assets.build()
        first = {}
        for name in assets.ASSETS:
            packed = (ROOT / "data/dashboard" / (name + ".gz")).read_bytes()
            self.assertEqual(gzip.decompress(packed), (ROOT / "dashboard" / name).read_bytes())
            first[name] = packed
        assets.build()
        for name, packed in first.items():
            self.assertEqual(packed, (ROOT / "data/dashboard" / (name + ".gz")).read_bytes())

    def test_upload_binary_multipart_and_checksum(self):
        payload = bytes(range(256)) * 23
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / "firmware.bin"
            image.write_bytes(payload)
            with patch.object(ota, "request", return_value={"ok": True}) as request:
                ota.upload("http://relay", "test-key", "firmware", image)
            base, key, path, body, headers = request.call_args.args
            self.assertEqual((base, key, path), ("http://relay", "test-key", "/api/v1/update/firmware"))
            self.assertEqual(headers["X-Update-Size"], str(len(payload)))
            self.assertEqual(headers["X-Update-MD5"], hashlib.md5(payload).hexdigest())
            message = BytesParser(policy=default).parsebytes(
                f"Content-Type: {headers['Content-Type']}\r\n\r\n".encode() + body)
            parts = list(message.iter_parts())
            self.assertEqual(len(parts), 1)
            self.assertEqual(parts[0].get_payload(decode=True), payload)

    def test_wrong_chip_rejected_before_build_or_upload(self):
        with patch("sys.argv", ["update", "http://relay", "--target", "all"]), \
             patch.dict("os.environ", {"RELAY_API_KEY": "test-key"}), \
             patch.object(ota, "request", return_value={"chip": "ESP32-C6", "filesystem": "spiffs"}), \
             patch.object(ota.subprocess, "run") as build, patch.object(ota, "upload") as upload:
            with self.assertRaisesRegex(RuntimeError, "does not match"):
                ota.main()
            build.assert_not_called()
            upload.assert_not_called()


if __name__ == "__main__":
    unittest.main()
