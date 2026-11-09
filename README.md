# MiniVectorDB

A lightweight vector database supporting SQL, persistence, transactions, crash recovery, and vector similarity search.

## Build and Run

```bash
docker build -t minivectordb .
docker run -i --rm -v /data:/data minivectordb
```

Data is persisted to `/data`. The program communicates via stdin/stdout in mysqltest style.
