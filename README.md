# MiniVectorDB

A lightweight vector database supporting SQL, persistence, transactions, crash recovery, and vector similarity search.

## Build and Run

```bash
docker build -t minivectordb .
docker run -i --rm -v /data:/data minivectordb
```

Data is persisted to `/data`. The container speaks the official JSONL stdin/stdout
protocol: it emits `{"type":"ready"}` after recovery, handles one JSON request per
line, returns one JSON response per request, and accepts `execute` and `shutdown`
operations. Diagnostics go to stderr. See the official
[`STDIO_PROTOCOL.md`](https://github.com/oceanbase/seekdb/blob/2026-competition-round2/STDIO_PROTOCOL.md)
for the wire format.

## Public sample verification

The official [round2 public sample pack](https://github.com/oceanbase/seekdb/tree/2026-competition-round2/mysqltest)
contains 20 cases and 95 SQL assertions. Run the engine-level checks with:

```bash
python3 -m unittest tests.test_official_public -v
```

This runner checks SQL behavior directly. To exercise the published JSONL
protocol and all public cases through a running process, use the official
`protocol_check/run.py` from an `oceanbase/seekdb` checkout:

```bash
python3 protocol_check/run.py \
  --command '["env","MINIVECTORDB_DATA_DIR={data_dir}","python3","/absolute/path/to/this/repo/minivectordb.py"]' \
  --output "protocol-results/public-$(date +%Y%m%d-%H%M%S)"
```

The `--command` path exercises the process protocol without Docker. Before
submission, run the same checker with `--image` against the built Docker image;
that also verifies the Docker entry point and container environment. The public
pack does not contain the hidden formal cases.
