"""Machine-consumed updater progress must be readable under every code page.

The Qt caller sets PYTHONUTF8 / PYTHONIOENCODING for its child process, but the
same scripts are also documented for standalone runs, and those inherit the
console code page. A progress line carrying Chinese text then either emitted
cp936/cp1252 bytes (which a UTF-8 consumer cannot decode, so the event was
dropped) or raised UnicodeEncodeError and lost the finished event entirely.

The contract tested here is deliberately one encoding-independent form: every
machine-consumed line is ASCII-only JSON, and decoding it yields exactly the
original text. Human-readable file output is not affected by this test.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[2] / "tools"

PROBE = (
    "import json, sys\n"
    "sys.path.insert(0, sys.argv[1])\n"
    "target = sys.argv[2]\n"
    "texts = json.loads(open(sys.argv[3], encoding='utf-8').read())\n"
    "if target == 'data':\n"
    "    import public_data_updater as module\n"
    "    for text in texts:\n"
    "        module.emit('progress', message=text)\n"
    "        module.emit('component', component='pets', status='updated', error=text)\n"
    "elif target == 'activity':\n"
    "    import public_activity_exchange_updater as module\n"
    "    for text in texts:\n"
    "        module.emit(text)\n"
    "else:\n"
    "    import public_routine_updater as module\n"
    "    for text in texts:\n"
    "        module._emit(text)\n"
)

PAYLOADS = (
    "检查官方公共数据：精灵养成、兑换目录与日常活动",
    "更新完成 🚀✨",
    "Café ¥ 中文 🎉 done",
    "plain ascii progress",
)
ENCODINGS = ("ascii", "cp1252", "cp936", "utf-8", None)
# Every emitted line of one target and where its text must reappear.
TARGET_FIELDS = {"data": ("message", "error"), "activity": ("message",), "routine": ("message",)}


def run_probe(target: str, encoding: str | None, redirect: str, payload_file: Path):
    environment = {key: value for key, value in os.environ.items()
                   if key not in {"PYTHONUTF8", "PYTHONIOENCODING"}}
    if encoding:
        environment["PYTHONIOENCODING"] = encoding
    command = [sys.executable, "-B", "-c", PROBE, str(TOOLS), target, str(payload_file)]
    if redirect == "pipe":
        completed = subprocess.run(command, capture_output=True, env=environment, timeout=180)
        return completed.returncode, completed.stdout, completed.stderr
    with tempfile.TemporaryFile() as stream:
        completed = subprocess.run(command, stdout=stream, stderr=subprocess.PIPE, env=environment, timeout=180)
        stream.seek(0)
        return completed.returncode, stream.read(), completed.stderr


class UpdaterEncodingTest(unittest.TestCase):
    def test_progress_lines_are_ascii_json_under_every_stream(self):
        with tempfile.TemporaryDirectory(prefix="kq-updater-encoding-") as directory:
            payload_file = Path(directory) / "payloads.json"
            payload_file.write_text(json.dumps(PAYLOADS), encoding="utf-8")
            for target, fields in TARGET_FIELDS.items():
                for encoding in ENCODINGS:
                    for redirect in ("pipe", "file"):
                        with self.subTest(target=target, encoding=encoding, redirect=redirect):
                            code, raw, stderr = run_probe(target, encoding, redirect, payload_file)
                            self.assertEqual(code, 0, stderr.decode("utf-8", "replace"))
                            self.assertNotIn(b"UnicodeEncodeError", stderr)
                            self.assertTrue(raw.isascii(), raw)
                            lines = [line for line in raw.split(b"\n") if line.strip()]
                            self.assertTrue(lines, "no progress line was written")
                            for line in lines:
                                value = json.loads(line.decode("ascii"))
                                self.assertIn("event", value)
                                # Exactly one payload field carries the text.
                                found = [value[field] for field in fields if field in value]
                                self.assertEqual(len(found), 1, value)
                                self.assertIn(found[0], PAYLOADS)

    def test_every_payload_is_decoded_back_unchanged(self):
        with tempfile.TemporaryDirectory(prefix="kq-updater-encoding-") as directory:
            payload_file = Path(directory) / "payloads.json"
            payload_file.write_text(json.dumps(PAYLOADS), encoding="utf-8")
            for target, fields in TARGET_FIELDS.items():
                expected = [text for text in PAYLOADS for _ in fields]
                for encoding in ENCODINGS:
                    with self.subTest(target=target, encoding=encoding):
                        code, raw, stderr = run_probe(target, encoding, "pipe", payload_file)
                        self.assertEqual(code, 0, stderr.decode("utf-8", "replace"))
                        decoded = []
                        for line in raw.split(b"\n"):
                            if not line.strip():
                                continue
                            value = json.loads(line.decode("ascii"))
                            decoded.extend(value[field] for field in fields if field in value)
                        self.assertEqual(decoded, expected)


if __name__ == "__main__":
    unittest.main()
