#!/usr/bin/env python3
import os, sys, time, subprocess, select, signal, shutil, math, random

BIN = os.environ.get("MVDB_BIN", "/root/round2/minivectordb")
BASE = "/root/mvdb_tests"

class DB:
    def __init__(self, dir, mode="exact", cache_mb=32):
        self.dir = dir
        self.proc = subprocess.Popen([BIN, "--data-dir", dir, "--mode", mode,
                                      "--cache-mb", str(cache_mb)],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=open(dir + ".stderr", "ab"))
        self._buf = b""
        self.wait_line("READY")
    def _read_chunk(self, timeout):
        fd = self.proc.stdout.fileno()
        r, _, _ = select.select([fd], [], [], timeout)
        if not r:
            return False
        chunk = os.read(fd, 65536)
        if not chunk:
            return False
        self._buf += chunk
        return True
    def wait_line(self, expect):
        while b"\n" not in self._buf:
            if not self._read_chunk(5.0):
                break
        line, _, rest = self._buf.partition(b"\n")
        self._buf = rest
        line = line.decode().strip()
        assert line == expect, f"expected {expect}, got {line!r}"
    def send(self, sql):
        self.proc.stdin.write((sql + ";\n").encode())
        self.proc.stdin.flush()
        return self.read_result()
    def read_result(self):
        lines = []
        while True:
            while b"\n" in self._buf:
                line, self._buf = self._buf.split(b"\n", 1)
                lines.append(line.decode().rstrip("\r"))
            if not self._read_chunk(0.15):
                break
        while b"\n" in self._buf:
            line, self._buf = self._buf.split(b"\n", 1)
            lines.append(line.decode().rstrip("\r"))
        return lines
    def kill(self):
        os.kill(self.proc.pid, signal.SIGKILL)
        self.proc.wait()
    def close(self):
        try:
            self.proc.stdin.close()
            self.proc.wait(timeout=10)
        except Exception:
            self.kill()

def fresh(name):
    d = os.path.join(BASE, name)
    if os.path.exists(d): shutil.rmtree(d)
    os.makedirs(d, exist_ok=True)
    return d

def ok(res): assert res == ["SUCCESS"], res
def fail(res): assert res == ["FAILURE"], res

def test_basic():
    db = DB(fresh("basic"))
    ok(db.send("CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(32), content VARCHAR(1024), embedding VECTOR(4))"))
    ok(db.send("INSERT INTO docs VALUES (1, 'database', '事务与恢复', '[1, 0, 0, 0]'), (2, 'database', '向量索引', '[0, 1, 0, 0]'), (3, 'travel', '旅行攻略', '[0, 0, 1, 0]')"))
    r = db.send("SELECT id, content, L2_DISTANCE(embedding, '[0.9, 0.1, 0, 0]') AS distance FROM docs WHERE category = 'database' ORDER BY distance LIMIT 2")
    assert r[0] == "id | content | distance", r
    assert r[1].startswith("1 | 事务与恢复 |"), r
    assert r[2].startswith("2 | 向量索引 |"), r
    r = db.send("SELECT * FROM docs ORDER BY id LIMIT 10")
    assert len(r) == 4, r
    r = db.send("SELECT id FROM docs WHERE id >= 2 AND id < 3 ORDER BY id")
    assert r == ["id", "2"], r
    ok(db.send("UPDATE docs SET content = 'updated' WHERE id = 3"))
    r = db.send("SELECT content FROM docs WHERE id = 3")
    assert r == ["content", "updated"], r
    ok(db.send("DELETE FROM docs WHERE id = 2"))
    r = db.send("SELECT id FROM docs ORDER BY id")
    assert r == ["id", "1", "3"], r
    fail(db.send("INSERT INTO docs VALUES (1, 'x', 'y', '[1,2,3,4]')"))
    fail(db.send("INSERT INTO docs VALUES (9, 'x', 'y', '[1,2,3]')"))
    fail(db.send("INSERT INTO docs VALUES (9, 'x', 'y', '[1,2,3,nan]')"))
    fail(db.send("SELECT * FROM docs WHERE id = 1 OR id = 2"))
    fail(db.send("ALTER TABLE docs ADD COLUMN x BIGINT"))
    # varchar unicode length
    ok(db.send("CREATE TABLE u (id BIGINT PRIMARY KEY, s VARCHAR(3))"))
    ok(db.send("INSERT INTO u VALUES (1, '你好世')"))
    fail(db.send("INSERT INTO u VALUES (2, '你好世界')"))
    # order tie -> pk asc
    ok(db.send("CREATE TABLE t2 (id BIGINT PRIMARY KEY, k BIGINT)"))
    ok(db.send("INSERT INTO t2 VALUES (3, 1), (1, 1), (2, 1)"))
    r = db.send("SELECT id FROM t2 ORDER BY k")
    assert r == ["id", "1", "2", "3"], r
    db.close()
    print("basic OK")

def test_txn():
    db = DB(fresh("txn"))
    ok(db.send("CREATE TABLE t (id BIGINT PRIMARY KEY, v VARCHAR(32), e VECTOR(2))"))
    ok(db.send("INSERT INTO t VALUES (1, 'a', '[1,0]'), (2, 'b', '[0,1]')"))
    ok(db.send("BEGIN"))
    ok(db.send("UPDATE t SET v = 'a2' WHERE id = 1"))
    ok(db.send("DELETE FROM t WHERE id = 2"))
    r = db.send("SELECT id, v FROM t ORDER BY id")
    assert r == ["id | v", "1 | a2"], r
    r = db.send("SELECT id, L2_DISTANCE(e, '[1,0]') AS d FROM t ORDER BY d LIMIT 5")
    assert len(r) == 2 and r[1].startswith("1 |"), r
    ok(db.send("ROLLBACK"))
    r = db.send("SELECT id, v FROM t ORDER BY id")
    assert r == ["id | v", "1 | a", "2 | b"], r
    # statement failure leaves txn usable
    ok(db.send("BEGIN"))
    ok(db.send("UPDATE t SET v = 'a3' WHERE id = 1"))
    fail(db.send("INSERT INTO t VALUES (1, 'dup', '[9,9]')"))
    ok(db.send("UPDATE t SET v = 'b3' WHERE id = 2"))
    ok(db.send("COMMIT"))
    r = db.send("SELECT id, v FROM t ORDER BY id")
    assert r == ["id | v", "1 | a3", "2 | b3"], r
    # failed statement atomicity: multi-row insert with dup at end
    ok(db.send("BEGIN"))
    fail(db.send("INSERT INTO t VALUES (5, 'x', '[1,1]'), (6, 'y', '[1,1]'), (1, 'dup', '[1,1]')"))
    r = db.send("SELECT id FROM t ORDER BY id")
    assert r == ["id", "1", "2"], r
    ok(db.send("COMMIT"))
    db.close()
    print("txn OK")

def test_restart():
    d = fresh("restart")
    db = DB(d)
    ok(db.send("CREATE TABLE t (id BIGINT PRIMARY KEY, e VECTOR(2))"))
    ok(db.send("INSERT INTO t VALUES (1, '[1,0]'), (2, '[0,1]')"))
    ok(db.send("BEGIN"))
    ok(db.send("INSERT INTO t VALUES (3, '[1,1]')"))
    db.close()  # normal close rolls back uncommitted
    db = DB(d)
    r = db.send("SELECT id FROM t ORDER BY id")
    assert r == ["id", "1", "2"], r
    db.close()
    print("restart OK")

def test_crash():
    d = fresh("crash")
    db = DB(d)
    ok(db.send("CREATE TABLE t (id BIGINT PRIMARY KEY, e VECTOR(2))"))
    ok(db.send("INSERT INTO t VALUES (1, '[1,0]')"))
    ok(db.send("BEGIN"))
    ok(db.send("INSERT INTO t VALUES (2, '[0,1]')"))
    db.kill()  # uncommitted txn
    db = DB(d)
    r = db.send("SELECT id FROM t ORDER BY id")
    assert r == ["id", "1"], r
    ok(db.send("INSERT INTO t VALUES (3, '[1,1]')"))  # autocommit
    ok(db.send("BEGIN"))
    ok(db.send("INSERT INTO t VALUES (4, '[2,2]')"))
    ok(db.send("COMMIT"))
    db.kill()
    db = DB(d)
    r = db.send("SELECT id FROM t ORDER BY id")
    assert r == ["id", "1", "3", "4"], r
    db.close()
    print("crash OK")

def test_big():
    d = fresh("big")
    db = DB(d, cache_mb=4)
    ok(db.send("CREATE TABLE big (id BIGINT PRIMARY KEY, pad VARCHAR(1024))"))
    random.seed(7)
    N = 30000
    for batch in range(N // 100):
        vals = ",".join(f"({batch*100+i}, '{'x'*900}')" for i in range(100))
        ok(db.send("INSERT INTO big VALUES " + vals))
    r = db.send("SELECT id FROM big WHERE id = 12345")
    assert r == ["id", "12345"], r
    r = db.send("SELECT id FROM big ORDER BY id LIMIT 3")
    assert r == ["id", "0", "1", "2"], r
    db.kill()
    db = DB(d, cache_mb=4)
    r = db.send("SELECT id FROM big WHERE id = 29999")
    assert r == ["id", "29999"], r
    db.close()
    print("big OK")

def test_ann():
    d = fresh("ann")
    db = DB(d, mode="ann")
    ok(db.send("CREATE TABLE v (id BIGINT PRIMARY KEY, cat VARCHAR(8), e VECTOR(16))"))
    random.seed(11)
    rows = []
    for i in range(3000):
        vec = [random.gauss(0, 1) for _ in range(16)]
        cat = "a" if i % 2 else "b"
        rows.append((i, cat, vec))
    for b in range(0, 3000, 100):
        vals = ",".join(f"({i}, '{c}', '[{','.join(repr(x) for x in vec)}]')" for i, c, vec in rows[b:b+100])
        ok(db.send("INSERT INTO v VALUES " + vals))
    ok(db.send("CREATE VECTOR INDEX vi ON v(e)"))
    # recall vs exact
    def topk(q, cat=None, k=10):
        cand = [r for r in rows if cat is None or r[1] == cat]
        cand.sort(key=lambda r: (math.dist(q, r[2]), r[0]))
        return [r[0] for r in cand[:k]]
    recalls = []
    for qi in range(50):
        q = [random.gauss(0, 1) for _ in range(16)]
        qs = "[" + ",".join(repr(x) for x in q) + "]"
        for cat in (None, "a"):
            where = f"WHERE cat = '{cat}'" if cat else ""
            r = db.send(f"SELECT id FROM v {where} ORDER BY L2_DISTANCE(e, '{qs}') LIMIT 10")
            got = [int(x) for x in r[1:]]
            truth = topk(q, cat)
            recalls.append(len(set(got) & set(truth)) / 10.0)
    avg = sum(recalls) / len(recalls)
    assert avg >= 0.95, f"recall {avg}"
    # index maintenance: update + delete reflected
    ok(db.send("UPDATE v SET e = '[99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99]' WHERE id = 0"))
    ok(db.send("DELETE FROM v WHERE id = 1"))
    q = [99]*16
    qs = "[" + ",".join(repr(x) for x in q) + "]"
    r = db.send(f"SELECT id FROM v ORDER BY L2_DISTANCE(e, '{qs}') LIMIT 1")
    assert r == ["id", "0"], r
    r = db.send(f"SELECT id FROM v ORDER BY L2_DISTANCE(e, '{qs}') LIMIT 3000")
    assert "1" not in r[1:], r
    db.close()
    print(f"ann OK recall={avg:.3f}")

if __name__ == "__main__":
    os.makedirs(BASE, exist_ok=True)
    which = sys.argv[1:] or ["basic", "txn", "restart", "crash", "big", "ann"]
    for name in which:
        globals()["test_" + name]()
    print("ALL PASS")