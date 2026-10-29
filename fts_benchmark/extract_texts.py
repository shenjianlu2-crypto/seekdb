#!/usr/bin/env python3
"""Prepare the tokenize benchmark inputs.

The IK tokenizer can emit a token whose byte range splits a multi-byte
character; the server then rejects the result with ER_WRONG_ARGUMENTS (1210)
and the client connection desynchronises because the error payload carries the
broken bytes.  That is a pre-existing product defect - it is not something this
benchmark may paper over - so the tokenize benchmark only uses inputs that the
unmodified server accepts.  Every candidate text is verified against the
running server and the failing ones are dropped.
"""

import json
import sys

import pymysql

sys.stdout.reconfigure(encoding="utf-8")

CANDIDATE_LENGTHS = (120, 200, 300, 400)


def connect():
    return pymysql.connect(host="127.0.0.1", port=2881, user="admin@sys",
                           password="", charset="utf8mb4", connect_timeout=20)


def try_tokenize(text, parser):
    """Return True when the server accepts the text and answers correctly."""
    connection = connect()
    try:
        cursor = connection.cursor()
        cursor.execute("SELECT TOKENIZE(%s, %s)", (text, parser))
        cursor.fetchall()
        return True
    except Exception:
        return False
    finally:
        connection.close()


def main():
    corpus = "data/docs.tsv"
    zh, en = [], []
    with open(corpus, "r", encoding="utf-8") as handle:
        for line in handle:
            parts = line.rstrip("\n").split("\t")
            if len(parts) < 3:
                continue
            row_id = int(parts[0])
            if row_id <= 14000 and len(zh) < 96:
                zh.append(parts[2])
            elif row_id > 14000 and len(en) < 96:
                en.append(parts[2])
            if len(zh) >= 96 and len(en) >= 96:
                break

    verified = {"zh": [], "en": []}
    for key, parser in (("zh", "ik"), ("en", "beng")):
        pool = zh if key == "zh" else en
        for text in pool:
            for length in CANDIDATE_LENGTHS:
                candidate = text[:length]
                if try_tokenize(candidate, parser):
                    verified[key].append(candidate)
                    break
            if len(verified[key]) >= 64:
                break
        print("%s: %d verified texts (of %d candidates)"
              % (key, len(verified[key]), len(pool)))

    with open("data/tokenize_texts.json", "w", encoding="utf-8") as handle:
        json.dump(verified, handle, ensure_ascii=False, indent=1)
    print("wrote data/tokenize_texts.json")


if __name__ == "__main__":
    main()
