# Local reproduction of the fulltext index benchmark

The scripts in this directory reproduce the performance measurement of the
"赛题四：全文索引调优" competition locally.  They are **not** the official
benchmark: the official one lives in `tools/benchmark/fts_large_bench*` and
is driven by the VLDB CI.  This directory exists so that the numbers in
`OPTIMIZATION_REPORT.md` can be reproduced and so that a change can be
validated before it is pushed.

## Prerequisites

* a built `seekdb` binary (`build.py build ... seekdb`)
* Python 3 with `pymysql`
* a free TCP port (2881 by default)

## Usage

```bash
# 1. build (stops a running instance first, the linker needs the exe)
bash fts_benchmark/build_seekdb.sh

# 2. start a clean instance (fresh data directory)
bash fts_benchmark/start_seekdb.sh
python fts_benchmark/wait_ready.py

# 3. generate the corpus (20,000 mixed Chinese/English documents)
python fts_benchmark/gen_data.py
python fts_benchmark/extract_texts.py     # verified tokenize inputs

# 4. run the benchmark
python fts_benchmark/bench.py --out reports/baseline.json --label baseline

# 5. score a candidate against a baseline
python fts_benchmark/score.py --baseline reports/baseline.json \
                              --candidate reports/candidate.json

# 6. prove that the tokenizer output and the MATCH results did not change
python fts_benchmark/verify_tokenize.py reports/verify.txt
diff reports/verify_baseline.txt reports/verify.txt
```

## Helper scripts

| script | purpose |
|---|---|
| `ablation_build.py` | build time per parser, fixed vs. per row cost |
| `sweep_parallelism.py` | build time as a function of the DDL parallelism |
| `measure_overhead.py` | fixed per call overhead of `TOKENIZE()` vs. per character cost |
| `inspect_bytes.py` | scratch tool used to find the invalid UTF-8 tokenizer output |
| `prepare_db.py` | create and fill the benchmark database without measuring |

## Note on the tokenize inputs

`extract_texts.py` verifies every candidate text against the running server
and only keeps the accepted ones.  The IK tokenizer can emit a token whose
byte range splits a multi-byte character; the server then rejects the
result with `ER_WRONG_ARGUMENTS` and the error payload desynchronises the
MySQL protocol.  That defect exists in the unmodified kernel as well and was
not changed by this optimization, so the benchmark simply avoids the inputs
that trigger it.
