#!/usr/bin/env python3
"""Wait until the seekdb instance accepts connections."""
import sys
import time

import pymysql

sys.stdout.reconfigure(encoding="utf-8")


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 2881
    for attempt in range(60):
        try:
            for user in ("admin@sys", "root@sys"):
                try:
                    connection = pymysql.connect(host=host, port=port, user=user,
                                                 password="", charset="utf8mb4", connect_timeout=10)
                    cursor = connection.cursor()
                    cursor.execute("SELECT VERSION()")
                    print("ready after %ds as %s: %s"
                          % (attempt * 5, user, cursor.fetchone()[0]))
                    connection.close()
                    return 0
                except Exception:
                    continue
        except Exception:
            time.sleep(5)
    print("server did not become ready")
    return 1


if __name__ == "__main__":
    sys.exit(main())
