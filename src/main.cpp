#include "engine.h"
#include "sql.h"
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

using namespace mvdb;

namespace {

size_t find_stmt_end(const std::string& s) {
  char quote = 0;
  for (size_t i = 0; i < s.size(); i++) {
    char c = s[i];
    if (quote) {
      if (c == '\\') { i++; continue; }
      if (c == quote) {
        if (i + 1 < s.size() && s[i + 1] == quote) { i++; continue; }
        quote = 0;
      }
      continue;
    }
    if (c == '\'' || c == '"') quote = c;
    else if (c == ';') return i;
  }
  return std::string::npos;
}

std::string trim(const std::string& s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

void emit_result(const Result& r) {
  if (!r.ok) {
    fprintf(stderr, "[mvdb] error %d: %s\n", r.err, r.msg.c_str());
    printf("FAILURE\n");
  } else if (!r.col_names.empty()) {
    for (size_t i = 0; i < r.col_names.size(); i++) {
      if (i) printf(" | ");
      printf("%s", r.col_names[i].c_str());
    }
    printf("\n");
    for (auto& row : r.rows) {
      for (size_t i = 0; i < row.size(); i++) {
        if (i) printf(" | ");
        printf("%s", row[i].c_str());
      }
      printf("\n");
    }
  } else if (r.msg == "EXIT") {
    // handled by caller
  } else {
    printf("SUCCESS\n");
  }
  fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
  std::string dir = "/data";
  std::string mode = "exact";
  size_t cache_mb = 32;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--data-dir") && i + 1 < argc) dir = argv[++i];
    else if (!strcmp(argv[i], "--mode") && i + 1 < argc) mode = argv[++i];
    else if (!strcmp(argv[i], "--cache-mb") && i + 1 < argc) cache_mb = atoi(argv[++i]);
  }
  try {
    Engine eng(dir, cache_mb * 1024 * 1024, mode);
    eng.open();
    printf("READY\n");
    fflush(stdout);
    std::string buf;
    std::string line;
    bool exiting = false;
    while (!exiting && std::getline(std::cin, line)) {
      buf += line;
      buf += "\n";
      size_t end;
      while ((end = find_stmt_end(buf)) != std::string::npos) {
        std::string sql = trim(buf.substr(0, end));
        buf.erase(0, end + 1);
        if (sql.empty()) continue;
        Result r;
        try {
          Stmt st = parse_sql(sql);
          r = eng.execute(st);
          if (r.ok && r.msg == "EXIT") exiting = true;
        } catch (const MVError& e) {
          r.ok = false;
          r.err = e.code;
          r.msg = e.what();
        }
        emit_result(r);
        if (exiting) break;
      }
    }
    eng.close();
  } catch (const std::exception& e) {
    fprintf(stderr, "[mvdb] fatal: %s\n", e.what());
    return 1;
  }
  return 0;
}