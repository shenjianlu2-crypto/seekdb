#!/usr/bin/env python3
"""Local reproduction of the OceanBase seekdb full-text index benchmark.

Mirrors the parameters described by the competition problem statement:

  * 20,000 mixed Chinese/English documents, imported 500 rows per batch
  * index build: IK on (title, content), IK on (content), BENG on (title, content)
  * tokenize : 3,000 executions per group, 3 groups, 30 warmup executions
  * query   : 200 executions per query per group, 3 groups, 30 warmup executions
  * every metric is reported as "group total time / execution count"

Auxiliary counters (import speed, SELECT 1 overhead, MATCH hit counts) are
collected as well; they do not take part in the score but they are needed to
prove that the optimization did not change any result.
"""

import argparse
import json
import os
import statistics
import sys
import time
from pathlib import Path

import pymysql

sys.stdout.reconfigure(encoding="utf-8")

DATABASE = "fts_large_bench"
BATCH_ROWS = 500
TOKENIZE_RUNS = 3000
TOKENIZE_GROUPS = 3
TOKENIZE_WARMUP = 30
QUERY_RUNS = 200
QUERY_GROUPS = 3
QUERY_WARMUP = 30
ZH_ROWS = 14000

# Terms that must actually hit the corpus; the hit counts are compared between
# the baseline build and the optimized build to prove result equivalence.
CN_TERMS = ["数据库", "全文索引", "查询优化", "内存管理", "存储引擎", "并行执行"]
EN_TERMS = ["database", "index", "query", "memory", "storage", "throughput"]
MIXED_TERMS = ["数据库 index", "内存 memory", "存储 storage"]

CREATE_TABLE = """
CREATE TABLE {table} (
  id BIGINT NOT NULL,
  title VARCHAR(1024),
  content LONGTEXT,
  PRIMARY KEY (id)
) DEFAULT CHARSET=utf8mb4
"""

INDEX_SPECS = [
    # (table, index name, columns, parser, metric)
    ("docs", "ft_ik_all", "title, content", "ik", "build_ik_all_sec"),
    ("docs", "ft_ik_content", "content", "ik", "build_ik_content_sec"),
    ("docs_en", "ft_beng", "title, content", "beng", "build_beng_en_sec"),
]


def log(message):
    print("[%s] %s" % (time.strftime("%H:%M:%S"), message), flush=True)


class Bench:
    def __init__(self, options):
        self.options = options
        self.connection = None
        last_error = None
        for user in options.user.split(","):
            try:
                self.connection = pymysql.connect(host=options.host, port=options.port,
                                                 user=user.strip(), password=options.password,
                                                 charset="utf8mb4", autocommit=True,
                                                 connect_timeout=30)
                break
            except Exception as error:
                last_error = error
        if self.connection is None:
            raise last_error
        self.cursor = self.connection.cursor()
        self.report = {"metrics": {}, "aux": {}, "hits": {}, "groups": {}}

    # ---------------------------------------------------------------- helpers
    def execute(self, sql, fetch=False):
        started = time.perf_counter()
        self.cursor.execute(sql)
        rows = self.cursor.fetchall() if fetch else None
        elapsed = time.perf_counter() - started
        return elapsed, rows

    def scalar(self, sql):
        self.cursor.execute(sql)
        row = self.cursor.fetchone()
        return row[0] if row else None

    def drain(self):
        while self.cursor.nextset():
            pass

    # ------------------------------------------------------------------ setup
    def prepare_schema(self):
        log("recreating database %s" % DATABASE)
        self.cursor.execute("DROP DATABASE IF EXISTS %s" % DATABASE)
        self.cursor.execute("CREATE DATABASE %s DEFAULT CHARSET=utf8mb4" % DATABASE)
        self.cursor.execute("USE %s" % DATABASE)
        # required to attach a fulltext index to a table that already holds data
        self.cursor.execute("ALTER SYSTEM SET "
                            "_ENABLE_ADD_FULLTEXT_INDEX_TO_EXISTING_TABLE = TRUE")
        for table in ("docs", "docs_en"):
            self.cursor.execute(CREATE_TABLE.format(table=table))

    def load_data(self):
        path = Path(self.options.data).resolve()
        rows = []
        with path.open("r", encoding="utf-8") as handle:
            for line in handle:
                parts = line.rstrip("\n").split("\t")
                rows.append(parts)
        zh_rows = [r for r in rows if int(r[0]) <= ZH_ROWS]
        en_rows = [r for r in rows if int(r[0]) > ZH_ROWS]
        log("loading %d zh rows and %d en rows (%d rows per batch)"
            % (len(zh_rows), len(en_rows), BATCH_ROWS))

        started = time.perf_counter()
        for table, table_rows in (("docs", zh_rows), ("docs_en", en_rows)):
            for offset in range(0, len(table_rows), BATCH_ROWS):
                chunk = table_rows[offset:offset + BATCH_ROWS]
                values = ",".join(
                    "(%s,'%s','%s')" % (row[0], row[1].replace("\\", "\\\\").replace("'", "''"),
                                        row[2].replace("\\", "\\\\").replace("'", "''"))
                    for row in chunk)
                self.cursor.execute("INSERT INTO %s (id, title, content) VALUES %s"
                                    % (table, values))
        elapsed = time.perf_counter() - started
        self.report["aux"]["import_sec"] = elapsed
        self.report["aux"]["import_rows"] = len(rows)
        log("import finished in %.2fs (%.0f rows/s)" % (elapsed, len(rows) / elapsed))

    # ------------------------------------------------------------ build phase
    def drop_all_indexes(self):
        # Fulltext indexes are not listed by SHOW INDEX, so drop them by name.
        for table, name in (("docs", "ft_ik_all"), ("docs", "ft_ik_content"),
                            ("docs_en", "ft_beng")):
            try:
                self.cursor.execute("DROP INDEX `%s` ON %s" % (name, table))
            except pymysql.err.OperationalError as error:
                if error.args[0] != 1091:  # ER_CANT_DROP_FIELD_OR_KEY
                    raise

    def warmup_build(self):
        """Build and drop a tiny index so that the dictionary and the DDL path
        are already loaded when the measured builds start."""
        self.cursor.execute("DROP TABLE IF EXISTS warmup_fts")
        self.cursor.execute("CREATE TABLE warmup_fts (id BIGINT NOT NULL, "
                            "title VARCHAR(1024), content LONGTEXT, PRIMARY KEY (id)) "
                            "DEFAULT CHARSET=utf8mb4")
        self.cursor.execute("INSERT INTO warmup_fts VALUES (1, 'warm up', 'warm up')")
        self.cursor.execute("CREATE FULLTEXT INDEX ft_warmup ON warmup_fts "
                            "(title, content) WITH PARSER ik")
        self.cursor.execute("DROP INDEX ft_warmup ON warmup_fts")
        self.cursor.execute("DROP TABLE warmup_fts")
        log("build path warmed up")

    def measure_build(self):
        for table, name, columns, parser, metric in INDEX_SPECS:
            self.drop_all_indexes()
            sql = ("CREATE FULLTEXT INDEX `%s` ON %s (%s) WITH PARSER %s"
                   % (name, table, columns, parser))
            started = time.perf_counter()
            self.cursor.execute(sql)
            elapsed = time.perf_counter() - started
            self.report["metrics"][metric] = elapsed
            log("%-24s %8.3f s" % (metric, elapsed))

    # --------------------------------------------------------- tokenize phase
    def prepare_tokenize_tables(self):
        """Materialise the tokenize inputs so one statement performs 3,000 calls."""
        texts = json.loads(Path("data/tokenize_texts.json").read_text(encoding="utf-8"))
        self.cursor.execute("DROP TABLE IF EXISTS tok_zh")
        self.cursor.execute("DROP TABLE IF EXISTS tok_en")
        self.cursor.execute("CREATE TABLE tok_zh (id INT NOT NULL, t VARCHAR(1024), "
                            "PRIMARY KEY (id)) DEFAULT CHARSET=utf8mb4")
        self.cursor.execute("CREATE TABLE tok_en (id INT NOT NULL, t VARCHAR(1024), "
                            "PRIMARY KEY (id)) DEFAULT CHARSET=utf8mb4")
        rows_zh, rows_en = [], []
        for index in range((TOKENIZE_GROUPS + 1) * TOKENIZE_RUNS):
            rows_zh.append((index + 1, texts["zh"][index % len(texts["zh"])]))
            rows_en.append((index + 1, texts["en"][index % len(texts["en"])]))
        for table, rows in (("tok_zh", rows_zh), ("tok_en", rows_en)):
            for offset in range(0, len(rows), 500):
                chunk = rows[offset:offset + 500]
                values = ",".join("(%d,'%s')" % (row[0],
                                                 row[1].replace("\\", "\\\\").replace("'", "''"))
                                  for row in chunk)
                self.cursor.execute("INSERT INTO %s (id, t) VALUES %s" % (table, values))
        log("tokenize input tables ready (%d rows each)" % len(rows_zh))

    def measure_tokenize(self, parser, table, metric):
        groups = []
        for group in range(TOKENIZE_GROUPS + 1):
            low = group * TOKENIZE_RUNS + 1
            high = low + TOKENIZE_RUNS - 1
            sql = ("SELECT COUNT(TOKENIZE(t, '%s')) FROM %s "
                   "WHERE id >= %d AND id <= %d" % (parser, table, low, high))
            for _ in range(TOKENIZE_WARMUP):
                self.cursor.execute(sql)
                self.cursor.fetchall()
            started = time.perf_counter()
            self.cursor.execute(sql)
            fetched = self.cursor.fetchall()
            elapsed = time.perf_counter() - started
            if group == 0:
                # the first group only warms the code path up
                continue
            groups.append(elapsed * 1000.0 / TOKENIZE_RUNS)
        value = statistics.mean(groups)
        self.report["metrics"][metric] = value
        self.report["groups"][metric] = groups
        log("%-24s %8.4f ms  groups=%s" % (metric, value,
                                           ["%.4f" % g for g in groups]))

    # ------------------------------------------------------------ query phase
    def measure_query(self, sql, hit_key):
        """Run one query QUERY_RUNS times per group; return the mean avg_ms.

        The hit count is taken from the result itself, which is the "查询命中数量"
        the official benchmark records as an auxiliary correctness signal.
        """
        groups = []
        hits = set()
        for group in range(QUERY_GROUPS):
            for _ in range(QUERY_WARMUP):
                self.cursor.execute(sql)
                self.cursor.fetchall()
            started = time.perf_counter()
            for _ in range(QUERY_RUNS):
                self.cursor.execute(sql)
                rows = self.cursor.fetchall()
            elapsed = time.perf_counter() - started
            groups.append(elapsed * 1000.0 / QUERY_RUNS)
            hits.add(int(rows[0][0]) if len(rows[0]) == 1 else len(rows))
        value = statistics.mean(groups)
        if hit_key:
            self.report["hits"][hit_key] = sorted(hits)
        return value

    def measure_queries(self):
        # IK side: Chinese, mixed and LIMIT queries against the IK index.
        cn_values = []
        for term in CN_TERMS:
            sql = ("SELECT COUNT(*) FROM docs "
                   "WHERE MATCH(title, content) AGAINST('%s')" % term)
            value = self.measure_query(sql, "cn:%s" % term)
            cn_values.append(value)
            log("  cn  %-24s %8.4f ms" % (term, value))
        self.report["metrics"]["query_cn_avg_ms"] = statistics.mean(cn_values)
        self.report["groups"]["query_cn_avg_ms"] = cn_values

        mixed_values = []
        for term in MIXED_TERMS:
            sql = ("SELECT COUNT(*) FROM docs "
                   "WHERE MATCH(title, content) AGAINST('%s')" % term)
            value = self.measure_query(sql, "mixed:%s" % term)
            mixed_values.append(value)
            log("  mix %-24s %8.4f ms" % (term, value))
        self.report["metrics"]["query_mixed_avg_ms"] = statistics.mean(mixed_values)
        self.report["groups"]["query_mixed_avg_ms"] = mixed_values

        limit_sql = ("SELECT id FROM docs WHERE MATCH(title, content) AGAINST('%s') "
                     "ORDER BY id LIMIT 10" % CN_TERMS[1])
        limit_value = self.measure_query(limit_sql, "limit")
        self.report["metrics"]["query_limit_avg_ms"] = limit_value
        self.report["groups"]["query_limit_avg_ms"] = [limit_value]
        log("  limit %-22s %8.4f ms" % (CN_TERMS[1], limit_value))

        # BENG side: English queries against the English table.
        beng_values = []
        for term in EN_TERMS:
            sql = ("SELECT COUNT(*) FROM docs_en "
                   "WHERE MATCH(title, content) AGAINST('%s')" % term)
            value = self.measure_query(sql, "beng:%s" % term)
            beng_values.append(value)
            log("  eng %-24s %8.4f ms" % (term, value))
        self.report["metrics"]["query_beng_avg_ms"] = statistics.mean(beng_values)
        self.report["groups"]["query_beng_avg_ms"] = beng_values

    # ------------------------------------------------------------------ extra
    def measure_select1(self):
        for _ in range(QUERY_WARMUP):
            self.cursor.execute("SELECT 1")
            self.cursor.fetchall()
        started = time.perf_counter()
        for _ in range(200):
            self.cursor.execute("SELECT 1")
            self.cursor.fetchall()
        elapsed = (time.perf_counter() - started) * 1000.0 / 200
        self.report["aux"]["select1_avg_ms"] = elapsed
        log("%-24s %8.4f ms" % ("select1_avg_ms", elapsed))

    def collect_hits(self):
        """Record the exact MATCH hit sets for result equivalence checking."""
        hits = {}
        for table, parser in (("docs", "ik"), ("docs_en", "beng")):
            terms = CN_TERMS if parser == "ik" else EN_TERMS
            for term in terms:
                sql = ("SELECT id FROM %s WHERE MATCH(title, content) AGAINST('%s') "
                       "ORDER BY id" % (table, term))
                self.cursor.execute(sql)
                hits["%s|%s" % (table, term)] = [row[0] for row in self.cursor.fetchall()]
        self.report["hit_sets"] = hits
        log("collected %d hit sets" % len(hits))

    def main(self):
        if not self.options.skip_load:
            self.prepare_schema()
            self.load_data()
        else:
            self.cursor.execute("USE %s" % DATABASE)

        self.prepare_tokenize_tables()
        self.warmup_build()

        self.measure_build()
        self.drop_all_indexes()

        # query phase needs the indexes in place
        self.cursor.execute("CREATE FULLTEXT INDEX ft_ik_all ON docs "
                            "(title, content) WITH PARSER ik")
        self.cursor.execute("CREATE FULLTEXT INDEX ft_beng ON docs_en "
                            "(title, content) WITH PARSER beng")
        self.measure_queries()
        self.collect_hits()
        self.drop_all_indexes()

        self.measure_tokenize("ik", "tok_zh", "tokenize_ik_avg_ms")
        self.measure_tokenize("beng", "tok_en", "tokenize_beng_avg_ms")

        self.measure_select1()

        report = {
            "meta": {
                "host": self.options.host,
                "port": self.options.port,
                "timestamp": time.strftime("%Y-%m-%d %H:%M:%S"),
                "label": self.options.label,
                "rows": 20000,
                "batch_rows": BATCH_ROWS,
                "tokenize_runs": TOKENIZE_RUNS,
                "query_runs": QUERY_RUNS,
            },
            "metrics": self.report["metrics"],
            "groups": self.report["groups"],
            "aux": self.report["aux"],
            "hits": self.report["hits"],
            "hit_sets": self.report.get("hit_sets", {}),
        }
        out = Path(self.options.out)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        log("report written to %s" % out)
        return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=2881)
    parser.add_argument("--user", default="admin@sys,root@sys")
    parser.add_argument("--password", default="")
    parser.add_argument("--data", default="data/docs.tsv")
    parser.add_argument("--out", default="report.json")
    parser.add_argument("--label", default="run")
    parser.add_argument("--skip-load", action="store_true")
    options = parser.parse_args()
    os.chdir(Path(__file__).resolve().parent)
    return Bench(options).main()


if __name__ == "__main__":
    sys.exit(main())
