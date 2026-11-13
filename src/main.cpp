#include "engine.h"
#include "json.h"
#include "sql.h"
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

using namespace mvdb;

namespace {

void write_line(const std::string& s) {
  fwrite(s.data(), 1, s.size(), stdout);
  fputc('\n', stdout);
  fflush(stdout);
}

void append_json_double(std::string& out, double d) {
  if (!std::isfinite(d) || d == 0.0) {
    out += "0.0";
    return;
  }
  char buf[64];
  auto result = std::to_chars(buf, buf + sizeof buf, d, std::chars_format::general);
  if (result.ec != std::errc()) {
    out += "0.0";
    return;
  }
  const size_t len = (size_t)(result.ptr - buf);
  out.append(buf, len);
  if (std::memchr(buf, '.', len) == nullptr && std::memchr(buf, 'e', len) == nullptr &&
      std::memchr(buf, 'E', len) == nullptr)
    out += ".0";
}

void append_cell_json(std::string& out, const Cell& c) {
  switch (c.kind) {
    case Cell::INT: {
      char buf[32];
      auto result = std::to_chars(buf, buf + sizeof buf, c.i);
      if (result.ec == std::errc()) out.append(buf, (size_t)(result.ptr - buf));
      else out += std::to_string(c.i);
      return;
    }
    case Cell::FLOAT: append_json_double(out, c.d); return;
    case Cell::STR:
      out.push_back('"');
      out += json_escape(c.s);
      out.push_back('"');
      return;
    case Cell::ARR: {
      out.push_back('[');
      for (size_t i = 0; i < c.arr.size(); i++) {
        if (i) out.push_back(',');
        append_json_double(out, c.arr[i]);
      }
      out.push_back(']');
      return;
    }
    default: out += "null"; return;
  }
}

std::string ok_prefix(const std::string& id) { return "{\"id\":" + id + ",\"ok\":true"; }

std::string success_json(const std::string& id, const Result& r) {
  if (r.shape == Result::QUERY) {
    std::string s;
    s.reserve(48 + id.size() + r.col_names.size() * 24 + r.rows.size() * 64);
    s += ok_prefix(id);
    s += ",\"columns\":[";
    for (size_t i = 0; i < r.col_names.size(); i++) {
      if (i) s += ",";
      s += "\"" + json_escape(r.col_names[i]) + "\"";
    }
    s += "],\"rows\":[";
    for (size_t i = 0; i < r.rows.size(); i++) {
      if (i) s += ",";
      s += "[";
      for (size_t j = 0; j < r.rows[i].size(); j++) {
        if (j) s += ",";
        append_cell_json(s, r.rows[i][j]);
      }
      s += "]";
    }
    s += "]}";
    return s;
  }
  if (r.shape == Result::DML)
    return ok_prefix(id) + ",\"affected_rows\":" + std::to_string(r.affected) + "}";
  return ok_prefix(id) + "}";
}

std::string failure_json(const std::string& id, const std::string& code, const std::string& msg) {
  return "{\"id\":" + id + ",\"ok\":false,\"error\":{\"code\":\"" + json_escape(code) +
         "\",\"message\":\"" + json_escape(msg) + "\"}}";
}

std::string id_text(const Json& req) {
  const Json* j = req.find("id");
  if (!j) return "null";
  return json_id_text(*j);
}

}  // namespace

int main(int argc, char** argv) {
  std::string dir = "/data";
  std::string mode = "exact";
  size_t cache_mb = 32;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--data-dir") && i + 1 < argc) dir = argv[++i];
    else if (!strcmp(argv[i], "--mode") && i + 1 < argc) mode = argv[++i];
    else if (!strcmp(argv[i], "--cache-mb") && i + 1 < argc) cache_mb = (size_t)atoll(argv[++i]);
  }
  std::string line;
  Engine* eng = nullptr;
  try {
    eng = new Engine(dir, cache_mb * 1024 * 1024, mode);
    eng->open();
  } catch (const std::exception& e) {
    fprintf(stderr, "[mvdb] fatal during recovery: %s\n", e.what());
    fflush(stderr);
    delete eng;
    return 1;
  }
  // The judge waits for this before sending anything, so it must precede the
  // first read and go out unbuffered.
  write_line("{\"type\":\"ready\"}");

  bool shutdown_seen = false;
  while (std::getline(std::cin, line)) {
    if (line.empty()) continue;
    Json req;
    try {
      req = json_parse(line);
    } catch (const std::exception& e) {
      fprintf(stderr, "[mvdb] %s\n", e.what());
      fflush(stderr);
      write_line(failure_json("null", "internal_error", e.what()));
      continue;
    }
    std::string id = id_text(req);
    const Json* opj = req.find("op");
    std::string op = opj && opj->kind == Json::STR ? opj->s : "";
    try {
      if (op == "shutdown") {
        eng->close();
        write_line(success_json(id, Result{}));
        shutdown_seen = true;
        break;
      }
      if (op != "execute") throw MVError("unsupported_op", "unsupported operation: " + op);
      const Json* sqlj = req.find("sql");
      if (!sqlj || sqlj->kind != Json::STR) throw MVError(ER_SYNTAX, "missing sql field");
      const Json* moj = req.find("search_mode");
      std::string m = moj && moj->kind == Json::STR ? moj->s : "exact";
      eng->set_mode(m.empty() ? "exact" : m);
      Stmt st = parse_sql(sqlj->s);
      Result r = eng->execute(st);
      write_line(success_json(id, r));
    } catch (const MVError& e) {
      write_line(failure_json(id, e.code, e.what()));
    } catch (const std::exception& e) {
      write_line(failure_json(id, "internal_error", e.what()));
    }
  }
  if (!shutdown_seen) {
    // stdin closed without a shutdown request: still leave a consistent image.
    try {
      eng->close();
    } catch (const std::exception& e) {
      fprintf(stderr, "[mvdb] error while closing: %s\n", e.what());
      fflush(stderr);
    }
  }
  delete eng;
  return 0;
}
