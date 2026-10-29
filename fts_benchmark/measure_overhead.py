#!/usr/bin/env python3
"""Measure the fixed per-call overhead of TOKENIZE() vs. the per-character cost."""
import json
import sys
import time

import pymysql

sys.stdout.reconfigure(encoding="utf-8")

RUNS = 3000


def main():
    connection = pymysql.connect(host="127.0.0.1", port=2881, user="root@sys",
                                 password="", charset="utf8mb4", autocommit=True,
                                 connect_timeout=20)
    cursor = connection.cursor()
    cursor.execute("USE fts_large_bench")
    texts = json.load(open("data/tokenize_texts.json", encoding="utf-8"))

    def bench(sql, label):
        for _ in range(30):
            cursor.execute(sql)
            cursor.fetchall()
        started = time.perf_counter()
        cursor.execute(sql)
        cursor.fetchall()
        elapsed = time.perf_counter() - started
        print("%-34s %8.4f ms/call" % (label, elapsed * 1000.0 / RUNS))

    bench("SELECT COUNT(TOKENIZE('a', 'ik')) FROM tok_zh", "ik  1 char")
    bench("SELECT COUNT(TOKENIZE('a', 'beng')) FROM tok_en", "beng 1 char")
    bench("SELECT COUNT(TOKENIZE(' ', 'ik')) FROM tok_zh", "ik  1 space")
    bench("SELECT COUNT(TOKENIZE(t, 'ik')) FROM tok_zh", "ik  full text")
    bench("SELECT COUNT(TOKENIZE(t, 'beng')) FROM tok_en", "beng full text")
    bench("SELECT COUNT(TOKENIZE(REPEAT('数据库',20), 'ik')) FROM tok_zh",
          "ik  60 cn chars")
    bench("SELECT COUNT(TOKENIZE(REPEAT('a',60), 'ik')) FROM tok_zh",
          "ik  60 ascii chars")
    bench("SELECT COUNT(1) FROM tok_zh", "baseline count(*)")


if __name__ == "__main__":
    main()
