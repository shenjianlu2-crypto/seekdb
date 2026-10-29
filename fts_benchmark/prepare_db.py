#!/usr/bin/env python3
"""Prepare the benchmark database (schema + corpus) without running the benchmark."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.stdout.reconfigure(encoding="utf-8")

import bench  # noqa: E402


class Options:
    host = "127.0.0.1"
    port = 2881
    user = "root@sys,admin@sys"
    password = ""
    data = "data/docs.tsv"
    out = "reports/unused.json"
    label = "prep"
    skip_load = False


def main():
    instance = bench.Bench(Options())
    instance.prepare_schema()
    instance.load_data()
    instance.prepare_tokenize_tables()
    print("database prepared")


if __name__ == "__main__":
    main()
