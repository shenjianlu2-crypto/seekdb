import json
import math
import re
import tempfile
import unittest
from pathlib import Path

import minivectordb


ROOT = Path(__file__).resolve().parents[1]
PUBLIC = Path(__file__).parent / "official_public"


def _cell_matches(actual, expected):
    if isinstance(expected, bool):
        return actual == str(expected)
    if isinstance(expected, int):
        try:
            return int(actual) == expected
        except ValueError:
            return False
    if isinstance(expected, float):
        try:
            return math.isclose(float(actual), expected, rel_tol=1e-5, abs_tol=1e-5)
        except ValueError:
            return False
    return actual == str(expected)


def _rows_match(actual, expected):
    return len(actual) == len(expected) and all(
        len(a) == len(e) and all(_cell_matches(x, y) for x, y in zip(a, e))
        for a, e in zip(actual, expected)
    )


class OfficialPublicSamples(unittest.TestCase):
    def test_all_published_cases_against_engine(self):
        manifest = json.loads((PUBLIC / "manifest.json").read_text(encoding="utf-8"))
        for case in manifest["cases"]:
            with self.subTest(case=case["id"]), tempfile.TemporaryDirectory() as data_dir:
                storage = minivectordb.StorageEngine(data_dir)
                executor = minivectordb.Executor(storage)
                expected = json.loads(
                    (PUBLIC / Path(case["result"]).name).read_text(encoding="utf-8")
                )
                assertions = 0
                test = (PUBLIC / Path(case["test"]).name).read_text(encoding="utf-8")
                try:
                    for line in (line.strip() for line in test.splitlines()):
                        if not line:
                            continue
                        if line == "--restart":
                            storage.shutdown()
                            storage = minivectordb.StorageEngine(data_dir)
                            executor = minivectordb.Executor(storage)
                            continue
                        if line.startswith("--mode "):
                            # The small public ANN case specifies the exact expected rows.
                            continue

                        self.assertLess(assertions, len(expected), "missing expected result")
                        result = expected[assertions]
                        assertions += 1

                        if "error" in result:
                            with self.assertRaises((minivectordb.ParseError, ValueError)) as caught:
                                executor.execute(minivectordb.parse_statement(line))
                            self.assertTrue(str(caught.exception))
                            continue

                        response = executor.execute(minivectordb.parse_statement(line))
                        if "columns" in result:
                            output_lines = response.splitlines()
                            header = output_lines[0].split("\t")
                            self.assertEqual(header, result["columns"])
                            actual_rows = [line.split("\t") for line in output_lines[1:]]
                            self.assertEqual(len(actual_rows), len(result["rows"]))
                            expected_rows = result["rows"]
                            if result.get("unordered"):
                                remaining = list(expected_rows)
                                for row in actual_rows:
                                    match = next(
                                        (i for i, other in enumerate(remaining)
                                         if _rows_match([row], [other])),
                                        None,
                                    )
                                    self.assertIsNotNone(match, f"unexpected row {row!r}")
                                    remaining.pop(match)
                                self.assertEqual(remaining, [])
                            else:
                                self.assertEqual(len(actual_rows), len(expected_rows))
                                for row, other in zip(actual_rows, expected_rows):
                                    self.assertTrue(_rows_match([row], [other]), f"{row!r} != {other!r}")
                        else:
                            if "affected_rows" in result:
                                match = re.search(r"\b(\d+) rows? affected\b", response)
                                self.assertIsNotNone(match, response)
                                self.assertEqual(int(match.group(1)), result["affected_rows"])

                    self.assertEqual(assertions, len(expected), "unused expected results")
                finally:
                    storage.shutdown()


if __name__ == "__main__":
    unittest.main()
