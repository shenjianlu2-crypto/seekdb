# ai_function mysqltest suite

This suite covers `AI_PROMPT`, `AI_COMPLETE`, `AI_EMBED` (single row and
cross-row batch), `AI_RERANK` and the Ollama provider. Every AI case points at
`http://0.0.0.0:8000/...`, so the tests need a local HTTP server that speaks
the OpenAI compatible, SiliconFlow and Ollama protocols. `ai_function_mock.py`
is that server: deterministic responses, no external network, request log for
verification.

## Cases

| case | content |
| --- | --- |
| `ai_prompt.test` | template rendering, JSON arguments, LOB inputs, type errors |
| `ai_function_enhancement.test` | prompt rendering, completion, embedding (single + batch), rerank, Ollama |
| `ai_model_ddl.test` / `ai_model_endpoint_ddl.test` / `ai_endpoint_sql_name.test` | AI model and endpoint DDL |
| `ai_model_lower_case_name_mysql*.test` | lower case model naming |

## Mock server

```bash
cd tools/deploy/mysql_test/test_suite/ai_function

python3 ai_function_mock.py start 8000     # start, truncate the request log
python3 ai_function_mock.py assert 8000 embedding-batch
python3 ai_function_mock.py stop 8000
```

Port 8000 is hardcoded in the test SQL. The server state (request log, pid,
stdout) lives in `/tmp/seekdb_ai_function_mock_8000/`.

Endpoints:

| path | protocol | response |
| --- | --- | --- |
| `/compatible-mode/v1/chat/completions` | OpenAI completion | `choices[0].message.content = "mock:" + user text` |
| `/compatible-mode/v1/embeddings` | OpenAI embedding | `data[i] = {index, embedding}`, 768 dimensions by default, honours `dimensions` |
| `/v1/rerank` | SiliconFlow rerank | `results[] = {index, document, relevance_score}`, documents matching the query score 0.9 |
| `/api/generate` | Ollama completion | `response = "mock:" + prompt`, `done = true` |
| `/api/embed` | Ollama embedding | `embeddings[i]`, 768 dimensions |

Embeddings are position sensitive, so misaligned results are detected. Set
`AI_FUNCTION_MOCK_SHUFFLE_EMBEDDINGS=1` when starting the server to return
`data` out of order and exercise the `index` based reordering in
`ObOpenAIUtils::ObOpenAIEmbed::parse_output`.

Sentinel text `always-fail` in a prompt, input or document makes the endpoint
return HTTP 400 with an error body, which is useful for manual robustness
checks (the SQL statement must raise an error without crashing the server).

### Assertions

`assert` reads `events.jsonl` and exits non-zero on mismatch:

| name | checks |
| --- | --- |
| `no-requests` | the log is empty |
| `complete` | a completion request succeeded |
| `complete-config` | a completion request carried `max_tokens = 16` from the SQL config |
| `embedding-768` | every embedding request used 768 dimensions |
| `embedding-batch` | at least one request carried several rows, `first` and `third` are present, the NULL row was never sent |
| `rerank` | both rerank statements were served (4 documents, then 2 documents for query `apple`) |
| `ollama` | `/api/generate` got `stream: false` and `/api/embed` succeeded |
| `empty-auth-ollama` | Ollama requests were sent without an `Authorization` header |

## Build and run

```bash
# Linux only; bazel toolchain comes with the repository
./build.py init
./build.py build seekdb

# terminal 1
cd tools/deploy/mysql_test/test_suite/ai_function
python3 ai_function_mock.py start 8000

# terminal 2
python3 tools/deploy/mysqltest.py run \
  --suite ai_function \
  --base-dir /tmp/seekdb_ai_function_base \
  --work-dir /tmp/seekdb_ai_function_work

# verify the observed traffic
python3 tools/deploy/mysql_test/test_suite/ai_function/ai_function_mock.py \
  assert 8000 embedding-batch
```

`mysqltest.py run` starts a throwaway seekdb instance, runs every
`ai_function.*` case and compares against `r/mysql/*.result`. Add
`--test-set ai_function.ai_function_enhancement` to narrow the run, and
`--record` to refresh the expected result file after an intentional change.
