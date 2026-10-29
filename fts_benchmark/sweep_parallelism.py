#!/usr/bin/env python3
"""Sweep the fulltext index build parallelism and report wall time / cpu time."""
import subprocess
import sys
import time

import pymysql

sys.stdout.reconfigure(encoding="utf-8")


def cpu_seconds():
    out = subprocess.run(["powershell", "-NoProfile", "-Command",
                          "(Get-Process seekdb | Measure-Object -Property CPU -Sum).Sum"],
                         capture_output=True, text=True)
    try:
        return float(out.stdout.strip())
    except Exception:
        return None


def main():
    connection = pymysql.connect(host="127.0.0.1", port=2881, user="root@sys",
                                 password="", charset="utf8mb4", autocommit=True,
                                 connect_timeout=20)
    cursor = connection.cursor()
    cursor.execute("USE fts_large_bench")
    values = sys.argv[1:] or ["1", "2", "4", "8"]
    for value in values:
        try:
            cursor.execute("DROP INDEX ft_sweep ON docs")
        except Exception:
            pass
        before = cpu_seconds()
        started = time.perf_counter()
        cursor.execute("CREATE /*+ PARALLEL(%s) */ FULLTEXT INDEX ft_sweep ON docs (title, content) "
                       "WITH PARSER ik" % value)
        wall = time.perf_counter() - started
        after = cpu_seconds()
        print("parallelism=%-3s wall=%7.2fs cpu=%7.2fs -> %5.2f cores"
              % (value, wall, after - before, (after - before) / wall))


if __name__ == "__main__":
    main()
