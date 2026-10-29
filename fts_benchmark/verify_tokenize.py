#!/usr/bin/env python3
"""Dump the exact TOKENIZE() output and MATCH result sets of the FTS stack.

The dump is used to prove that the optimization did not change the tokenizer
output or the query semantics: run it against the baseline build and against
the optimized build and compare the two files byte by byte.
"""

import hashlib
import json
import sys
from pathlib import Path

import pymysql
from pymysql.converters import escape_item

sys.stdout.reconfigure(encoding="utf-8")

TEXTS = []


def load_texts():
    global TEXTS
    data = json.load(open("data/tokenize_texts.json", encoding="utf-8"))
    texts = []
    for key in ("zh", "en"):
        for text in data[key]:
            texts.append((key, text))
    # additional hand written edge cases
    texts += [
        ("zh", ""),
        ("zh", " "),
        ("zh", "a"),
        ("zh", "A"),
        ("zh", "数据库ABC数据库"),
        ("zh", "0123456789"),
        ("zh", "一、二、三，数据库"),
        ("zh", "数据库　全角空格　结束"),
        ("zh", "混合English和中文的文本"),
        ("zh", "数据库" * 30),
        ("en", ""),
        ("en", "Hello World"),
        ("en", "HELLO hello World"),
        ("en", "don't stop believing"),
        ("en", "a-b_c,d.e@f&g+h"),
        ("en", "123 456 789"),
        ("en", "the quick brown fox jumps over the lazy dog"),
        ("en", "OceanBase 4.3.5 seekdb fulltext"),
    ]
    TEXTS = texts


def main():
    out_path = Path(sys.argv[1] if len(sys.argv) > 1 else "reports/tokenize_dump.txt")
    load_texts()
    connection = pymysql.connect(host="127.0.0.1", port=2881, user="root@sys",
                                 password="", charset="utf8mb4", autocommit=True,
                                 connect_timeout=20)
    cursor = connection.cursor()
    lines = []
    for kind, text in TEXTS:
        for parser in ("ik", "beng", "space", "ngram"):
            sql = "SELECT TOKENIZE(%s, %s)" % (escape_item(text), escape_item(parser))
            # a fresh connection per statement: the server may answer with a
            # broken payload for some inputs and the protocol would desync
            scratch = pymysql.connect(host="127.0.0.1", port=2881, user="root@sys",
                                      password="", charset="utf8mb4", autocommit=True,
                                      connect_timeout=20)
            try:
                scratch_cursor = scratch.cursor()
                scratch_cursor.execute(sql)
                value = scratch_cursor.fetchone()[0]
            except Exception as error:
                value = "ERROR:%s" % (error.args[0],)
            finally:
                scratch.close()
            lines.append("%s|%s|%s|%s" % (kind, parser, hashlib.md5(text.encode("utf-8")).hexdigest(),
                                           value))
    # MATCH semantics on the benchmark corpus
    cursor.execute("USE fts_large_bench")
    cursor.execute("CREATE FULLTEXT INDEX ft_verify ON docs (title, content) WITH PARSER ik")
    try:
        for term in ("数据库", "全文索引", "查询优化", "内存管理", "并行执行", "OceanBase"):
            sql = ("SELECT id, MATCH(title, content) AGAINST('%s') FROM docs "
                   "WHERE MATCH(title, content) AGAINST('%s') ORDER BY id" % (term, term))
            cursor.execute(sql)
            rows = cursor.fetchall()
            digest = hashlib.md5(repr(rows).encode("utf-8")).hexdigest()
            lines.append("match|%s|%d|%s" % (term, len(rows), digest))
    finally:
        cursor.execute("DROP INDEX ft_verify ON docs")
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("wrote %s (%d lines)" % (out_path, len(lines)))


if __name__ == "__main__":
    main()
