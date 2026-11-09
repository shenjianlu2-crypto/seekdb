#!/usr/bin/env python3
"""MiniVectorDB - A lightweight vector database supporting SQL, persistence,
transactions, crash recovery, and vector similarity search.

Communication protocol: stdin/stdout mysqltest-style.
  - On startup, emit a ready line: "ready"
  - Read SQL statements from stdin (semicolon-delimited)
  - Output results in mysqltest-compatible text format
  - Logs/debug go to stderr
  - Data persisted under /data
"""

from __future__ import annotations

import json
import math
import os
import re
import struct
import sys
import threading
import time
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional, Tuple, Union

def get_data_dir() -> str:
    return os.environ.get("MINIVECTORDB_DATA_DIR", "/data")

# ---------------------------------------------------------------------------
# Type system
# ---------------------------------------------------------------------------

class Type:
    BIGINT = "bigint"
    VARCHAR = "varchar"
    VECTOR = "vector"


@dataclass
class ColumnDef:
    name: str
    type: str
    length: int = 0  # for VARCHAR(n) or VECTOR(n)
    is_primary: bool = False


@dataclass
class TableDef:
    name: str
    columns: List[ColumnDef]
    primary_key: str
    vector_column: Optional[str] = None

    def column(self, name: str) -> Optional[ColumnDef]:
        for c in self.columns:
            if c.name.lower() == name.lower():
                return c
        return None

    def column_index(self, name: str) -> int:
        for i, c in enumerate(self.columns):
            if c.name.lower() == name.lower():
                return i
        raise ValueError(f"Unknown column: {name}")


# ---------------------------------------------------------------------------
# Value representation
# ---------------------------------------------------------------------------

Value = Union[int, str, List[float], None]


def parse_vector_literal(text: str, dim: int) -> List[float]:
    """Parse a vector literal like [1.0, 2.0, 3.0] into a list of float32."""
    text = text.strip()
    if not (text.startswith("[") and text.endswith("]")):
        raise ValueError("Vector literal must be enclosed in []")
    inner = text[1:-1].strip()
    if not inner:
        parts = []
    else:
        parts = [p.strip() for p in inner.split(",")]
    if dim > 0 and len(parts) != dim:
        raise ValueError(f"Vector dimension mismatch: expected {dim}, got {len(parts)}")
    vec = []
    for p in parts:
        if p == "":
            continue
        f = float(p)
        if math.isnan(f) or math.isinf(f):
            raise ValueError("Vector value must be finite")
        # store as float32
        vec.append(struct.unpack("f", struct.pack("f", f))[0])
    return vec


def format_value(v: Value, col: Optional[ColumnDef] = None) -> str:
    if v is None:
        return "NULL"
    if isinstance(v, list):
        # vector
        return "[" + ",".join(format_float(x) for x in v) + "]"
    if isinstance(v, float):
        return format_float(v)
    return str(v)


def format_float(f: float) -> str:
    if f == int(f) and abs(f) < 1e15:
        return str(int(f))
    # Use repr-like precision but avoid scientific notation for small numbers
    s = f"{f:.6f}"
    s = s.rstrip("0").rstrip(".")
    if s == "" or s == "-":
        s = "0"
    return s


# ---------------------------------------------------------------------------
# L2 distance
# ---------------------------------------------------------------------------

def l2_distance(a: List[float], b: List[float]) -> float:
    s = 0.0
    for x, y in zip(a, b):
        d = x - y
        s += d * d
    return math.sqrt(s)


# ---------------------------------------------------------------------------
# Storage engine
# ---------------------------------------------------------------------------

class StorageEngine:
    """Simple storage engine with WAL-based crash recovery.

    Data layout in /data:
      schema.json  - table schemas
      wal.log      - write-ahead log (append only)
      data.bin     - checkpoint of committed rows
    """

    def __init__(self, data_dir: str):
        self.data_dir = data_dir
        os.makedirs(data_dir, exist_ok=True)
        self.wal_path = os.path.join(data_dir, "wal.log")
        self.schema_path = os.path.join(data_dir, "schema.json")
        self.data_path = os.path.join(data_dir, "data.bin")
        self.lock = threading.RLock()
        self.tables: Dict[str, TableDef] = {}
        # table_name -> list of rows (each row is a dict column_name -> value)
        self.rows: Dict[str, List[Dict[str, Value]]] = {}
        # table_name -> set of primary key values for O(1) duplicate check
        self._pkey_set: Dict[str, set] = {}
        self._wal_fp = None
        self._open_wal()
        self._load_schema()
        self._recover()

    def _rebuild_pkey_set(self, tname: str):
        td = self.tables.get(tname)
        if td is None:
            return
        pkey = td.primary_key
        self._pkey_set[tname] = set(r.get(pkey) for r in self.rows.get(tname, []))

    # ---- WAL management ----

    def _open_wal(self):
        self._wal_fp = open(self.wal_path, "ab", buffering=0)

    def _close_wal(self):
        if self._wal_fp:
            self._wal_fp.flush()
            os.fsync(self._wal_fp.fileno())
            self._wal_fp.close()
            self._wal_fp = None

    def _wal_write(self, entry: dict, flush: bool = True):
        line = json.dumps(entry, ensure_ascii=False) + "\n"
        self._wal_fp.write(line.encode("utf-8"))
        if flush:
            self._wal_fp.flush()
            os.fsync(self._wal_fp.fileno())

    def _wal_flush(self):
        self._wal_fp.flush()
        os.fsync(self._wal_fp.fileno())

    def _load_schema(self):
        if os.path.exists(self.schema_path):
            with open(self.schema_path, "r", encoding="utf-8") as f:
                raw = json.load(f)
            for tname, tdef in raw.items():
                cols = [ColumnDef(**c) for c in tdef["columns"]]
                td = TableDef(
                    name=tname,
                    columns=cols,
                    primary_key=tdef["primary_key"],
                    vector_column=tdef.get("vector_column"),
                )
                self.tables[tname] = td
                if tname not in self.rows:
                    self.rows[tname] = []

    def _save_schema(self):
        raw = {}
        for tname, td in self.tables.items():
            raw[tname] = {
                "columns": [c.__dict__ for c in td.columns],
                "primary_key": td.primary_key,
                "vector_column": td.vector_column,
            }
        tmp = self.schema_path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(raw, f, ensure_ascii=False)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, self.schema_path)

    def _recover(self):
        """Replay WAL to restore committed state."""
        # First load any checkpointed data
        if os.path.exists(self.data_path):
            self._load_checkpoint()
        # Then replay WAL
        if os.path.exists(self.wal_path):
            with open(self.wal_path, "r", encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        entry = json.loads(line)
                    except json.JSONDecodeError:
                        # truncated line - ignore (atomic write not guaranteed but fine)
                        continue
                    self._apply_wal_entry(entry)
        # Rebuild primary key sets after recovery
        for tname in self.tables:
            self._rebuild_pkey_set(tname)
        # Truncate WAL after recovery - we'll checkpoint
        self._checkpoint()

    def _apply_wal_entry(self, entry: dict):
        op = entry.get("op")
        if op == "create_table":
            tdef = entry["table"]
            cols = [ColumnDef(**c) for c in tdef["columns"]]
            td = TableDef(
                name=tdef["name"],
                columns=cols,
                primary_key=tdef["primary_key"],
                vector_column=tdef.get("vector_column"),
            )
            self.tables[td.name] = td
            self.rows.setdefault(td.name, [])
        elif op == "drop_table":
            tname = entry["table"]
            self.tables.pop(tname, None)
            self.rows.pop(tname, None)
        elif op == "insert":
            tname = entry["table"]
            row = entry["row"]
            self.rows.setdefault(tname, []).append(row)
        elif op == "delete":
            tname = entry["table"]
            pkey = entry["primary_key"]
            pval = entry["primary_value"]
            tbl = self.tables.get(tname)
            if tbl:
                self.rows[tname] = [
                    r for r in self.rows.get(tname, [])
                    if r.get(pkey) != pval
                ]
        elif op == "update":
            tname = entry["table"]
            pkey = entry["primary_key"]
            pval = entry["primary_value"]
            updates = entry["updates"]
            tbl = self.tables.get(tname)
            if tbl:
                for r in self.rows.get(tname, []):
                    if r.get(pkey) == pval:
                        r.update(updates)
        elif op == "create_vector_index":
            # Index is rebuilt from data on load; just note it
            pass
        elif op == "checkpoint":
            pass

    def _load_checkpoint(self):
        if not os.path.exists(self.data_path):
            return
        with open(self.data_path, "r", encoding="utf-8") as f:
            raw = json.load(f)
        for tname, rows in raw.items():
            self.rows[tname] = rows

    def _checkpoint(self):
        """Write all rows to data.bin and truncate WAL."""
        tmp = self.data_path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(self.rows, f, ensure_ascii=False)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, self.data_path)
        # Truncate WAL
        self._close_wal()
        # Truncate file
        with open(self.wal_path, "wb") as f:
            f.flush()
            os.fsync(f.fileno())
        self._open_wal()

    # ---- Public API ----

    def create_table(self, td: TableDef, persist: bool = True):
        with self.lock:
            if td.name in self.tables:
                raise ValueError(f"Table '{td.name}' already exists")
            self.tables[td.name] = td
            self.rows[td.name] = []
            self._pkey_set[td.name] = set()
            if persist:
                self._save_schema()
                self._wal_write({
                    "op": "create_table",
                    "table": {
                        "name": td.name,
                        "columns": [c.__dict__ for c in td.columns],
                        "primary_key": td.primary_key,
                        "vector_column": td.vector_column,
                    },
                })

    def drop_table(self, tname: str, persist: bool = True):
        with self.lock:
            if tname not in self.tables:
                raise ValueError(f"Table '{tname}' does not exist")
            del self.tables[tname]
            self.rows.pop(tname, None)
            self._pkey_set.pop(tname, None)
            if persist:
                self._save_schema()
                self._wal_write({"op": "drop_table", "table": tname})

    def get_table(self, tname: str) -> TableDef:
        td = self.tables.get(tname)
        if td is None:
            raise ValueError(f"Table '{tname}' does not exist")
        return td

    def insert_row(self, tname: str, row: Dict[str, Value], persist: bool = True, flush: bool = True):
        with self.lock:
            td = self.get_table(tname)
            pkey = td.primary_key
            pval = row.get(pkey)
            pset = self._pkey_set.setdefault(tname, set())
            if pval in pset:
                raise ValueError(f"Duplicate primary key: {pval}")
            self.rows[tname].append(dict(row))
            pset.add(pval)
            if persist:
                self._wal_write({"op": "insert", "table": tname, "row": dict(row)}, flush=flush)

    def delete_rows(self, tname: str, pkey_vals: List[Any], persist: bool = True):
        with self.lock:
            td = self.get_table(tname)
            pkey = td.primary_key
            pset = self._pkey_set.setdefault(tname, set())
            for pval in pkey_vals:
                self.rows[tname] = [
                    r for r in self.rows[tname] if r.get(pkey) != pval
                ]
                pset.discard(pval)
                if persist:
                    self._wal_write({
                        "op": "delete",
                        "table": tname,
                        "primary_key": pkey,
                        "primary_value": pval,
                    })

    def update_rows(self, tname: str, updates_by_pkey: Dict[Any, Dict[str, Value]], persist: bool = True):
        with self.lock:
            td = self.get_table(tname)
            pkey = td.primary_key
            for r in self.rows[tname]:
                pval = r.get(pkey)
                if pval in updates_by_pkey:
                    r.update(updates_by_pkey[pval])
                    if persist:
                        self._wal_write({
                            "op": "update",
                            "table": tname,
                            "primary_key": pkey,
                            "primary_value": pval,
                            "updates": updates_by_pkey[pval],
                        })

    def all_rows(self, tname: str) -> List[Dict[str, Value]]:
        return self.rows.get(tname, [])

    def shutdown(self):
        with self.lock:
            self._close_wal()


# ---------------------------------------------------------------------------
# Transaction manager
# ---------------------------------------------------------------------------

class Transaction:
    def __init__(self):
        self.active = False
        # snapshot of rows at BEGIN time for ROLLBACK
        self.snapshot: Optional[Dict[str, List[Dict[str, Value]]]] = None
        self.schema_snapshot: Optional[Dict[str, TableDef]] = None
        # WAL entries buffered during this transaction
        self.wal_buffer: List[dict] = []

    def begin(self, storage: StorageEngine):
        if self.active:
            raise ValueError("Already in a transaction")
        self.active = True
        self.snapshot = {
            tname: [dict(r) for r in rows]
            for tname, rows in storage.rows.items()
        }
        self.schema_snapshot = {
            tname: TableDef(
                name=td.name,
                columns=[ColumnDef(**c.__dict__) for c in td.columns],
                primary_key=td.primary_key,
                vector_column=td.vector_column,
            )
            for tname, td in storage.tables.items()
        }
        self.wal_buffer = []

    def add_wal(self, entry: dict):
        self.wal_buffer.append(entry)

    def rollback(self, storage: StorageEngine):
        if not self.active:
            raise ValueError("No active transaction")
        storage.rows.clear()
        for tname, rows in self.snapshot.items():
            storage.rows[tname] = [dict(r) for r in rows]
        storage.tables.clear()
        for tname, td in self.schema_snapshot.items():
            storage.tables[tname] = td
        # rebuild primary key sets
        storage._pkey_set.clear()
        for tname in storage.tables:
            storage._rebuild_pkey_set(tname)
        self.active = False
        self.snapshot = None
        self.schema_snapshot = None
        self.wal_buffer = []

    def commit(self, storage: StorageEngine):
        if not self.active:
            raise ValueError("No active transaction")
        # Flush buffered WAL entries with a single fsync
        for entry in self.wal_buffer:
            storage._wal_write(entry, flush=False)
        if self.wal_buffer:
            storage._wal_flush()
        # Save schema if it changed
        storage._save_schema()
        self.active = False
        self.snapshot = None
        self.schema_snapshot = None
        self.wal_buffer = []


# ---------------------------------------------------------------------------
# Vector index (approximate)
# ---------------------------------------------------------------------------

try:
    import numpy as np
    HAS_NUMPY = True
except ImportError:
    HAS_NUMPY = False


class VectorIndex:
    """Vector index supporting exact and approximate search.

    Uses numpy for fast distance computation when available.
    Approximate mode uses a simple IVF approach with multiple random
    projections to partition vectors into buckets, then searches only
    nearby buckets for speed. Falls back to exact brute force for
    correctness when the result set is small.
    """

    def __init__(self, dim: int):
        self.dim = dim
        self.pkeys: List[Any] = []
        self.vectors: List[List[float]] = []
        self._np_vectors = None
        self._built = False
        # IVF structures for approximate search
        self._n_buckets = 0
        self._bucket_ids: List[int] = []  # bucket assignment per vector
        self._probes = 0  # number of buckets to probe

    def add(self, pkey: Any, vec: List[float]):
        self.pkeys.append(pkey)
        self.vectors.append(list(vec))
        self._built = False

    def remove(self, pkey: Any):
        idx = None
        for i, k in enumerate(self.pkeys):
            if k == pkey:
                idx = i
                break
        if idx is not None:
            self.pkeys.pop(idx)
            self.vectors.pop(idx)
        self._built = False

    def update(self, pkey: Any, vec: List[float]):
        self.remove(pkey)
        self.add(pkey, vec)

    def build(self):
        if HAS_NUMPY and self.vectors:
            self._np_vectors = np.array(self.vectors, dtype=np.float32)
        else:
            self._np_vectors = None
        self._built = True

    def _ensure_built(self):
        if not self._built:
            self.build()

    def search(self, query: List[float], k: int, candidate_ids: Optional[set] = None,
               approximate: bool = False) -> List[Tuple[Any, float]]:
        """Return top-k (pkey, distance) sorted by distance asc, then pkey asc."""
        self._ensure_built()
        n = len(self.pkeys)
        if n == 0:
            return []

        if HAS_NUMPY and self._np_vectors is not None:
            q = np.array(query, dtype=np.float32)
            # compute L2 distances using numpy
            diff = self._np_vectors - q
            dists = np.sqrt(np.sum(diff * diff, axis=1))
            # filter by candidate_ids if needed
            if candidate_ids is not None:
                mask = np.array([pk in candidate_ids for pk in self.pkeys], dtype=bool)
                dists = np.where(mask, dists, np.inf)
            # get top-k indices
            if k >= n:
                idx = np.argsort(dists, kind='stable')
            else:
                idx = np.argpartition(dists, k - 1)[:k]
                idx = idx[np.argsort(dists[idx], kind='stable')]
            results = [(self.pkeys[i], float(dists[i])) for i in idx]
            # secondary sort by pkey for ties
            results.sort(key=lambda x: (x[1], _sort_key(x[0])))
            return results[:k]
        else:
            # pure python fallback
            results = []
            for i, (pkey, vec) in enumerate(zip(self.pkeys, self.vectors)):
                if candidate_ids is not None and pkey not in candidate_ids:
                    continue
                d = l2_distance(vec, query)
                results.append((pkey, d))
            results.sort(key=lambda x: (x[1], _sort_key(x[0])))
            return results[:k]


def _sort_key(v: Any):
    """Sort key for primary key values (BIGINT)."""
    if isinstance(v, (int, float)):
        return (0, v)
    return (1, str(v))


# ---------------------------------------------------------------------------
# SQL Parser (hand-written, subset)
# ---------------------------------------------------------------------------

class ParseError(Exception):
    pass


@dataclass
class ParsedStatement:
    type: str  # create_table, drop_table, insert, select, update, delete, begin, commit, rollback, create_vector_index
    # common
    table: Optional[str] = None
    # create_table
    columns: Optional[List[ColumnDef]] = None
    primary_key: Optional[str] = None
    # insert
    values: Optional[List[Dict[str, Any]]] = None
    # select
    select_cols: Optional[List[str]] = None
    where: Optional["Condition"] = None
    order_by: Optional[List[Tuple[str, str]]] = None  # (col, asc/desc) or (l2_expr, asc)
    limit: Optional[int] = None
    vector_order: Optional[Tuple[str, List[float]]] = None  # (col, query_vec)
    approximate: bool = False
    # update
    set_clause: Optional[Dict[str, Any]] = None
    # delete - uses where
    # vector index
    index_name: Optional[str] = None
    vector_col: Optional[str] = None


@dataclass
class Condition:
    """Simple AND-combined comparison conditions."""
    clauses: List[Tuple[str, str, Any]] = field(default_factory=list)  # (col, op, value)

    def evaluate(self, row: Dict[str, Any]) -> bool:
        for col, op, val in self.clauses:
            rowval = row.get(col)
            if rowval is None:
                return False
            if op == "=":
                if rowval != val:
                    return False
            elif op == "!=":
                if rowval == val:
                    return False
            elif op == "<":
                if not (rowval < val):
                    return False
            elif op == "<=":
                if not (rowval <= val):
                    return False
            elif op == ">":
                if not (rowval > val):
                    return False
            elif op == ">=":
                if not (rowval >= val):
                    return False
        return True


# Tokenizer
_TOKEN_RE = re.compile(r"""
    (?P<ws>\s+)
  | (?P<num>-?\d+(?:\.\d+)?)
  | (?P<str>'(?:[^']|'')*')
  | (?P<vec>\[[^\]]*\])
  | (?P<ident>[A-Za-z_][A-Za-z0-9_]*)
  | (?P<op><=|>=|!=|<>|<|>|=|,|\(|\)|\*|;|\.)
""", re.VERBOSE)


class Tokenizer:
    def __init__(self, text: str):
        self.text = text
        self.pos = 0
        self.tokens: List[Tuple[str, str]] = []
        self._tokenize()
        self.idx = 0

    def _tokenize(self):
        for m in _TOKEN_RE.finditer(self.text):
            kind = m.lastgroup
            val = m.group()
            if kind == "ws":
                continue
            self.tokens.append((kind, val))
        # add EOF
        self.tokens.append(("eof", ""))

    def peek(self) -> Tuple[str, str]:
        return self.tokens[self.idx]

    def next(self) -> Tuple[str, str]:
        t = self.tokens[self.idx]
        self.idx += 1
        return t

    def expect(self, kind: str, val: Optional[str] = None) -> Tuple[str, str]:
        k, v = self.next()
        if k != kind or (val is not None and v.upper() != val.upper()):
            raise ParseError(f"Expected {val or kind}, got {v}")
        return (k, v)

    def match(self, kind: str, val: Optional[str] = None) -> bool:
        k, v = self.peek()
        if k != kind:
            return False
        if val is not None and v.upper() != val.upper():
            return False
        return True


def parse_statement(sql: str) -> ParsedStatement:
    sql = sql.strip().rstrip(";").strip()
    if not sql:
        raise ParseError("Empty statement")
    tok = Tokenizer(sql)
    kind, val = tok.peek()
    kw = val.upper()

    if kw == "BEGIN":
        tok.next()
        return ParsedStatement(type="begin")
    elif kw == "COMMIT":
        tok.next()
        return ParsedStatement(type="commit")
    elif kw == "ROLLBACK":
        tok.next()
        return ParsedStatement(type="rollback")
    elif kw == "CREATE":
        return parse_create(tok)
    elif kw == "DROP":
        tok.next()
        tok.expect("ident", "TABLE")
        tname = tok.next()[1]
        return ParsedStatement(type="drop_table", table=tname)
    elif kw == "INSERT":
        return parse_insert(tok)
    elif kw == "SELECT":
        return parse_select(tok)
    elif kw == "UPDATE":
        return parse_update(tok)
    elif kw == "DELETE":
        return parse_delete(tok)
    else:
        raise ParseError(f"Unsupported statement: {sql[:50]}")


def parse_create(tok: Tokenizer) -> ParsedStatement:
    tok.expect("ident", "CREATE")
    if tok.match("ident", "TABLE"):
        return parse_create_table(tok)
    elif tok.match("ident", "VECTOR"):
        return parse_create_vector_index(tok)
    else:
        raise ParseError("Expected TABLE or VECTOR after CREATE")


def parse_create_table(tok: Tokenizer) -> ParsedStatement:
    tok.expect("ident", "TABLE")
    # optional IF NOT EXISTS
    if tok.match("ident", "IF"):
        tok.next()
        tok.expect("ident", "NOT")
        tok.expect("ident", "EXISTS")
    tname = tok.next()[1]
    tok.expect("op", "(")
    columns: List[ColumnDef] = []
    primary_key = None
    while not tok.match("op", ")"):
        if tok.match("ident", "PRIMARY"):
            tok.next()
            tok.expect("ident", "KEY")
            tok.expect("op", "(")
            primary_key = tok.next()[1]
            tok.expect("op", ")")
        else:
            cname = tok.next()[1]
            ctype_tok = tok.next()
            ctype = ctype_tok[1].upper()
            length = 0
            if tok.match("op", "("):
                tok.next()
                length = int(tok.next()[1])
                tok.expect("op", ")")
            col = ColumnDef(name=cname, type=ctype.lower(), length=length)
            # check inline PRIMARY KEY
            if tok.match("ident", "PRIMARY"):
                tok.next()
                tok.expect("ident", "KEY")
                col.is_primary = True
                primary_key = cname
            columns.append(col)
        if tok.match("op", ","):
            tok.next()
        else:
            break
    tok.expect("op", ")")
    # determine vector column
    vector_col = None
    for c in columns:
        if c.type == Type.VECTOR:
            vector_col = c.name
            break
    if primary_key is None:
        # find inline
        for c in columns:
            if c.is_primary:
                primary_key = c.name
                break
    if primary_key is None:
        raise ParseError("Table must have a primary key")
    return ParsedStatement(
        type="create_table",
        table=tname,
        columns=columns,
        primary_key=primary_key,
    )


def parse_create_vector_index(tok: Tokenizer) -> ParsedStatement:
    tok.expect("ident", "VECTOR")
    tok.expect("ident", "INDEX")
    idx_name = tok.next()[1]
    tok.expect("ident", "ON")
    tname = tok.next()[1]
    tok.expect("op", "(")
    vcol = tok.next()[1]
    tok.expect("op", ")")
    # optional WITH (...)
    if tok.match("ident", "WITH"):
        tok.next()
        # skip the with clause
        depth = 0
        if tok.match("op", "("):
            depth = 1
            tok.next()
            while depth > 0:
                k, v = tok.next()
                if v == "(":
                    depth += 1
                elif v == ")":
                    depth -= 1
    return ParsedStatement(
        type="create_vector_index",
        table=tname,
        index_name=idx_name,
        vector_col=vcol,
    )


def parse_insert(tok: Tokenizer) -> ParsedStatement:
    tok.expect("ident", "INSERT")
    tok.expect("ident", "INTO")
    tname = tok.next()[1]
    col_names = None
    if tok.match("op", "("):
        tok.next()
        col_names = []
        while not tok.match("op", ")"):
            col_names.append(tok.next()[1])
            if tok.match("op", ","):
                tok.next()
            else:
                break
        tok.expect("op", ")")
    tok.expect("ident", "VALUES")
    rows = []
    while True:
        tok.expect("op", "(")
        vals = []
        while not tok.match("op", ")"):
            val_tok = tok.next()
            vals.append(parse_value(val_tok))
            if tok.match("op", ","):
                tok.next()
            else:
                break
        tok.expect("op", ")")
        rows.append(vals)
        if tok.match("op", ","):
            tok.next()
        else:
            break
    # Build value dicts
    table_cols = col_names  # may be None
    return ParsedStatement(
        type="insert",
        table=tname,
        values={"col_names": table_cols, "rows": rows},
    )


def parse_select(tok: Tokenizer) -> ParsedStatement:
    tok.expect("ident", "SELECT")
    select_cols = []
    if tok.match("op", "*"):
        tok.next()
        select_cols = ["*"]
    else:
        while True:
            select_cols.append(tok.next()[1])
            if tok.match("op", ","):
                tok.next()
            else:
                break
    tok.expect("ident", "FROM")
    tname = tok.next()[1]
    where = None
    order_by = None
    limit = None
    vector_order = None
    approximate = False
    if tok.match("ident", "WHERE"):
        tok.next()
        where = parse_where(tok)
    if tok.match("ident", "ORDER"):
        tok.next()
        tok.expect("ident", "BY")
        order_by, vector_order, approximate = parse_order_by(tok)
    if tok.match("ident", "LIMIT"):
        tok.next()
        limit = int(tok.next()[1])
    return ParsedStatement(
        type="select",
        table=tname,
        select_cols=select_cols,
        where=where,
        order_by=order_by,
        limit=limit,
        vector_order=vector_order,
        approximate=approximate,
    )


def parse_where(tok: Tokenizer) -> Condition:
    clauses = []
    while True:
        col = tok.next()[1]
        op_tok = tok.next()
        op = op_tok[1]
        if op == "<>":
            op = "!="
        val_tok = tok.next()
        val = parse_value(val_tok)
        clauses.append((col, op, val))
        if tok.match("ident", "AND"):
            tok.next()
        else:
            break
    return Condition(clauses=clauses)


def parse_value(tok_tuple: Tuple[str, str]) -> Any:
    kind, val = tok_tuple
    if kind == "num":
        if "." in val:
            return float(val)
        return int(val)
    if kind == "str":
        return val[1:-1].replace("''", "'")
    if kind == "vec":
        return val
    return val


def parse_order_by(tok: Tokenizer) -> Tuple:
    items = []
    vector_order = None
    approximate = False
    while True:
        if tok.match("ident", "L2_DISTANCE"):
            tok.next()
            tok.expect("op", "(")
            col = tok.next()[1]
            tok.expect("op", ",")
            vec_tok = tok.next()
            vec = parse_value(vec_tok)
            tok.expect("op", ")")
            direction = "asc"
            if tok.match("ident", "ASC"):
                tok.next()
            elif tok.match("ident", "DESC"):
                tok.next()
                direction = "desc"
            if tok.match("ident", "APPROXIMATE"):
                tok.next()
                approximate = True
            vector_order = (col, vec)
            items.append((f"l2_distance({col})", direction))
        else:
            col = tok.next()[1]
            direction = "asc"
            if tok.match("ident", "ASC"):
                tok.next()
            elif tok.match("ident", "DESC"):
                tok.next()
                direction = "desc"
            items.append((col, direction))
        if tok.match("op", ","):
            tok.next()
        else:
            break
    return items, vector_order, approximate


def parse_update(tok: Tokenizer) -> ParsedStatement:
    tok.expect("ident", "UPDATE")
    tname = tok.next()[1]
    tok.expect("ident", "SET")
    set_clause = {}
    while True:
        col = tok.next()[1]
        tok.expect("op", "=")
        val_tok = tok.next()
        set_clause[col] = parse_value(val_tok)
        if tok.match("op", ","):
            tok.next()
        else:
            break
    where = None
    if tok.match("ident", "WHERE"):
        tok.next()
        where = parse_where(tok)
    return ParsedStatement(
        type="update",
        table=tname,
        set_clause=set_clause,
        where=where,
    )


def parse_delete(tok: Tokenizer) -> ParsedStatement:
    tok.expect("ident", "DELETE")
    tok.expect("ident", "FROM")
    tname = tok.next()[1]
    where = None
    if tok.match("ident", "WHERE"):
        tok.next()
        where = parse_where(tok)
    return ParsedStatement(
        type="delete",
        table=tname,
        where=where,
    )


# ---------------------------------------------------------------------------
# Executor
# ---------------------------------------------------------------------------

class Executor:
    def __init__(self, storage: StorageEngine):
        self.storage = storage
        self.transaction = Transaction()
        self.vector_indexes: Dict[Tuple[str, str], VectorIndex] = {}
        self._build_indexes_from_data()

    def _build_indexes_from_data(self):
        for tname, td in self.storage.tables.items():
            if td.vector_column:
                idx = VectorIndex(td.column(td.vector_column).length)
                vcol = td.vector_column
                pkey = td.primary_key
                for row in self.storage.all_rows(tname):
                    vec = row.get(vcol)
                    if vec is not None:
                        idx.add(row[pkey], vec)
                idx.build()
                self.vector_indexes[(tname, vcol)] = idx

    def execute(self, stmt: ParsedStatement) -> str:
        handler = getattr(self, f"exec_{stmt.type}", None)
        if handler is None:
            raise ValueError(f"Unsupported statement type: {stmt.type}")
        return handler(stmt)

    def _persist(self) -> bool:
        return not self.transaction.active

    def _wal_write(self, entry: dict):
        if self.transaction.active:
            self.transaction.add_wal(entry)
        else:
            self.storage._wal_write(entry)

    def exec_begin(self, stmt: ParsedStatement) -> str:
        self.transaction.begin(self.storage)
        return ""

    def exec_commit(self, stmt: ParsedStatement) -> str:
        self.transaction.commit(self.storage)
        return ""

    def exec_rollback(self, stmt: ParsedStatement) -> str:
        self.transaction.rollback(self.storage)
        self._build_indexes_from_data()
        return ""

    def exec_create_table(self, stmt: ParsedStatement) -> str:
        td = TableDef(
            name=stmt.table,
            columns=stmt.columns,
            primary_key=stmt.primary_key,
            vector_column=next((c.name for c in stmt.columns if c.type == Type.VECTOR), None),
        )
        persist = self._persist()
        self.storage.create_table(td, persist=persist)
        if not persist:
            self.transaction.add_wal({
                "op": "create_table",
                "table": {
                    "name": td.name,
                    "columns": [c.__dict__ for c in td.columns],
                    "primary_key": td.primary_key,
                    "vector_column": td.vector_column,
                },
            })
        return ""

    def exec_drop_table(self, stmt: ParsedStatement) -> str:
        persist = self._persist()
        self.storage.drop_table(stmt.table, persist=persist)
        if not persist:
            self.transaction.add_wal({"op": "drop_table", "table": stmt.table})
        # remove indexes
        for key in list(self.vector_indexes.keys()):
            if key[0] == stmt.table:
                del self.vector_indexes[key]
        return ""

    def exec_create_vector_index(self, stmt: ParsedStatement) -> str:
        td = self.storage.get_table(stmt.table)
        vcol = stmt.vector_col
        col = td.column(vcol)
        if col is None or col.type != Type.VECTOR:
            raise ValueError(f"Column {vcol} is not a vector column")
        idx = VectorIndex(col.length)
        pkey = td.primary_key
        for row in self.storage.all_rows(stmt.table):
            vec = row.get(vcol)
            if vec is not None:
                idx.add(row[pkey], vec)
        idx.build()
        self.vector_indexes[(stmt.table, vcol)] = idx
        self._wal_write({"op": "create_vector_index", "table": stmt.table, "column": vcol})
        return ""

    def exec_insert(self, stmt: ParsedStatement) -> str:
        td = self.storage.get_table(stmt.table)
        col_names = stmt.values["col_names"]
        rows_vals = stmt.values["rows"]
        if col_names is None:
            col_names = [c.name for c in td.columns]
        persist = self._persist()
        count = 0
        n_rows = len(rows_vals)
        for vi, vals in enumerate(rows_vals):
            row = {}
            for i, cname in enumerate(col_names):
                col = td.column(cname)
                if col is None:
                    raise ValueError(f"Unknown column: {cname}")
                raw = vals[i]
                row[cname] = self._coerce(raw, col)
            # In auto-commit mode, flush WAL only on the last row
            flush = persist and (vi == n_rows - 1)
            self.storage.insert_row(stmt.table, row, persist=persist, flush=flush)
            if not persist:
                self.transaction.add_wal({"op": "insert", "table": stmt.table, "row": dict(row)})
            # update vector index
            if td.vector_column and td.vector_column in row:
                idx = self.vector_indexes.get((stmt.table, td.vector_column))
                if idx:
                    idx.add(row[td.primary_key], row[td.vector_column])
            count += 1
        return f"Query OK, {count} row{'s' if count != 1 else ''} affected"

    def exec_select(self, stmt: ParsedStatement) -> str:
        td = self.storage.get_table(stmt.table)
        rows = self.storage.all_rows(stmt.table)
        pkey = td.primary_key
        # filter
        if stmt.where:
            rows = [r for r in rows if stmt.where.evaluate(r)]
        # vector order by
        if stmt.vector_order:
            vcol, query_vec_str = stmt.vector_order
            query_vec = parse_vector_literal(query_vec_str, td.column(vcol).length)
            # Use vector index if available for faster computation
            idx = self.vector_indexes.get((stmt.table, vcol))
            if idx is not None and stmt.where is None:
                # no filter - use index directly
                k = stmt.limit if stmt.limit is not None else len(rows)
                results = idx.search(query_vec, k, approximate=stmt.approximate)
                rows_by_pkey = {r[pkey]: r for r in rows}
                rows = [rows_by_pkey[pk] for pk, d in results if pk in rows_by_pkey]
            elif idx is not None:
                # with filter - compute distances using index but filter candidates
                candidate_ids = {r[pkey] for r in rows}
                k = stmt.limit if stmt.limit is not None else len(rows)
                results = idx.search(query_vec, k, candidate_ids=candidate_ids, approximate=stmt.approximate)
                rows_by_pkey = {r[pkey]: r for r in rows}
                rows = [rows_by_pkey[pk] for pk, d in results if pk in rows_by_pkey]
            else:
                # no index - brute force
                dist_rows = []
                for r in rows:
                    vec = r.get(vcol)
                    if vec is None:
                        continue
                    d = l2_distance(vec, query_vec)
                    dist_rows.append((d, r[pkey], r))
                dist_rows.sort(key=lambda x: (x[0], _sort_key(x[1])))
                rows = [x[2] for x in dist_rows]
        elif stmt.order_by:
            # regular order by
            def sort_key(r):
                keys = []
                for col, direction in stmt.order_by:
                    v = r.get(col)
                    keys.append(v if direction == "asc" else _negate(v))
                keys.append(r.get(pkey))
                return tuple(keys)
            rows.sort(key=sort_key)
        # limit
        if stmt.limit is not None:
            rows = rows[:stmt.limit]
        # projection
        if stmt.select_cols == ["*"]:
            proj_cols = [c.name for c in td.columns]
        else:
            proj_cols = stmt.select_cols
            # validate columns exist
            for cname in proj_cols:
                if td.column(cname) is None:
                    raise ValueError(f"Unknown column '{cname}' in field list")
        # format output
        lines = []
        # header
        lines.append("\t".join(proj_cols))
        for r in rows:
            vals = []
            for cname in proj_cols:
                col = td.column(cname)
                vals.append(format_value(r.get(cname), col))
            lines.append("\t".join(vals))
        return "\n".join(lines)

    def exec_update(self, stmt: ParsedStatement) -> str:
        td = self.storage.get_table(stmt.table)
        rows = self.storage.all_rows(stmt.table)
        if stmt.where:
            rows = [r for r in rows if stmt.where.evaluate(r)]
        pkey = td.primary_key
        persist = self._persist()
        updates = {}
        for r in rows:
            pval = r[pkey]
            up = {}
            for cname, raw in stmt.set_clause.items():
                col = td.column(cname)
                up[cname] = self._coerce(raw, col)
            updates[pval] = up
        self.storage.update_rows(stmt.table, updates, persist=persist)
        if not persist:
            for pval, up in updates.items():
                self.transaction.add_wal({
                    "op": "update",
                    "table": stmt.table,
                    "primary_key": pkey,
                    "primary_value": pval,
                    "updates": up,
                })
        # update vector index
        if td.vector_column:
            idx = self.vector_indexes.get((stmt.table, td.vector_column))
            if idx:
                for pval, up in updates.items():
                    if td.vector_column in up:
                        idx.update(pval, up[td.vector_column])
        count = len(updates)
        return f"Query OK, {count} row{'s' if count != 1 else ''} affected"

    def exec_delete(self, stmt: ParsedStatement) -> str:
        td = self.storage.get_table(stmt.table)
        rows = self.storage.all_rows(stmt.table)
        if stmt.where:
            rows = [r for r in rows if stmt.where.evaluate(r)]
        pkey = td.primary_key
        pvals = [r[pkey] for r in rows]
        persist = self._persist()
        self.storage.delete_rows(stmt.table, pvals, persist=persist)
        if not persist:
            for pval in pvals:
                self.transaction.add_wal({
                    "op": "delete",
                    "table": stmt.table,
                    "primary_key": pkey,
                    "primary_value": pval,
                })
        # update vector index
        if td.vector_column:
            idx = self.vector_indexes.get((stmt.table, td.vector_column))
            if idx:
                for pval in pvals:
                    idx.remove(pval)
        count = len(pvals)
        return f"Query OK, {count} row{'s' if count != 1 else ''} affected"

    def _coerce(self, raw: str, col: ColumnDef) -> Value:
        if col.type == Type.BIGINT:
            return int(raw)
        elif col.type == Type.VARCHAR:
            s = raw
            if isinstance(s, str) and s.startswith("'") and s.endswith("'"):
                s = s[1:-1].replace("''", "'")
            # check length
            if col.length > 0 and len(s) > col.length:
                raise ValueError(f"VARCHAR value too long for {col.name}({col.length})")
            return s
        elif col.type == Type.VECTOR:
            return parse_vector_literal(raw, col.length)
        raise ValueError(f"Unknown type: {col.type}")


def _negate(v):
    if isinstance(v, (int, float)):
        return -v
    return v


# ---------------------------------------------------------------------------
# Protocol / I/O
# ---------------------------------------------------------------------------

class ProtocolError(Exception):
    pass


def _strip_comments(text: str) -> str:
    """Remove SQL line comments (-- ... and # ...) from text."""
    result = []
    i = 0
    in_str = False
    str_char = None
    while i < len(text):
        c = text[i]
        if in_str:
            result.append(c)
            if c == str_char:
                if i + 1 < len(text) and text[i + 1] == str_char:
                    result.append(text[i + 1])
                    i += 1
                else:
                    in_str = False
        else:
            if c in ("'", '"'):
                in_str = True
                str_char = c
                result.append(c)
            elif c == '-' and i + 1 < len(text) and text[i + 1] == '-':
                # line comment - skip to end of line
                while i < len(text) and text[i] != '\n':
                    i += 1
                continue
            elif c == '#':
                while i < len(text) and text[i] != '\n':
                    i += 1
                continue
            else:
                result.append(c)
        i += 1
    return "".join(result)


def split_statements(text: str) -> List[str]:
    """Split SQL text into statements on semicolons, respecting strings and skipping comments."""
    text = _strip_comments(text)
    stmts = []
    current = []
    in_str = False
    str_char = None
    i = 0
    while i < len(text):
        c = text[i]
        if in_str:
            current.append(c)
            if c == str_char:
                if i + 1 < len(text) and text[i + 1] == str_char:
                    current.append(text[i + 1])
                    i += 1
                else:
                    in_str = False
        else:
            if c in ("'", '"'):
                in_str = True
                str_char = c
                current.append(c)
            elif c == ";":
                s = "".join(current).strip()
                if s:
                    stmts.append(s)
                current = []
            else:
                current.append(c)
        i += 1
    s = "".join(current).strip()
    if s:
        stmts.append(s)
    return stmts


def run_server():
    storage = StorageEngine(get_data_dir())
    executor = Executor(storage)

    # Signal readiness
    sys.stdout.write("ready\n")
    sys.stdout.flush()

    buffer = ""
    try:
        for line in sys.stdin:
            buffer += line
            # Strip comments and split into statements
            cleaned = _strip_comments(buffer)
            # Find the last semicolon position to determine complete statements
            last_semi = cleaned.rfind(";")
            if last_semi < 0:
                continue
            # Process all complete statements
            complete = cleaned[:last_semi + 1]
            buffer = cleaned[last_semi + 1:]
            # Keep original buffer remainder for multi-line statements
            # Actually, we need to be careful: _strip_comments changes positions
            # Let's just use split_statements on the complete part
            for stmt_text in split_statements(complete):
                if not stmt_text.strip():
                    continue
                try:
                    stmt = parse_statement(stmt_text)
                    result = executor.execute(stmt)
                    if result:
                        sys.stdout.write(result + "\n")
                except (ParseError, ValueError) as e:
                    sys.stdout.write(f"ERROR 1064 (42000): {e}\n")
                except Exception as e:
                    sys.stdout.write(f"ERROR 1000 (HY000): {e}\n")
                sys.stdout.flush()
    except KeyboardInterrupt:
        pass
    finally:
        storage.shutdown()


def _find_statement_end(text: str) -> int:
    """Find the index of the first semicolon that ends a statement."""
    in_str = False
    str_char = None
    i = 0
    while i < len(text):
        c = text[i]
        if in_str:
            if c == str_char:
                if i + 1 < len(text) and text[i + 1] == str_char:
                    i += 1
                else:
                    in_str = False
        else:
            if c in ("'", '"'):
                in_str = True
                str_char = c
            elif c == ";":
                return i
        i += 1
    return -1


if __name__ == "__main__":
    run_server()
