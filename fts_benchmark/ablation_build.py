#!/usr/bin/env python3
"""Ablation experiments for the fulltext index build path."""
import sys
import time

import pymysql

sys.stdout.reconfigure(encoding="utf-8")

HOST, PORT = "127.0.0.1", 2881


def connect():
    for user in ("admin@sys", "root@sys"):
        try:
            return pymysql.connect(host=HOST, port=PORT, user=user, password="",
                                   charset="utf8mb4", autocommit=True, connect_timeout=20)
        except Exception:
            continue
    raise RuntimeError("cannot connect")


def main():
    connection = connect()
    cursor = connection.cursor()
    cursor.execute("USE fts_large_bench")

    def timed(sql, ignore_error=False):
        started = time.perf_counter()
        try:
            cursor.execute(sql)
            cursor.fetchall()
        except Exception as error:
            if not ignore_error:
                raise
            return 0.0
        return time.perf_counter() - started

    # 1. how many rows do the auxiliary tables hold?
    for table, index in (("docs", "ft_ik_all"), ("docs_en", "ft_beng")):
        timed("DROP INDEX `%s` ON %s" % (index, table), True)
    cursor.execute("SELECT table_name FROM information_schema.tables "
                   "WHERE table_schema='fts_large_bench'")
    print("tables:", [row[0] for row in cursor.fetchall()])
    for table in ("docs", "docs_en"):
        for name in ("ft_ik_all", "ft_ik_content"):
            try:
                timed("DROP INDEX `%s` ON %s" % (name, table), True)
            except Exception:
                pass

    # 2. index build cost per parser
    for parser, table in (("ik", "docs"), ("space", "docs"), ("beng", "docs")):
        name = "ft_tmp_%s" % parser
        try:
            timed("DROP INDEX `%s` ON %s" % (name, table), True)
        except Exception:
            pass
        elapsed = timed("CREATE FULLTEXT INDEX `%s` ON %s (title, content) WITH PARSER %s"
                        % (name, table, parser))
        print("build %-6s on %-8s : %8.3f s" % (parser, table, elapsed))

    # 3. auxiliary table sizes
    cursor.execute("SELECT table_name, table_rows FROM information_schema.tables "
                   "WHERE table_schema='fts_large_bench'")
    for row in cursor.fetchall():
        print("aux table:", row)

    # 4. how long does an empty index build take (fixed overhead)?
    cursor.execute("DROP TABLE IF EXISTS fts_large_bench.tiny")
    cursor.execute("CREATE TABLE fts_large_bench.tiny (id BIGINT NOT NULL, "
                   "title VARCHAR(1024), content LONGTEXT, PRIMARY KEY (id))")
    elapsed = timed("CREATE FULLTEXT INDEX ft_tiny ON fts_large_bench.tiny "
                    "(title, content) WITH PARSER ik")
    print("empty ik index build: %8.3f s" % elapsed)

    # 5. half-size table to check linearity
    cursor.execute("DROP TABLE IF EXISTS fts_large_bench.half")
    cursor.execute("CREATE TABLE fts_large_bench.half (id BIGINT NOT NULL, "
                   "title VARCHAR(1024), content LONGTEXT, PRIMARY KEY (id)) "
                   "DEFAULT CHARSET=utf8mb4")
    cursor.execute("INSERT INTO fts_large_bench.half SELECT * FROM fts_large_bench.docs "
                   "WHERE id <= 7000")
    elapsed = timed("CREATE FULLTEXT INDEX ft_half ON fts_large_bench.half "
                    "(title, content) WITH PARSER ik")
    print("7000-row ik index build: %8.3f s" % elapsed)

    # cleanup
    for name, table in (("ft_tmp_ik", "docs"), ("ft_tmp_space", "docs"),
                        ("ft_tmp_beng", "docs")):
        try:
            timed("DROP INDEX `%s` ON %s" % (name, table), True)
        except Exception:
            pass


if __name__ == "__main__":
    main()
