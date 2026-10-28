#!/usr/bin/env python3
"""Deterministic HTTP mock for the ai_function mysqltest suite.

The suite talks to five endpoints on a single port (8000 by default):

  POST /compatible-mode/v1/chat/completions   OpenAI compatible completion
  POST /compatible-mode/v1/embeddings         OpenAI compatible embedding
  POST /v1/rerank                             SiliconFlow compatible rerank
  POST /api/generate                          Ollama native completion
  POST /api/embed                             Ollama native embedding

Usage:
  python3 ai_function_mock.py start 8000
  python3 ai_function_mock.py assert 8000 embedding-batch
  python3 ai_function_mock.py stop 8000

Assertions read the request log written by the server and fail with a
non-zero exit code when the observed traffic does not match.
"""

import json
import os
import signal
import socket
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

DEFAULT_DIMENSION = 768
SHUFFLE_ENV = "AI_FUNCTION_MOCK_SHUFFLE_EMBEDDINGS"


def state_dir(port):
    return "/tmp/seekdb_ai_function_mock_%d" % port


def state_path(port, name):
    return os.path.join(state_dir(port), name)


def read_events(port):
    events = []
    path = state_path(port, "events.jsonl")
    if os.path.exists(path):
        with open(path, "r", encoding="utf-8") as stream:
            for line in stream:
                line = line.strip()
                if line:
                    events.append(json.loads(line))
    return events


def stop_server(port):
    pid_path = state_path(port, "server.pid")
    if not os.path.exists(pid_path):
        return
    try:
        with open(pid_path, "r", encoding="ascii") as stream:
            pid = int(stream.read().strip())
        try:
            os.kill(pid, signal.SIGTERM)
        except OSError:
            pass
        for _ in range(50):
            try:
                os.kill(pid, 0)
            except OSError:
                break
            time.sleep(0.02)
    except (ValueError, ProcessLookupError):
        pass
    try:
        os.unlink(pid_path)
    except FileNotFoundError:
        pass


def start_server(port):
    os.makedirs(state_dir(port), exist_ok=True)
    stop_server(port)
    for name in ("events.jsonl", "server.out"):
        with open(state_path(port, name), "w", encoding="utf-8"):
            pass
    output = open(state_path(port, "server.out"), "a", encoding="utf-8")
    process = subprocess.Popen(
        [sys.executable, os.path.abspath(__file__), "serve", str(port)],
        stdin=subprocess.DEVNULL,
        stdout=output,
        stderr=subprocess.STDOUT,
        start_new_session=True,
    )
    with open(state_path(port, "server.pid"), "w", encoding="ascii") as stream:
        stream.write(str(process.pid))
    for _ in range(100):
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.05):
                return
        except OSError:
            if process.poll() is not None:
                break
            time.sleep(0.02)
    raise RuntimeError("ai_function mock did not start on port %d" % port)


def message_text(body):
    system_text = ""
    user_text = ""
    for message in body.get("messages", []):
        role = message.get("role")
        content = message.get("content", "")
        if not isinstance(content, str):
            content = json.dumps(content, ensure_ascii=False)
        if role == "system":
            system_text = content
        elif role == "user":
            user_text = content
    return system_text, user_text


def embedding_vector(text, position, dimension):
    """Deterministic, position sensitive vector so reordering is detectable."""
    seed = sum(ord(ch) for ch in str(text)) + position
    vector = [0.0] * dimension
    vector[0] = float(seed % 97 + 1)
    vector[1] = float(position + 1)
    vector[2] = 0.5
    return vector


def rerank_text(document):
    if isinstance(document, str):
        return document
    return json.dumps(document, ensure_ascii=False, sort_keys=True)


def build_rerank_results(query, documents):
    results = []
    for index, document in enumerate(documents):
        text = rerank_text(document)
        matched = bool(query) and query.lower() in text.lower()
        results.append(
            {
                "index": index,
                "document": document,
                "relevance_score": 0.9 if matched else 0.1,
            }
        )
    # Stable best-first order: matching documents first, then original order.
    results.sort(key=lambda item: (-item["relevance_score"], item["index"]))
    return results


def serve(port):
    lock = threading.Lock()
    log_path = state_path(port, "events.jsonl")
    shuffle = os.environ.get(SHUFFLE_ENV, "") not in ("", "0", "false")

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):
            return

        def reply(self, status, response, event):
            payload = json.dumps(response, ensure_ascii=False).encode("utf-8")
            event["status"] = status
            with lock:
                with open(log_path, "a", encoding="utf-8") as stream:
                    stream.write(json.dumps(event, ensure_ascii=False) + "\n")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

        def fail(self, message):
            self.reply(400, {"error": {"message": message}}, {})

        def read_body(self):
            size = int(self.headers.get("Content-Length", "0"))
            raw = self.rfile.read(size)
            try:
                body = json.loads(raw.decode("utf-8"))
            except Exception:
                body = {}
            if not isinstance(body, dict):
                body = {}
            return body

        def do_POST(self):
            body = self.read_body()
            parsed = urlparse(self.path).path
            event = {
                "path": parsed,
                "authorization": self.headers.get("Authorization", ""),
                "content_type": self.headers.get("Content-Type", ""),
            }

            if parsed.endswith("/api/generate"):
                self.handle_generate(body, event)
            elif parsed.endswith("/api/embed"):
                self.handle_ollama_embed(body, event)
            elif parsed.endswith("/chat/completions"):
                self.handle_chat(body, event)
            elif parsed.endswith("/embeddings"):
                self.handle_embedding(body, event)
            elif parsed.endswith("/rerank"):
                self.handle_rerank(body, event)
            else:
                self.fail("unsupported path " + parsed)

        def handle_chat(self, body, event):
            system_text, user_text = message_text(body)
            event.update(
                {
                    "system": system_text,
                    "user": user_text,
                    "max_tokens": body.get("max_tokens"),
                    "temperature": body.get("temperature"),
                }
            )
            if "always-fail" in user_text:
                self.reply(400, {"error": {"message": "injected failure"}}, event)
                return
            content = "mock:" + user_text if user_text else "mock"
            response = {
                "id": "mock-chat",
                "object": "chat.completion",
                "model": body.get("model", "mock"),
                "choices": [
                    {
                        "index": 0,
                        "message": {"role": "assistant", "content": content},
                        "finish_reason": "stop",
                    }
                ],
                "usage": {"prompt_tokens": 1, "completion_tokens": 1, "total_tokens": 2},
            }
            self.reply(200, response, event)

        def handle_embedding(self, body, event):
            raw_input = body.get("input", [])
            inputs = raw_input if isinstance(raw_input, list) else [raw_input]
            texts = ["" if value is None else str(value) for value in inputs]
            dimension = body.get("dimensions")
            if not isinstance(dimension, int) or dimension <= 0:
                dimension = DEFAULT_DIMENSION
            event.update(
                {
                    "input_count": len(texts),
                    "inputs": texts,
                    "dimension": dimension,
                }
            )
            if any("always-fail" in text for text in texts):
                self.reply(400, {"error": {"message": "injected failure"}}, event)
                return
            data = [
                {
                    "object": "embedding",
                    "index": index,
                    "embedding": embedding_vector(text, index, dimension),
                }
                for index, text in enumerate(texts)
            ]
            if shuffle:
                data.reverse()
            response = {
                "object": "list",
                "model": body.get("model", "mock-embedding"),
                "data": data,
                "usage": {"prompt_tokens": 1, "total_tokens": 1},
            }
            self.reply(200, response, event)

        def handle_rerank(self, body, event):
            query = str(body.get("query", ""))
            documents = body.get("documents", [])
            if not isinstance(documents, list):
                documents = []
            event.update(
                {
                    "query": query,
                    "document_count": len(documents),
                    "documents": [rerank_text(document) for document in documents],
                }
            )
            if any("always-fail" in rerank_text(doc) for doc in documents):
                self.reply(400, {"error": {"message": "injected failure"}}, event)
                return
            response = {
                "id": "mock-rerank",
                "model": body.get("model", "mock-rerank"),
                "results": build_rerank_results(query, documents),
                "meta": {"tokens": {"input_tokens": 1, "output_tokens": 1}},
            }
            self.reply(200, response, event)

        def handle_generate(self, body, event):
            prompt = str(body.get("prompt", ""))
            event.update(
                {
                    "prompt": prompt,
                    "stream": body.get("stream"),
                    "model": body.get("model", ""),
                }
            )
            if "always-fail" in prompt:
                self.reply(400, {"error": "injected failure"}, event)
                return
            response = {
                "model": body.get("model", "mock"),
                "created_at": "2026-01-01T00:00:00Z",
                "response": "mock:" + prompt if prompt else "mock",
                "done": True,
                "done_reason": "stop",
            }
            self.reply(200, response, event)

        def handle_ollama_embed(self, body, event):
            raw_input = body.get("input", [])
            inputs = raw_input if isinstance(raw_input, list) else [raw_input]
            texts = ["" if value is None else str(value) for value in inputs]
            dimension = body.get("dimensions")
            if not isinstance(dimension, int) or dimension <= 0:
                dimension = DEFAULT_DIMENSION
            event.update(
                {
                    "input_count": len(texts),
                    "inputs": texts,
                    "dimension": dimension,
                    "model": body.get("model", ""),
                }
            )
            if any("always-fail" in text for text in texts):
                self.reply(400, {"error": "injected failure"}, event)
                return
            response = {
                "model": body.get("model", "mock"),
                "embeddings": [
                    embedding_vector(text, index, dimension)
                    for index, text in enumerate(texts)
                ],
            }
            self.reply(200, response, event)

    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()


def assert_events(port, assertion):
    events = read_events(port)
    chat_events = [event for event in events if event["path"].endswith("/chat/completions")]
    embedding_events = [event for event in events if event["path"].endswith("/embeddings")]
    rerank_events = [event for event in events if event["path"].endswith("/rerank")]
    generate_events = [event for event in events if event["path"].endswith("/api/generate")]
    ollama_embed_events = [event for event in events if event["path"].endswith("/api/embed")]

    if assertion == "no-requests":
        ok = not events
    elif assertion == "complete":
        ok = any(event["status"] == 200 for event in chat_events)
    elif assertion == "complete-config":
        ok = any(
            event["status"] == 200 and event.get("max_tokens") == 16
            for event in chat_events
        )
    elif assertion == "embedding-768":
        ok = bool(embedding_events) and all(
            event["status"] == 200 and event["dimension"] == DEFAULT_DIMENSION
            for event in embedding_events
        )
    elif assertion == "embedding-batch":
        # Cross-row batching: several rows must share one request, the NULL row
        # must never be sent, and every non-null row must be present.
        inputs = [text for event in embedding_events for text in event["inputs"]]
        shared_request = any(event["input_count"] >= 2 for event in embedding_events)
        ok = (
            shared_request
            and "first" in inputs
            and "third" in inputs
            and "" not in inputs
            and "None" not in inputs
            and all(event["status"] == 200 for event in embedding_events)
        )
    elif assertion == "rerank":
        string_ok = any(
            event["status"] == 200 and event["document_count"] == 4
            for event in rerank_events
        )
        scored_ok = any(
            event["status"] == 200
            and event["query"] == "apple"
            and event["document_count"] == 2
            and "apple" in " ".join(event["documents"])
            for event in rerank_events
        )
        ok = string_ok and scored_ok
    elif assertion == "ollama":
        generate_ok = any(
            event["status"] == 200 and event.get("stream") is False
            for event in generate_events
        )
        embed_ok = any(event["status"] == 200 for event in ollama_embed_events)
        ok = generate_ok and embed_ok
    elif assertion == "empty-auth-ollama":
        ok = any(event["authorization"] == "" for event in generate_events) and any(
            event["authorization"] == "" for event in ollama_embed_events
        )
    else:
        raise ValueError("unknown assertion: " + assertion)

    if not ok:
        raise AssertionError("assertion %s failed: %r" % (assertion, events))
    print("mock_%s=ok" % assertion.replace("-", "_"))


def main():
    if len(sys.argv) < 3:
        raise SystemExit("usage: ai_function_mock.py MODE PORT [ASSERTION]")
    mode = sys.argv[1]
    port = int(sys.argv[2])
    if mode == "start":
        start_server(port)
    elif mode == "stop":
        stop_server(port)
    elif mode == "serve":
        serve(port)
    elif mode == "reset":
        with open(state_path(port, "events.jsonl"), "w", encoding="utf-8"):
            pass
    elif mode == "assert":
        assert_events(port, sys.argv[3])
    else:
        raise SystemExit("unknown mode: " + mode)


if __name__ == "__main__":
    main()
