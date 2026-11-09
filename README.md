# MiniVectorDB

A lightweight vector database supporting SQL, persistence, transactions, crash recovery, and vector similarity search.

## Build and Run

```bash
docker build -t minivectordb .
docker run -i --rm -v /data:/data minivectordb
```

Data is persisted to `/data`. The program communicates via stdin/stdout in mysqltest style.

## Public sample verification

The official [round2 public sample pack](https://github.com/oceanbase/seekdb/tree/2026-competition-round2/mysqltest)
contains 20 cases and 95 SQL assertions. Its README says the sample pack does
not include the organization's complete process communication driver. Run the
published cases against the SQL engine with:

```bash
python3 -m unittest tests.test_official_public -v
```

This runner checks statement results, expected errors, restart persistence,
and the rows asserted by the small ANN sample. It invokes the engine directly;
the `--mode ann` directive is not sent through an official driver, so this does
not validate mode switching, stdin/stdout framing, or hidden formal cases. The
Task 1 document still leaves statement boundaries, output framing, error-code
mapping, readiness, search-mode switching, and shutdown to the complete
package/driver.
