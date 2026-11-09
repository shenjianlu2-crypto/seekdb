#!/usr/bin/env python3
import os, sys, time, shutil, random, subprocess, select, json

BIN = os.environ.get("MVDB_BIN", "/root/round2/minivectordb")
BASE = "/root/mvdb_bench"

class DB:
    def __init__(self, dir, mode, cache_mb=32):
        self.dir = dir
        self.proc = subprocess.Popen([BIN, "--data-dir", dir, "--mode", mode, "--cache-mb", str(cache_mb)],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=open(dir + ".stderr", "ab"))
        self._buf = b""
        assert self.read_lines(1, 5.0) == ["READY"]
    def _chunk(self, timeout):
        fd = self.proc.stdout.fileno()
        r, _, _ = select.select([fd], [], [], timeout)
        if not r:
            return False
        b = os.read(fd, 1 << 20)
        if not b:
            return False
        self._buf += b
        return True
    def read_lines(self, n, timeout=10.0):
        lines = []
        while len(lines) < n:
            while b"\n" in self._buf and len(lines) < n:
                line, self._buf = self._buf.split(b"\n", 1)
                lines.append(line.decode().rstrip("\r"))
            if len(lines) < n and not self._chunk(timeout):
                break
        return lines
    def send(self, sql, expect=1):
        self.proc.stdin.write((sql + ";\n").encode())
        self.proc.stdin.flush()
        return self.read_lines(expect)
    def stats(self):
        pid = self.proc.pid
        io = {}
        try:
            for ln in open(f"/proc/{pid}/io"):
                k, v = ln.split(":")
                io[k] = int(v)
        except FileNotFoundError:
            pass
        hwm = 0
        try:
            for ln in open(f"/proc/{pid}/status"):
                if ln.startswith("VmHWM"): hwm = int(ln.split()[1])
        except FileNotFoundError:
            pass
        return {"read_bytes": io.get("read_bytes", 0), "write_bytes": io.get("write_bytes", 0), "peak_kb": hwm}
    def close(self):
        self.proc.stdin.close()
        self.proc.wait(timeout=30)

def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "ann"
    random.seed(1234)
    N, DIM, Q, K = 10000, 128, 1000, 10
    rows = []
    for i in range(N):
        vec = [random.uniform(-1, 1) for _ in range(DIM)]
        rows.append((i, "a" if i % 2 else "b", vec))
    queries = []
    for qi in range(Q):
        q = [random.uniform(-1, 1) for _ in range(DIM)]
        queries.append((q, "a" if qi % 2 else None))
    for name in ("bench_ann", "bench_exact"):
        d = os.path.join(BASE, name)
        if os.path.exists(d): shutil.rmtree(d)
        os.makedirs(d)
    db = DB(os.path.join(BASE, "bench_ann"), mode)
    t0 = time.time()
    db.send("CREATE TABLE v (id BIGINT PRIMARY KEY, cat VARCHAR(8), e VECTOR(128))")
    for b in range(0, N, 100):
        vals = ",".join(f"({i}, '{c}', '[{','.join(f'{x:.6f}' for x in vec)}]')" for i, c, vec in rows[b:b+100])
        r = db.send("INSERT INTO v VALUES " + vals)
        assert r == ["SUCCESS"], r
    write_t = time.time() - t0
    write_stats = db.stats()
    t0 = time.time()
    db.send("CREATE VECTOR INDEX vi ON v(e)")
    prep_t = time.time() - t0
    t0 = time.time()
    results = []
    for q, cat in queries:
        qs = "[" + ",".join(f"{x:.6f}" for x in q) + "]"
        where = f"WHERE cat = '{cat}'" if cat else ""
        r = db.send(f"SELECT id FROM v {where} ORDER BY L2_DISTANCE(e, '{qs}') LIMIT {K}", K + 1)
        assert len(r) == K + 1, r
        results.append([int(x) for x in r[1:]])
    query_t = time.time() - t0
    qstats = db.stats()
    db.close()

    dbe = DB(os.path.join(BASE, "bench_exact"), "exact")
    dbe.send("CREATE TABLE v (id BIGINT PRIMARY KEY, cat VARCHAR(8), e VECTOR(128))")
    for b in range(0, N, 100):
        vals = ",".join(f"({i}, '{c}', '[{','.join(f'{x:.6f}' for x in vec)}]')" for i, c, vec in rows[b:b+100])
        dbe.send("INSERT INTO v VALUES " + vals)
    recalls = []
    for (q, cat), got in zip(queries, results):
        qs = "[" + ",".join(f"{x:.6f}" for x in q) + "]"
        where = f"WHERE cat = '{cat}'" if cat else ""
        r = dbe.send(f"SELECT id FROM v {where} ORDER BY L2_DISTANCE(e, '{qs}') LIMIT {K}", K + 1)
        truth = [int(x) for x in r[1:]]
        recalls.append(len(set(got) & set(truth)) / float(min(K, len(truth))))
    dbe.close()
    ra = sum(recalls[0::2]) / len(recalls[0::2])
    rb = sum(recalls[1::2]) / len(recalls[1::2])
    qps = Q / query_t if query_t > 0 else 0
    print(json.dumps({"mode": mode, "write_s": round(write_t, 3), "prep_s": round(prep_t, 3),
                      "query_s": round(query_t, 3), "qps": round(qps, 1),
                      "recall_filtered": round(ra, 4), "recall_unfiltered": round(rb, 4),
                      "write_io": write_stats, "query_io": qstats}, indent=1))
    assert ra >= 0.95 and rb >= 0.95, "recall below 0.95"

if __name__ == "__main__":
    main()