#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <map>

namespace mvdb {

// Minimal JSON reader/writer for the judge's JSONL protocol. No third party.
struct Json {
  enum Kind { NUL, BOOL, INT, FLT, STR, ARR, OBJ } kind = NUL;
  bool b = false;
  int64_t i = 0;
  double d = 0;
  std::string s;
  std::vector<Json> arr;
  std::vector<std::pair<std::string, Json>> obj;

  const Json* find(const std::string& key) const {
    for (auto& kv : obj) if (kv.first == key) return &kv.second;
    return nullptr;
  }
  bool is_int() const { return kind == INT; }
};

// Throws std::runtime_error on malformed input.
Json json_parse(const std::string& text);

std::string json_escape(const std::string& s);

std::string json_id_text(const Json& j);

}  // namespace mvdb
