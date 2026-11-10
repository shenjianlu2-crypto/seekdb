import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
PROGRAM = ROOT / "minivectordb.py"


class StdioProtocolTests(unittest.TestCase):
    def test_jsonl_handshake_execute_and_shutdown(self):
        with tempfile.TemporaryDirectory() as data_dir:
            env = os.environ.copy()
            env["MINIVECTORDB_DATA_DIR"] = data_dir
            process = subprocess.Popen(
                [sys.executable, str(PROGRAM)],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding="utf-8",
                env=env,
            )
            try:
                ready = json.loads(process.stdout.readline())
                self.assertEqual({"type": "ready"}, ready)

                requests = [
                    {
                        "id": 7,
                        "op": "execute",
                        "sql": "CREATE TABLE docs (id BIGINT PRIMARY KEY, content VARCHAR(16));",
                        "search_mode": "exact",
                    },
                    {
                        "id": "row-write",
                        "op": "execute",
                        "sql": "INSERT INTO docs VALUES (1, '你好');",
                        "search_mode": "exact",
                    },
                    {
                        "id": 9,
                        "op": "execute",
                        "sql": "SELECT id, content FROM docs ORDER BY id;",
                        "search_mode": "exact",
                    },
                    {"id": 10, "op": "shutdown"},
                ]
                responses = []
                for request in requests:
                    process.stdin.write(json.dumps(request, ensure_ascii=False) + "\n")
                    process.stdin.flush()
                    responses.append(json.loads(process.stdout.readline()))

                self.assertEqual({"id": 7, "ok": True}, responses[0])
                self.assertEqual(
                    {"id": "row-write", "ok": True, "affected_rows": 1},
                    responses[1],
                )
                self.assertEqual(
                    {
                        "id": 9,
                        "ok": True,
                        "columns": ["id", "content"],
                        "rows": [[1, "你好"]],
                    },
                    responses[2],
                )
                self.assertEqual({"id": 10, "ok": True}, responses[3])
                process.stdin.close()
                self.assertEqual(0, process.wait(timeout=5))
                self.assertEqual("", process.stdout.read())
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=5)
                for stream in (process.stdin, process.stdout, process.stderr):
                    if not stream.closed:
                        stream.close()


if __name__ == "__main__":
    unittest.main()
