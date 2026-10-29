#!/usr/bin/env python3
"""Check whether the TOKENIZE() failure depends on the column collation."""
import json
import sys

import pymysql

sys.stdout.reconfigure(encoding="utf-8")


def escape(text):
    return text.replace("\\", "\\\\").replace("'", "''")


def main():
    data = json.load(open("data/tokenize_texts.json", encoding="utf-8"))
    text = data["zh"][6][:152]
    connection = pymysql.connect(host="127.0.0.1", port=2881, user="admin@sys",
                                 password="", charset="utf8mb4", connect_timeout=20)
    cursor = connection.cursor()
    cursor.execute("DROP DATABASE IF EXISTS ctest")
    cursor.execute("CREATE DATABASE ctest DEFAULT CHARSET=utf8mb4")
    cursor.execute("USE ctest")
    for table, collation in (("t_bin", "utf8mb4_bin"), ("t_gen", "utf8mb4_general_ci")):
        cursor.execute("CREATE TABLE %s (id INT, c LONGTEXT) DEFAULT CHARSET=utf8mb4 "
                      "COLLATE=%s" % (table, collation))
        cursor.execute("INSERT INTO %s VALUES (1, %%s)" % table, (text,))
        try:
            cursor.execute("SELECT TOKENIZE(c, 'ik') FROM %s" % table)
            rows = cursor.fetchall()
            print(collation, "OK rows=%d" % len(rows))
        except Exception as error:
            print(collation, "ERR", str(error)[:60])
            try:
                cursor.execute("SELECT 1")
                cursor.fetchall()
            except Exception as inner:
                print("   connection broken:", str(inner)[:50])
                break


if __name__ == "__main__":
    main()
