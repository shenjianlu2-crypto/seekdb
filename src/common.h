#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <stdexcept>

namespace mvdb {

enum class ColType : uint8_t { BIGINT = 0, VARCHAR = 1, VECTOR = 2 };

struct Value {
  ColType type = ColType::BIGINT;
  int64_t i = 0;
  double d = 0;
  std::string s;
  std::vector<float> v;

  static Value bigint(int64_t x) { Value v; v.type = ColType::BIGINT; v.i = x; return v; }
  static Value varchar(std::string x) { Value v; v.type = ColType::VARCHAR; v.s = std::move(x); return v; }
  static Value vector(std::vector<float> x) { Value v; v.type = ColType::VECTOR; v.v = std::move(x); return v; }
};

struct MVError : std::runtime_error {
  int code;
  MVError(int c, const std::string& m) : std::runtime_error(m), code(c) {}
};

enum ErrCode {
  ER_SYNTAX = 1064,
  ER_DUP_PK = 1062,
  ER_NO_TABLE = 1146,
  ER_NO_COLUMN = 1054,
  ER_BAD_VALUE = 1366,
  ER_TOO_LONG = 1406,
  ER_UNSUPPORTED = 1235,
  ER_TXN = 1568,
  ER_INTERNAL = 1815,
};

inline size_t utf8_len(const std::string& s) {
  size_t n = 0;
  for (unsigned char c : s) if ((c & 0xC0) != 0x80) n++;
  return n;
}

inline std::vector<float> parse_vector_literal(const std::string& raw) {
  std::vector<float> out;
  size_t i = 0, n = raw.size();
  while (i < n && isspace((unsigned char)raw[i])) i++;
  if (i >= n || raw[i] != '[') throw MVError(ER_BAD_VALUE, "vector literal must start with '['");
  i++;
  while (i < n && isspace((unsigned char)raw[i])) i++;
  if (i < n && raw[i] == ']') { i++; }
  else {
    for (;;) {
      while (i < n && isspace((unsigned char)raw[i])) i++;
      size_t start = i;
      while (i < n && raw[i] != ',' && raw[i] != ']') i++;
      std::string tok = raw.substr(start, i - start);
      while (!tok.empty() && isspace((unsigned char)tok.back())) tok.pop_back();
      if (tok.empty()) throw MVError(ER_BAD_VALUE, "empty vector element");
      char* end = nullptr;
      double d = strtod(tok.c_str(), &end);
      if (end != tok.c_str() + tok.size()) throw MVError(ER_BAD_VALUE, "invalid vector element: " + tok);
      if (!std::isfinite(d)) throw MVError(ER_BAD_VALUE, "vector element not finite");
      if (d > 3.402823466e38 || d < -3.402823466e38) throw MVError(ER_BAD_VALUE, "vector element overflows float32");
      out.push_back((float)d);
      while (i < n && isspace((unsigned char)raw[i])) i++;
      if (i < n && raw[i] == ',') { i++; continue; }
      if (i < n && raw[i] == ']') { i++; break; }
      throw MVError(ER_BAD_VALUE, "vector literal must end with ']'");
    }
  }
  while (i < n && isspace((unsigned char)raw[i])) i++;
  if (i != n) throw MVError(ER_BAD_VALUE, "trailing characters in vector literal");
  return out;
}

inline std::string fmt_double(double d) {
  char buf[64];
  snprintf(buf, sizeof buf, "%.2f", d);
  std::string s(buf);
  if (s.find('.') != std::string::npos) {
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
  }
  if (s == "-0") s = "0";
  return s;
}

inline std::string fmt_float(float f) { return fmt_double((double)f); }

inline std::string vector_to_string(const std::vector<float>& v) {
  std::string s = "[";
  for (size_t i = 0; i < v.size(); i++) {
    if (i) s += ", ";
    s += fmt_float(v[i]);
  }
  s += "]";
  return s;
}

inline std::string value_to_string(const Value& v) {
  switch (v.type) {
    case ColType::BIGINT: return std::to_string(v.i);
    case ColType::VARCHAR: return v.s;
    case ColType::VECTOR: return vector_to_string(v.v);
    default: return fmt_double(v.d);
  }
}

inline double l2_distance(const std::vector<float>& a, const std::vector<float>& b) {
  double sum = 0;
  size_t n = a.size() < b.size() ? a.size() : b.size();
  for (size_t i = 0; i < n; i++) {
    double d = (double)a[i] - (double)b[i];
    sum += d * d;
  }
  return std::sqrt(sum);
}

}  // namespace mvdb