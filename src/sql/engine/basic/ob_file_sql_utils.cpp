/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SQL_ENG
#include "ob_file_sql_utils.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif

#include "share/ob_errno.h"
#include "lib/oblog/ob_log_module.h"

namespace oceanbase
{
namespace sql
{

namespace
{

struct StrLess
{
  bool operator()(const std::string &a, const std::string &b) const { return a < b; }
};

int read_whole_file(const common::ObString &path, std::string &out)
{
  int ret = OB_SUCCESS;
  std::string file_path(path.ptr(), path.length());
  FILE *fp = ::fopen(file_path.c_str(), "rb");
  if (NULL == fp) {
    ret = OB_FILE_NOT_EXIST;
    LOG_WARN("failed to open file sql source file", K(ret), K(file_path.c_str()));
  } else {
    char buf[65536];
    size_t n = 0;
    while ((n = ::fread(buf, 1, sizeof(buf), fp)) > 0) {
      out.append(buf, n);
    }
    ::fclose(fp);
    if (out.size() >= 3 && (unsigned char)out[0] == 0xEF &&
        (unsigned char)out[1] == 0xBB && (unsigned char)out[2] == 0xBF) {
      out.erase(0, 3);
    }
  }
  return ret;
}

bool is_space(char c)
{
  return ' ' == c || '\t' == c || '\r' == c || '\n' == c;
}

std::string trim_copy(const std::string &s)
{
  size_t b = 0;
  size_t e = s.size();
  while (b < e && is_space(s[b])) { ++b; }
  while (e > b && is_space(s[e - 1])) { --e; }
  return s.substr(b, e - b);
}

std::string lower_copy(const std::string &s)
{
  std::string r(s);
  for (size_t i = 0; i < r.size(); ++i) {
    if (r[i] >= 'A' && r[i] <= 'Z') { r[i] = (char)(r[i] - 'A' + 'a'); }
  }
  return r;
}

bool parse_int64(const std::string &s, int64_t &v)
{
  if (s.empty()) { return false; }
  size_t i = 0;
  bool neg = false;
  if ('-' == s[0] || '+' == s[0]) {
    neg = '-' == s[0];
    i = 1;
  }
  if (i >= s.size()) { return false; }
  int64_t value = 0;
  for (; i < s.size(); ++i) {
    if (s[i] < '0' || s[i] > '9') { return false; }
    if (value > (INT64_MAX - (s[i] - '0')) / 10) { return false; }
    value = value * 10 + (s[i] - '0');
  }
  v = neg ? -value : value;
  return true;
}

bool parse_double(const std::string &s, double &v)
{
  if (s.empty()) { return false; }
  const char *begin = s.c_str();
  char *end = NULL;
  v = ::strtod(begin, &end);
  if (end == begin) { return false; }
  while (end && *end != '\0') {
    if (!is_space(*end)) { return false; }
    ++end;
  }
  return true;
}

bool is_bool_text(const std::string &s)
{
  std::string l = lower_copy(s);
  return l == "true" || l == "false";
}

bool is_digit_char(char c) { return c >= '0' && c <= '9'; }

bool match_date(const std::string &s, int64_t &days_out)
{
  // YYYY-MM-DD, returns days since 1970-01-01 (approximate day count is enough for validation)
  if (s.size() != 10 || s[4] != '-' || s[7] != '-') { return false; }
  for (size_t i = 0; i < s.size(); ++i) {
    if (i == 4 || i == 7) { continue; }
    if (!is_digit_char(s[i])) { return false; }
  }
  int y = std::atoi(s.substr(0, 4).c_str());
  int m = std::atoi(s.substr(5, 2).c_str());
  int d = std::atoi(s.substr(8, 2).c_str());
  if (y < 1 || y > 9999 || m < 1 || m > 12 || d < 1 || d > 31) { return false; }
  static const int mon_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int max_day = mon_days[m - 1];
  if (2 == m && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) { max_day = 29; }
  if (d > max_day) { return false; }
  days_out = (int64_t)y * 372 + m * 31 + d; // comparable surrogate value
  return true;
}

bool match_datetime(const std::string &s)
{
  // YYYY-MM-DD[ T]HH:MM:SS[.fff]
  if (s.size() < 19) { return false; }
  if (!(s[10] == ' ' || s[10] == 'T')) { return false; }
  if (s[13] != ':' || s[16] != ':') { return false; }
  int64_t dummy = 0;
  if (!match_date(s.substr(0, 10), dummy)) { return false; }
  for (size_t i = 11; i < 19; ++i) {
    if (i == 13 || i == 16) { continue; }
    if (!is_digit_char(s[i])) { return false; }
  }
  if (s.size() > 19) {
    if (s[19] != '.') { return false; }
    for (size_t i = 20; i < s.size(); ++i) {
      if (!is_digit_char(s[i])) { return false; }
    }
  }
  int hh = std::atoi(s.substr(11, 2).c_str());
  int mm = std::atoi(s.substr(14, 2).c_str());
  int ss = std::atoi(s.substr(17, 2).c_str());
  return hh <= 23 && mm <= 59 && ss <= 59;
}

struct TypeAccum
{
  int64_t count_;
  bool has_int_;
  bool has_double_;
  bool has_bool_;
  bool has_date_;
  bool has_datetime_;
  bool has_text_;
  int64_t max_len_;
  TypeAccum()
    : count_(0), has_int_(false), has_double_(false), has_bool_(false), has_date_(false),
      has_datetime_(false), has_text_(false), max_len_(0) {}
};

void accum_value(TypeAccum &acc, const std::string &value)
{
  ++acc.count_;
  if ((int64_t)value.size() > acc.max_len_) { acc.max_len_ = (int64_t)value.size(); }
  int64_t dummy = 0;
  int64_t iv = 0;
  double dv = 0.0;
  if (match_datetime(value)) {
    acc.has_datetime_ = true;
  } else if (match_date(value, dummy)) {
    acc.has_date_ = true;
  } else if (is_bool_text(value)) {
    acc.has_bool_ = true;
  } else if (parse_int64(value, iv)) {
    if (0 == iv || 1 == iv) {
      acc.has_bool_ = true;
    } else {
      acc.has_int_ = true;
    }
  } else if (parse_double(value, dv)) {
    acc.has_double_ = true;
  } else {
    acc.has_text_ = true;
  }
}

ObFileSqlColumnType finalize_type(const TypeAccum &acc)
{
  ObFileSqlColumnType type = FS_COL_NULL;
  if (0 == acc.count_) {
    type = FS_COL_NULL;
  } else if (acc.has_text_) {
    type = FS_COL_VARCHAR;
  } else if (acc.has_date_ && acc.has_datetime_) {
    type = FS_COL_DATETIME;
  } else if (acc.has_date_) {
    type = (acc.has_int_ || acc.has_double_ || acc.has_bool_) ? FS_COL_VARCHAR : FS_COL_DATE;
  } else if (acc.has_datetime_) {
    type = (acc.has_int_ || acc.has_double_ || acc.has_bool_) ? FS_COL_VARCHAR : FS_COL_DATETIME;
  } else if (acc.has_double_) {
    type = FS_COL_DOUBLE;
  } else if (acc.has_int_) {
    type = FS_COL_BIGINT; // mixed 0/1 values with other integers widen to BIGINT
  } else if (acc.has_bool_) {
    type = FS_COL_BOOLEAN;
  } else {
    type = FS_COL_VARCHAR;
  }
  return type;
}

void append_json_escaped(std::string &out, const std::string &in)
{
  out.push_back('"');
  for (size_t i = 0; i < in.size(); ++i) {
    unsigned char c = (unsigned char)in[i];
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)c);
          out += buf;
        } else {
          out.push_back((char)c);
        }
    }
  }
  out.push_back('"');
}

void append_double(std::string &out, double v)
{
  if (std::isnan(v) || std::isinf(v)) {
    out += "null";
  } else {
    char buf[64];
    snprintf(buf, sizeof(buf), "%.17g", v);
    out += buf;
  }
}

void append_column_value(std::string &out, ObFileSqlColumnType type, const std::string &raw)
{
  int64_t iv = 0;
  double dv = 0.0;
  switch (type) {
    case FS_COL_BIGINT:
      if (parse_int64(raw, iv)) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%lld", (long long)iv);
        out += buf;
      } else {
        append_json_escaped(out, raw);
      }
      break;
    case FS_COL_DOUBLE:
      if (parse_double(raw, dv)) {
        append_double(out, dv);
      } else {
        append_json_escaped(out, raw);
      }
      break;
    case FS_COL_BOOLEAN: {
      std::string l = lower_copy(raw);
      out += (l == "true" || l == "1") ? "true" : "false";
      break;
    }
    case FS_COL_DATE:
    case FS_COL_DATETIME: {
      // normalize the ISO 'T' separator so the value can be cast by the SQL engine
      std::string normalized = raw;
      if (normalized.size() > 10 && 'T' == normalized[10]) {
        normalized[10] = ' ';
      }
      append_json_escaped(out, normalized);
      break;
    }
    default:
      append_json_escaped(out, raw);
      break;
  }
}

int alloc_ob_string(common::ObIAllocator &alloc, const std::string &s, common::ObString &out)
{
  int ret = OB_SUCCESS;
  char *buf = static_cast<char *>(alloc.alloc(s.size() + 1));
  if (NULL == buf) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    if (!s.empty()) { memcpy(buf, s.data(), s.size()); }
    buf[s.size()] = '\0';
    out.assign_ptr(buf, (int32_t)s.size());
  }
  return ret;
}

// ---------------------------------------------------------------------------
// CSV parsing
// ---------------------------------------------------------------------------

struct CsvData
{
  std::vector<std::vector<std::string> > rows_;
};

int parse_csv(const std::string &data, const ObFileSqlOptions &opts, CsvData &out)
{
  int ret = OB_SUCCESS;
  const char delim = (char)opts.delim_;
  const char quote = (char)opts.quote_;
  const char escape = (char)opts.escape_;
  std::vector<std::string> row;
  std::string field;
  bool in_quotes = false;
  bool field_quoted = false;
  bool row_has_data = false;
  size_t i = 0;
  while (i < data.size() && OB_SUCCESS == ret) {
    char c = data[i];
    if (in_quotes) {
      if (escape != quote && c == escape && i + 1 < data.size()) {
        char next = data[i + 1];
        if (next == quote || next == escape) {
          field.push_back(next);
          i += 2;
        } else if (next == 'n') {
          field.push_back('\n');
          i += 2;
        } else if (next == 'r') {
          field.push_back('\r');
          i += 2;
        } else if (next == 't') {
          field.push_back('\t');
          i += 2;
        } else if (next == '0') {
          field.push_back('\0');
          i += 2;
        } else {
          field.push_back(c);
          ++i;
        }
      } else if (c == quote) {
        if (i + 1 < data.size() && data[i + 1] == quote) {
          field.push_back(quote);
          i += 2;
        } else {
          in_quotes = false;
          ++i;
        }
      } else {
        field.push_back(c);
        ++i;
      }
    } else {
      if (c == quote && field.empty() && !field_quoted) {
        in_quotes = true;
        field_quoted = true;
        row_has_data = true;
        ++i;
      } else if (c == delim) {
        row.push_back(field);
        field.clear();
        field_quoted = false;
        row_has_data = true;
        ++i;
      } else if (c == '\n' || c == '\r') {
        if (c == '\r' && i + 1 < data.size() && data[i + 1] == '\n') { ++i; }
        ++i;
        row.push_back(field);
        field.clear();
        field_quoted = false;
        out.rows_.push_back(row);
        row.clear();
        row_has_data = false;
      } else {
        field.push_back(c);
        row_has_data = true;
        ++i;
      }
    }
  }
  if (OB_SUCC(ret)) {
    if (!field.empty() || !row.empty() || field_quoted) {
      row.push_back(field);
      out.rows_.push_back(row);
    } else if (row_has_data) {
      row.push_back(field);
      out.rows_.push_back(row);
    }
  }
  if (OB_SUCC(ret)) {
    // drop trailing fully empty lines
    while (!out.rows_.empty() && out.rows_.back().size() == 1 && out.rows_.back()[0].empty()) {
      out.rows_.pop_back();
    }
  }
  return ret;
}

bool detect_header(const CsvData &csv, const ObFileSqlOptions &opts)
{
  if (csv.rows_.empty()) { return false; }
  if (csv.rows_.size() < 2) { return true; }
  const std::vector<std::string> &first = csv.rows_[0];
  const std::vector<std::string> &second = csv.rows_[1];
  int text_like = 0;
  int second_typed = 0;
  for (size_t i = 0; i < first.size(); ++i) {
    TypeAccum acc;
    accum_value(acc, first[i]);
    if (acc.has_text_) { ++text_like; }
    if (i < second.size()) {
      TypeAccum acc2;
      accum_value(acc2, second[i]);
      if (!acc2.has_text_ && acc2.count_ > 0) { ++second_typed; }
    }
  }
  UNUSED(opts);
  return text_like == (int)first.size() && second_typed > 0;
}

int normalize_header_names(const std::vector<std::string> &raw_names,
                           std::vector<std::string> &names)
{
  int ret = OB_SUCCESS;
  names.clear();
  for (size_t i = 0; i < raw_names.size(); ++i) {
    std::string name = trim_copy(raw_names[i]);
    if (name.empty()) {
      char buf[32];
      snprintf(buf, sizeof(buf), "column%d", (int)i);
      name = buf;
    }
    std::string base = name;
    int suffix = 2;
    while (std::find(names.begin(), names.end(), name) != names.end()) {
      char buf[32];
      snprintf(buf, sizeof(buf), "__%d", suffix);
      name = base + buf;
      ++suffix;
    }
    names.push_back(name);
  }
  return ret;
}

int infer_csv(const std::string &data, ObFileSqlOptions &opts, std::vector<std::string> &names,
              std::vector<ObFileSqlColumnType> &types, std::vector<bool> &nullable,
              std::vector<int64_t> &max_len)
{
  int ret = OB_SUCCESS;
  CsvData csv;
  if (OB_FAIL(parse_csv(data, opts, csv))) {
  } else if (csv.rows_.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("csv file has no rows", K(ret));
  } else {
    bool has_header = opts.header_specified_ ? opts.header_ : detect_header(csv, opts);
    size_t start = 0;
    std::vector<std::string> raw_names;
    if (has_header) {
      raw_names = csv.rows_[0];
      start = 1;
    }
    size_t col_count = raw_names.size();
    if (!has_header) {
      for (size_t r = 0; r < csv.rows_.size(); ++r) {
        col_count = std::max(col_count, csv.rows_[r].size());
      }
    }
    if (has_header) {
      normalize_header_names(raw_names, names);
    } else {
      for (size_t i = 0; i < col_count; ++i) {
        char buf[32];
        snprintf(buf, sizeof(buf), "column%d", (int)i);
        names.push_back(buf);
      }
    }
    std::vector<TypeAccum> accs(col_count);
    nullable.assign(col_count, false);
    for (size_t r = start; r < csv.rows_.size(); ++r) {
      const std::vector<std::string> &row = csv.rows_[r];
      if (row.size() != col_count) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("csv row has inconsistent field count", K(ret), K(r), K(row.size()), K(col_count));
      }
      for (size_t c = 0; OB_SUCC(ret) && c < col_count; ++c) {
        const std::string &value = row[c];
        if (value.empty() || (!opts.nullstr_.empty() &&
                              value == std::string(opts.nullstr_.ptr(), opts.nullstr_.length()))) {
          nullable[c] = true;
        } else {
          accum_value(accs[c], value);
        }
      }
      if (OB_FAIL(ret)) { break; }
    }
    types.clear();
    max_len.clear();
    for (size_t c = 0; OB_SUCC(ret) && c < col_count; ++c) {
      types.push_back(finalize_type(accs[c]));
      max_len.push_back(accs[c].max_len_);
    }
  }
  return ret;
}

int csv_to_json(const std::string &data, ObFileSqlOptions &opts,
                const std::vector<std::string> &names,
                const std::vector<ObFileSqlColumnType> &types,
                std::string &out)
{
  int ret = OB_SUCCESS;
  CsvData csv;
  if (OB_FAIL(parse_csv(data, opts, csv))) {
  } else if (csv.rows_.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    bool has_header = opts.header_specified_ ? opts.header_ : detect_header(csv, opts);
    size_t start = has_header ? 1 : 0;
    size_t col_count = names.size();
    out.push_back('[');
    bool first_row = true;
    for (size_t r = start; r < csv.rows_.size(); ++r) {
      const std::vector<std::string> &row = csv.rows_[r];
      if (row.size() != col_count) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("csv row has inconsistent field count", K(ret), K(r), K(row.size()), K(col_count));
        break;
      }
      if (!first_row) { out.push_back(','); }
      first_row = false;
      out.push_back('{');
      for (size_t c = 0; c < col_count; ++c) {
        if (c > 0) { out.push_back(','); }
        append_json_escaped(out, names[c]);
        out.push_back(':');
        const std::string &value = row[c];
        if (value.empty() || (!opts.nullstr_.empty() &&
                              value == std::string(opts.nullstr_.ptr(), opts.nullstr_.length()))) {
          out += "null";
        } else {
          append_column_value(out, types[c], value);
        }
      }
      out.push_back('}');
    }
    out.push_back(']');
  }
  return ret;
}

// ---------------------------------------------------------------------------
// Minimal JSON parser (used for JSONL input)
// ---------------------------------------------------------------------------

struct JValue
{
  enum Kind { JNULL, JBOOL, JINT, JDOUBLE, JSTR, JOBJ, JARR } kind_;
  int64_t i_;
  double d_;
  bool b_;
  std::string s_;
  std::vector<std::pair<std::string, JValue> > obj_;
  std::vector<JValue> arr_;
  JValue() : kind_(JNULL), i_(0), d_(0), b_(false) {}
};

struct JParser
{
  const char *p_;
  const char *end_;
  explicit JParser(const char *b, const char *e) : p_(b), end_(e) {}
  void skip_ws()
  {
    while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\r' || *p_ == '\n')) { ++p_; }
  }
  bool parse_string(std::string &out)
  {
    out.clear();
    if (p_ >= end_ || *p_ != '"') { return false; }
    ++p_;
    while (p_ < end_) {
      char c = *p_++;
      if (c == '"') { return true; }
      if (c == '\\') {
        if (p_ >= end_) { return false; }
        char e = *p_++;
        switch (e) {
          case '"': out.push_back('"'); break;
          case '\\': out.push_back('\\'); break;
          case '/': out.push_back('/'); break;
          case 'b': out.push_back('\b'); break;
          case 'f': out.push_back('\f'); break;
          case 'n': out.push_back('\n'); break;
          case 'r': out.push_back('\r'); break;
          case 't': out.push_back('\t'); break;
          case 'u': {
            if (end_ - p_ < 4) { return false; }
            unsigned cp = 0;
            for (int k = 0; k < 4; ++k) {
              char h = *p_++;
              cp <<= 4;
              if (h >= '0' && h <= '9') { cp |= (unsigned)(h - '0'); }
              else if (h >= 'a' && h <= 'f') { cp |= (unsigned)(h - 'a' + 10); }
              else if (h >= 'A' && h <= 'F') { cp |= (unsigned)(h - 'A' + 10); }
              else { return false; }
            }
            if (cp < 0x80) {
              out.push_back((char)cp);
            } else if (cp < 0x800) {
              out.push_back((char)(0xC0 | (cp >> 6)));
              out.push_back((char)(0x80 | (cp & 0x3F)));
            } else {
              out.push_back((char)(0xE0 | (cp >> 12)));
              out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
              out.push_back((char)(0x80 | (cp & 0x3F)));
            }
            break;
          }
          default: return false;
        }
      } else {
        out.push_back(c);
      }
    }
    return false;
  }
  bool parse_value(JValue &v, int depth)
  {
    if (depth > 64) { return false; }
    skip_ws();
    if (p_ >= end_) { return false; }
    char c = *p_;
    if (c == '{') {
      ++p_;
      v.kind_ = JValue::JOBJ;
      skip_ws();
      if (p_ < end_ && *p_ == '}') { ++p_; return true; }
      while (p_ < end_) {
        skip_ws();
        std::string key;
        if (!parse_string(key)) { return false; }
        skip_ws();
        if (p_ >= end_ || *p_ != ':') { return false; }
        ++p_;
        JValue child;
        if (!parse_value(child, depth + 1)) { return false; }
        v.obj_.push_back(std::make_pair(key, child));
        skip_ws();
        if (p_ < end_ && *p_ == ',') { ++p_; continue; }
        if (p_ < end_ && *p_ == '}') { ++p_; return true; }
        return false;
      }
      return false;
    } else if (c == '[') {
      ++p_;
      v.kind_ = JValue::JARR;
      skip_ws();
      if (p_ < end_ && *p_ == ']') { ++p_; return true; }
      while (p_ < end_) {
        JValue child;
        if (!parse_value(child, depth + 1)) { return false; }
        v.arr_.push_back(child);
        skip_ws();
        if (p_ < end_ && *p_ == ',') { ++p_; continue; }
        if (p_ < end_ && *p_ == ']') { ++p_; return true; }
        return false;
      }
      return false;
    } else if (c == '"') {
      v.kind_ = JValue::JSTR;
      return parse_string(v.s_);
    } else if (end_ - p_ >= 4 && 0 == strncmp(p_, "true", 4)) {
      v.kind_ = JValue::JBOOL; v.b_ = true; p_ += 4; return true;
    } else if (end_ - p_ >= 5 && 0 == strncmp(p_, "false", 5)) {
      v.kind_ = JValue::JBOOL; v.b_ = false; p_ += 5; return true;
    } else if (end_ - p_ >= 4 && 0 == strncmp(p_, "null", 4)) {
      v.kind_ = JValue::JNULL; p_ += 4; return true;
    } else {
      const char *start = p_;
      if (p_ < end_ && (*p_ == '-' || *p_ == '+')) { ++p_; }
      bool is_double = false;
      while (p_ < end_ && ((*p_ >= '0' && *p_ <= '9') || *p_ == '.' || *p_ == 'e' ||
                           *p_ == 'E' || *p_ == '-' || *p_ == '+')) {
        if (*p_ == '.' || *p_ == 'e' || *p_ == 'E') { is_double = true; }
        ++p_;
      }
      if (p_ == start) { return false; }
      std::string num(start, p_);
      if (!is_double) {
        int64_t iv = 0;
        if (parse_int64(num, iv)) {
          v.kind_ = JValue::JINT; v.i_ = iv; return true;
        }
      }
      double dv = 0.0;
      if (!parse_double(num, dv)) { return false; }
      v.kind_ = JValue::JDOUBLE; v.d_ = dv;
      return true;
    }
  }
};

void accum_json_value(TypeAccum &acc, const JValue &v)
{
  switch (v.kind_) {
    case JValue::JNULL:
      break;
    case JValue::JBOOL:
      ++acc.count_;
      acc.has_bool_ = true;
      acc.max_len_ = std::max<int64_t>(acc.max_len_, 5);
      break;
    case JValue::JINT:
      ++acc.count_;
      if (0 == v.i_ || 1 == v.i_) { acc.has_bool_ = true; } else { acc.has_int_ = true; }
      acc.max_len_ = std::max<int64_t>(acc.max_len_, 20);
      break;
    case JValue::JDOUBLE:
      ++acc.count_;
      acc.has_double_ = true;
      acc.max_len_ = std::max<int64_t>(acc.max_len_, 24);
      break;
    case JValue::JSTR: {
      ++acc.count_;
      int64_t dummy = 0;
      if (match_datetime(v.s_)) { acc.has_datetime_ = true; }
      else if (match_date(v.s_, dummy)) { acc.has_date_ = true; }
      else { acc.has_text_ = true; }
      acc.max_len_ = std::max<int64_t>(acc.max_len_, (int64_t)v.s_.size());
      break;
    }
    default:
      break;
  }
}

void append_json_value(std::string &out, ObFileSqlColumnType type, const JValue &v)
{
  switch (v.kind_) {
    case JValue::JNULL: out += "null"; break;
    case JValue::JBOOL: out += v.b_ ? "true" : "false"; break;
    case JValue::JINT: {
      char buf[32];
      snprintf(buf, sizeof(buf), "%lld", (long long)v.i_);
      out += buf;
      break;
    }
    case JValue::JDOUBLE: append_double(out, v.d_); break;
    case JValue::JSTR:
      append_column_value(out, type, v.s_);
      break;
    default: out += "null"; break;
  }
}

int parse_jsonl_lines(const std::string &data, std::vector<JValue> &rows)
{
  int ret = OB_SUCCESS;
  size_t pos = 0;
  while (pos <= data.size() && OB_SUCC(ret)) {
    size_t eol = data.find('\n', pos);
    if (eol == std::string::npos) { eol = data.size(); }
    std::string line = data.substr(pos, eol - pos);
    if (!line.empty() && line[line.size() - 1] == '\r') { line.erase(line.size() - 1); }
    if (!trim_copy(line).empty()) {
      JParser parser(line.c_str(), line.c_str() + line.size());
      JValue v;
      if (!parser.parse_value(v, 0)) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("invalid json line", K(ret), K(line.c_str()));
      } else if (JValue::JOBJ != v.kind_) {
        ret = OB_NOT_SUPPORTED;
        LOG_WARN("json line is not an object", K(ret));
      } else {
        rows.push_back(v);
      }
    }
    if (eol == data.size()) { break; }
    pos = eol + 1;
  }
  return ret;
}

int infer_jsonl(const std::string &data, std::vector<std::string> &names,
                std::vector<ObFileSqlColumnType> &types, std::vector<bool> &nullable,
                std::vector<int64_t> &max_len)
{
  int ret = OB_SUCCESS;
  std::vector<JValue> rows;
  if (OB_FAIL(parse_jsonl_lines(data, rows))) {
  } else if (rows.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    std::vector<TypeAccum> accs;
    for (size_t r = 0; OB_SUCC(ret) && r < rows.size(); ++r) {
      for (size_t i = 0; i < rows[r].obj_.size(); ++i) {
        const std::string &key = rows[r].obj_[i].first;
        const JValue &val = rows[r].obj_[i].second;
        if (JValue::JOBJ == val.kind_ || JValue::JARR == val.kind_) {
          ret = OB_NOT_SUPPORTED;
          LOG_WARN("nested json value is not supported by read_json", K(ret), K(key.c_str()));
          break;
        }
        size_t idx = names.size();
        for (size_t k = 0; k < names.size(); ++k) {
          if (names[k] == key) { idx = k; break; }
        }
        if (idx == names.size()) {
          names.push_back(key);
          accs.push_back(TypeAccum());
          nullable.push_back(false);
        }
      }
    }
    if (OB_SUCC(ret)) {
      // mark columns missing in some row as nullable
      std::vector<int> present(names.size(), 0);
      for (size_t r = 0; r < rows.size(); ++r) {
        for (size_t i = 0; i < rows[r].obj_.size(); ++i) {
          const std::string &key = rows[r].obj_[i].first;
          const JValue &val = rows[r].obj_[i].second;
          for (size_t k = 0; k < names.size(); ++k) {
            if (names[k] == key) {
              ++present[k];
              accum_json_value(accs[k], val);
              break;
            }
          }
        }
      }
      types.clear();
      max_len.clear();
      for (size_t k = 0; k < names.size(); ++k) {
        if (present[k] < (int)rows.size()) { nullable[k] = true; }
        types.push_back(finalize_type(accs[k]));
        max_len.push_back(accs[k].max_len_);
      }
    }
  }
  return ret;
}

int jsonl_to_json(const std::string &data, const std::vector<std::string> &names,
                  const std::vector<ObFileSqlColumnType> &types, std::string &out)
{
  int ret = OB_SUCCESS;
  std::vector<JValue> rows;
  if (OB_FAIL(parse_jsonl_lines(data, rows))) {
  } else {
    out.push_back('[');
    for (size_t r = 0; r < rows.size(); ++r) {
      if (r > 0) { out.push_back(','); }
      out.push_back('{');
      for (size_t c = 0; c < names.size(); ++c) {
        if (c > 0) { out.push_back(','); }
        append_json_escaped(out, names[c]);
        out.push_back(':');
        const JValue *val = NULL;
        for (size_t i = 0; i < rows[r].obj_.size(); ++i) {
          if (rows[r].obj_[i].first == names[c]) {
            val = &rows[r].obj_[i].second;
            break;
          }
        }
        if (NULL == val || JValue::JNULL == val->kind_) {
          out += "null";
        } else if (JValue::JOBJ == val->kind_ || JValue::JARR == val->kind_) {
          ret = OB_NOT_SUPPORTED;
          break;
        } else {
          append_json_value(out, types[c], *val);
        }
      }
      out.push_back('}');
      if (OB_FAIL(ret)) { break; }
    }
    out.push_back(']');
  }
  return ret;
}

std::string format_type_name(ObFileSqlColumnType type)
{
  switch (type) {
    case FS_COL_BIGINT: return "BIGINT";
    case FS_COL_DOUBLE: return "DOUBLE";
    case FS_COL_BOOLEAN: return "BOOLEAN";
    case FS_COL_DATE: return "DATE";
    case FS_COL_DATETIME: return "DATETIME";
    case FS_COL_NULL: return "NULL";
    default: return "VARCHAR";
  }
}

// ---------------------------------------------------------------------------
// Parquet (minimal reader: PLAIN / RLE_DICTIONARY, uncompressed or snappy)
// ---------------------------------------------------------------------------

struct ThriftReader
{
  const unsigned char *p_;
  const unsigned char *end_;
  int64_t last_id_;
  ThriftReader(const unsigned char *b, const unsigned char *e) : p_(b), end_(e), last_id_(0) {}
  bool ok() const { return p_ <= end_; }
  // Reads a struct field header; 'type' is zero for the STOP marker.
  bool read_field(unsigned char &type, int64_t &id)
  {
    unsigned char b = 0;
    if (!read_byte(b)) { return false; }
    type = b & 0x0F;
    id = 0;
    if (0 == type) { return true; }
    const int64_t delta = (b & 0xF0) >> 4;
    if (0 == delta) {
      if (!read_zigzag(id)) { return false; }
    } else {
      id = last_id_ + delta;
    }
    last_id_ = id;
    return true;
  }
  bool read_byte(unsigned char &b)
  {
    if (p_ >= end_) { return false; }
    b = *p_++;
    return true;
  }
  bool read_varint(uint64_t &v)
  {
    v = 0;
    int shift = 0;
    while (p_ < end_ && shift <= 63) {
      unsigned char b = *p_++;
      v |= (uint64_t)(b & 0x7F) << shift;
      if (!(b & 0x80)) { return true; }
      shift += 7;
    }
    return false;
  }
  bool read_zigzag(int64_t &v)
  {
    uint64_t u = 0;
    if (!read_varint(u)) { return false; }
    v = (int64_t)((u >> 1) ^ (~(u & 1) + 1));
    return true;
  }
  bool read_binary(std::string &out)
  {
    uint64_t len = 0;
    if (!read_varint(len) || len > (uint64_t)(end_ - p_)) { return false; }
    out.assign((const char *)p_, (size_t)len);
    p_ += len;
    return true;
  }
  bool read_double(double &d)
  {
    if (end_ - p_ < 8) { return false; }
    memcpy(&d, p_, 8);
    p_ += 8;
    return true;
  }
};

// generic thrift compact struct skipper
bool thrift_skip_struct_field(ThriftReader &rd, unsigned char type, int depth);

bool thrift_skip_value(ThriftReader &rd, unsigned char type, int depth)
{
  if (depth > 64) { return false; }
  switch (type) {
    case 1: case 2: return true; // bool stored in field header
    case 3: { unsigned char b = 0; return rd.read_byte(b); }
    case 4: case 5: case 6: { int64_t v = 0; return rd.read_zigzag(v); }
    case 7: { double d = 0; return rd.read_double(d); }
    case 8: { std::string s; return rd.read_binary(s); }
    case 9: case 10: {
      unsigned char b = 0;
      if (!rd.read_byte(b)) { return false; }
      uint64_t size = (b >> 4) & 0x0F;
      unsigned char etype = b & 0x0F;
      if (0x0F == size) {
        if (!rd.read_varint(size)) { return false; }
      }
      for (uint64_t i = 0; i < size; ++i) {
        if (!thrift_skip_value(rd, etype, depth + 1)) { return false; }
      }
      return true;
    }
    case 11: {
      unsigned char b = 0;
      if (!rd.read_byte(b)) { return false; }
      uint64_t size = 0;
      if (!rd.read_varint(size)) { return false; }
      unsigned char kt = (b >> 4) & 0x0F;
      unsigned char vt = b & 0x0F;
      for (uint64_t i = 0; i < size; ++i) {
        if (!thrift_skip_value(rd, kt, depth + 1) || !thrift_skip_value(rd, vt, depth + 1)) {
          return false;
        }
      }
      return true;
    }
    case 12: return thrift_skip_struct_field(rd, 0, depth + 1);
    default: return false;
  }
}

bool thrift_skip_struct_field(ThriftReader &rd, unsigned char /*first*/, int depth)
{
  if (depth > 64) { return false; }
  while (true) {
    unsigned char type = 0;
    int64_t id = 0;
    if (!rd.read_field(type, id)) { return false; }
    if (0 == type) { return true; } // STOP
    if (1 == type || 2 == type) {
      continue;
    }
    if (!thrift_skip_value(rd, type, depth + 1)) { return false; }
  }
}

bool read_list_header(ThriftReader &rd, unsigned char &etype, uint64_t &size)
{
  unsigned char b = 0;
  if (!rd.read_byte(b)) { return false; }
  size = (b >> 4) & 0x0F;
  etype = b & 0x0F;
  if (0x0F == size && !rd.read_varint(size)) { return false; }
  return true;
}

struct ParquetSchemaElement
{
  int32_t type_;        // -1 when group
  int32_t repetition_;  // 0 required, 1 optional, 2 repeated
  int32_t converted_;   // converted type
  int32_t type_length_;
  std::string name_;
  int32_t num_children_;
  ParquetSchemaElement() : type_(-1), repetition_(0), converted_(-1), type_length_(0), num_children_(0) {}
};

struct ParquetColumnMeta
{
  int32_t type_;
  int32_t codec_;
  int64_t num_values_;
  int64_t data_page_offset_;
  int64_t dict_page_offset_;
  std::string path_;
  ParquetColumnMeta() : type_(0), codec_(0), num_values_(0), data_page_offset_(-1), dict_page_offset_(-1) {}
};

struct ParquetRowGroup
{
  int64_t num_rows_;
  std::vector<ParquetColumnMeta> columns_;
  ParquetRowGroup() : num_rows_(0) {}
};

struct ParquetMeta
{
  int64_t num_rows_;
  std::vector<ParquetSchemaElement> schema_;
  std::vector<ParquetRowGroup> row_groups_;
  ParquetMeta() : num_rows_(0) {}
};

bool parse_schema_element(ThriftReader &rd, ParquetSchemaElement &e)
{
  const int64_t saved_id = rd.last_id_;
  rd.last_id_ = 0;
  bool ok = true;
  while (ok) {
    unsigned char type = 0;
    int64_t id = 0;
    if (!rd.read_field(type, id)) {
      ok = false;
    } else if (0 == type) {
      break;
    } else if (1 == type || 2 == type) {
      if (id == 2) { e.type_length_ = (int32_t)(1 == type ? 1 : 0); }
    } else if (4 == type || 5 == type || 6 == type) {
      int64_t v = 0;
      if (!rd.read_zigzag(v)) {
        ok = false;
      } else if (1 == id) {
        e.type_ = (int32_t)v;
      } else if (3 == id) {
        e.repetition_ = (int32_t)v;
      } else if (5 == id) {
        e.num_children_ = (int32_t)v;
      } else if (6 == id) {
        e.converted_ = (int32_t)v;
      } else if (2 == id) {
        e.type_length_ = (int32_t)v;
      }
    } else if (8 == type) {
      std::string s;
      if (!rd.read_binary(s)) {
        ok = false;
      } else if (4 == id) {
        e.name_ = s;
      }
    } else if (!thrift_skip_value(rd, type, 0)) {
      ok = false;
    }
  }
  rd.last_id_ = saved_id;
  return ok;
}

bool parse_column_meta(ThriftReader &rd, ParquetColumnMeta &m)
{
  const int64_t saved_id = rd.last_id_;
  rd.last_id_ = 0;
  bool ok = true;
  while (ok) {
    unsigned char type = 0;
    int64_t id = 0;
    if (!rd.read_field(type, id)) {
      ok = false;
    } else if (0 == type) {
      break;
    } else if (4 == type || 5 == type || 6 == type) {
      int64_t v = 0;
      if (!rd.read_zigzag(v)) {
        ok = false;
      } else if (1 == id) {
        m.type_ = (int32_t)v;
      } else if (4 == id) {
        m.codec_ = (int32_t)v;
      } else if (5 == id) {
        m.num_values_ = v;
      } else if (8 == id || 9 == id) {
        // 8 in the 1.0 thrift definition, 9 otherwise
        m.data_page_offset_ = v;
      } else if (10 == id || 11 == id) {
        // 10 in the 1.0 thrift definition, 11 otherwise
        m.dict_page_offset_ = v;
      }
    } else if (9 == type) {
      unsigned char etype = 0;
      uint64_t size = 0;
      if (!read_list_header(rd, etype, size)) {
        ok = false;
      } else if (3 == id) {
        for (uint64_t i = 0; ok && i < size; ++i) {
          std::string s;
          if (8 != etype || !rd.read_binary(s)) {
            ok = false;
          } else {
            if (i > 0) { m.path_.push_back('.'); }
            m.path_ += s;
          }
        }
      } else {
        for (uint64_t i = 0; ok && i < size; ++i) {
          ok = thrift_skip_value(rd, etype, 0);
        }
      }
    } else if (!thrift_skip_value(rd, type, 0)) {
      ok = false;
    }
  }
  rd.last_id_ = saved_id;
  return ok;
}

bool parse_column_chunk(ThriftReader &rd, ParquetColumnMeta &m)
{
  const int64_t saved_id = rd.last_id_;
  rd.last_id_ = 0;
  bool ok = true;
  while (ok) {
    unsigned char type = 0;
    int64_t id = 0;
    if (!rd.read_field(type, id)) {
      ok = false;
    } else if (0 == type) {
      break;
    } else if (3 == id && 12 == type) {
      ok = parse_column_meta(rd, m);
    } else if (1 == type || 2 == type) {
      continue;
    } else if (!thrift_skip_value(rd, type, 0)) {
      ok = false;
    }
  }
  rd.last_id_ = saved_id;
  return ok;
}

bool parse_row_group(ThriftReader &rd, ParquetRowGroup &rg)
{
  const int64_t saved_id = rd.last_id_;
  rd.last_id_ = 0;
  bool ok = true;
  while (ok) {
    unsigned char type = 0;
    int64_t id = 0;
    if (!rd.read_field(type, id)) {
      ok = false;
    } else if (0 == type) {
      break;
    } else if (1 == id && 9 == type) {
      unsigned char etype = 0;
      uint64_t size = 0;
      if (!read_list_header(rd, etype, size)) {
        ok = false;
      } else {
        for (uint64_t i = 0; ok && i < size; ++i) {
          ParquetColumnMeta m;
          if (12 != etype || !parse_column_chunk(rd, m)) {
            ok = false;
          } else {
            rg.columns_.push_back(m);
          }
        }
      }
    } else if (6 == type) {
      int64_t v = 0;
      if (!rd.read_zigzag(v)) {
        ok = false;
      } else if (3 == id) {
        rg.num_rows_ = v;
      }
    } else if (1 == type || 2 == type) {
      continue;
    } else if (!thrift_skip_value(rd, type, 0)) {
      ok = false;
    }
  }
  rd.last_id_ = saved_id;
  return ok;
}

bool parse_file_meta_data(const unsigned char *p, const unsigned char *end, ParquetMeta &meta)
{
  ThriftReader rd(p, end);
  bool ok = true;
  while (ok) {
    unsigned char type = 0;
    int64_t id = 0;
    if (!rd.read_field(type, id)) {
      ok = false;
    } else if (0 == type) {
      break;
    } else if (2 == id && 9 == type) {
      unsigned char etype = 0;
      uint64_t size = 0;
      if (!read_list_header(rd, etype, size)) {
        ok = false;
      } else {
        for (uint64_t i = 0; ok && i < size; ++i) {
          ParquetSchemaElement e;
          if (12 != etype || !parse_schema_element(rd, e)) {
            ok = false;
          } else {
            meta.schema_.push_back(e);
          }
        }
      }
    } else if (4 == id && 9 == type) {
      unsigned char etype = 0;
      uint64_t size = 0;
      if (!read_list_header(rd, etype, size)) {
        ok = false;
      } else {
        for (uint64_t i = 0; ok && i < size; ++i) {
          ParquetRowGroup rg;
          if (12 != etype || !parse_row_group(rd, rg)) {
            ok = false;
          } else {
            meta.row_groups_.push_back(rg);
          }
        }
      }
    } else if (6 == type) {
      int64_t v = 0;
      if (!rd.read_zigzag(v)) {
        ok = false;
      } else if (3 == id) {
        meta.num_rows_ = v;
      }
    } else if (1 == type || 2 == type) {
      continue;
    } else if (!thrift_skip_value(rd, type, 0)) {
      ok = false;
    }
  }
  return ok;
}

struct ParquetColumnData
{
  std::string name_;
  int32_t physical_type_;
  int32_t converted_type_;
  int32_t max_def_;
  ObFileSqlColumnType type_;
  std::vector<std::string> values_;   // one entry per row, empty string means NULL
  std::vector<bool> is_null_;
  ParquetColumnData() : physical_type_(-1), converted_type_(-1), max_def_(0), type_(FS_COL_VARCHAR) {}
};

bool snappy_decompress(const std::string &in, std::string &out, size_t expected)
{
  out.clear();
  size_t pos = 0;
  uint64_t ulen = 0;
  int shift = 0;
  while (pos < in.size() && shift <= 63) {
    unsigned char b = (unsigned char)in[pos++];
    ulen |= (uint64_t)(b & 0x7F) << shift;
    if (!(b & 0x80)) { break; }
    shift += 7;
  }
  if (expected > 0 && ulen != expected) {
    // still continue, output length is authoritative
  }
  out.reserve((size_t)ulen);
  while (pos < in.size()) {
    unsigned char tag = (unsigned char)in[pos++];
    int kind = tag & 0x03;
    if (0 == kind) {
      uint64_t len = (tag >> 2);
      if (len < 60) {
        ++len;
      } else {
        int extra = (int)len - 59;
        if (extra < 1 || extra > 4 || pos + extra > in.size()) { return false; }
        len = 0;
        for (int i = 0; i < extra; ++i) {
          len |= (uint64_t)((unsigned char)in[pos + i]) << (8 * i);
        }
        ++len;
        pos += extra;
      }
      if (pos + len > in.size()) { return false; }
      out.append(in, pos, (size_t)len);
      pos += (size_t)len;
    } else {
      uint64_t len = 0;
      uint64_t offset = 0;
      if (1 == kind) {
        len = ((tag >> 2) & 0x07) + 4;
        offset = ((uint64_t)(tag >> 5) << 8);
        if (pos >= in.size()) { return false; }
        offset |= (unsigned char)in[pos++];
      } else if (2 == kind) {
        len = (tag >> 2) + 1;
        if (pos + 2 > in.size()) { return false; }
        offset = (unsigned char)in[pos] | ((uint64_t)(unsigned char)in[pos + 1] << 8);
        pos += 2;
      } else {
        len = (tag >> 2) + 1;
        if (pos + 4 > in.size()) { return false; }
        offset = (uint64_t)(unsigned char)in[pos] |
                 ((uint64_t)(unsigned char)in[pos + 1] << 8) |
                 ((uint64_t)(unsigned char)in[pos + 2] << 16) |
                 ((uint64_t)(unsigned char)in[pos + 3] << 24);
        pos += 4;
      }
      if (offset >= out.size()) { return false; }
      size_t start = out.size() - (size_t)offset;
      for (uint64_t i = 0; i < len; ++i) {
        out.push_back(out[start + (size_t)i]);
      }
    }
  }
  return true;
}

bool decode_rle_bitpacked(const unsigned char *data, size_t len, int bit_width,
                          int64_t num_values, std::vector<uint32_t> &out)
{
  size_t pos = 0;
  out.clear();
  while ((int64_t)out.size() < num_values && pos < len) {
    uint64_t header = 0;
    int shift = 0;
    bool ok = true;
    while (pos < len && shift <= 63) {
      unsigned char b = data[pos++];
      header |= (uint64_t)(b & 0x7F) << shift;
      if (!(b & 0x80)) { break; }
      shift += 7;
      if (pos >= len) { ok = false; }
    }
    if (!ok) { return false; }
    if (header & 1) {
      uint64_t groups = header >> 1;
      uint64_t count = groups * 8;
      uint64_t bytes_needed = groups * (uint64_t)bit_width;
      if (pos + bytes_needed > len) { return false; }
      uint64_t bit_pos = 0;
      uint64_t total_bits = count * (uint64_t)bit_width;
      for (uint64_t i = 0; i < count && (int64_t)out.size() < num_values; ++i) {
        uint32_t v = 0;
        for (int b = 0; b < bit_width; ++b) {
          uint64_t bp = bit_pos + (uint64_t)b;
          unsigned char byte = data[pos + bp / 8];
          if (byte & (1u << (bp % 8))) { v |= (1u << b); }
        }
        bit_pos += (uint64_t)bit_width;
        out.push_back(v);
        if (bit_pos >= total_bits) { break; }
      }
      pos += (size_t)bytes_needed;
    } else {
      uint64_t run = header >> 1;
      int bytes = (bit_width + 7) / 8;
      if (pos + bytes > len) { return false; }
      uint32_t v = 0;
      for (int i = 0; i < bytes; ++i) {
        v |= ((uint32_t)data[pos + i]) << (8 * i);
      }
      pos += bytes;
      for (uint64_t i = 0; i < run && (int64_t)out.size() < num_values; ++i) {
        out.push_back(v);
      }
    }
  }
  return (int64_t)out.size() >= num_values;
}

int32_t parquet_physical_to_col_type(int32_t physical, int32_t converted, int32_t repetition,
                                     ObFileSqlColumnType &type)
{
  int ret = OB_SUCCESS;
  if (6 == converted) { // DATE
    type = FS_COL_DATE;
  } else if (9 == converted || 10 == converted) { // TIMESTAMP millis / micros
    type = FS_COL_DATETIME;
  } else {
    switch (physical) {
      case 0: type = FS_COL_BOOLEAN; break;
      case 1: type = FS_COL_BIGINT; break;
      case 2: type = FS_COL_BIGINT; break;
      case 4: type = FS_COL_DOUBLE; break;
      case 5: type = FS_COL_DOUBLE; break;
      case 6: type = FS_COL_VARCHAR; break;
      default:
        ret = OB_NOT_SUPPORTED;
        LOG_WARN("unsupported parquet physical type", K(ret), K(physical));
        break;
    }
  }
  UNUSED(repetition);
  return ret;
}

std::string format_days(int64_t days)
{
  // civil_from_days
  int64_t z = days + 719468;
  int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  uint64_t doe = (uint64_t)(z - era * 146097);
  uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t y = (int64_t)yoe + era * 400;
  uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  uint64_t mp = (5 * doy + 2) / 153;
  uint64_t d = doy - (153 * mp + 2) / 5 + 1;
  uint64_t m = mp < 10 ? mp + 3 : mp - 9;
  y += (m <= 2);
  char buf[32];
  snprintf(buf, sizeof(buf), "%04lld-%02llu-%02llu", (long long)y,
           (unsigned long long)m, (unsigned long long)d);
  return buf;
}

std::string format_timestamp_millis(int64_t millis)
{
  int64_t secs = millis / 1000;
  int64_t ms = millis % 1000;
  if (ms < 0) { ms += 1000; secs -= 1; }
  int64_t days = secs / 86400;
  int64_t sod = secs % 86400;
  if (sod < 0) { sod += 86400; days -= 1; }
  char buf[64];
  snprintf(buf, sizeof(buf), "%s %02lld:%02lld:%02lld.%03lld", format_days(days).c_str(),
           (long long)(sod / 3600), (long long)((sod % 3600) / 60),
           (long long)(sod % 60), (long long)ms);
  return buf;
}

int read_parquet_column(const std::string &file_data, const ParquetMeta &meta,
                        const ParquetSchemaElement &elem, const ParquetColumnMeta &col_meta,
                        ParquetColumnData &out)
{
  int ret = OB_SUCCESS;
  out.name_ = col_meta.path_;
  out.physical_type_ = col_meta.type_;
  out.converted_type_ = elem.converted_;
  out.max_def_ = (1 == elem.repetition_) ? 1 : 0;
  if (2 == elem.repetition_) {
    ret = OB_NOT_SUPPORTED;
    LOG_WARN("repeated parquet columns are not supported", K(ret));
  } else if (OB_FAIL(parquet_physical_to_col_type(col_meta.type_, elem.converted_,
                                                  elem.repetition_, out.type_))) {
  } else {
    int64_t row_count = 0;
    for (size_t i = 0; i < meta.row_groups_.size(); ++i) {
      row_count += meta.row_groups_[i].num_rows_;
    }
    out.values_.assign((size_t)row_count, std::string());
    out.is_null_.assign((size_t)row_count, true);

    int64_t row_base = 0;
    for (size_t rg_idx = 0; OB_SUCC(ret) && rg_idx < meta.row_groups_.size(); ++rg_idx) {
      const ParquetRowGroup &rg = meta.row_groups_[rg_idx];
      const ParquetColumnMeta *cm = NULL;
      for (size_t c = 0; c < rg.columns_.size(); ++c) {
        if (rg.columns_[c].path_ == col_meta.path_) { cm = &rg.columns_[c]; break; }
      }
      if (NULL == cm) {
        row_base += rg.num_rows_;
        continue;
      }
      std::vector<int32_t> dictionary;
      std::string dict_plain;
      int64_t page_offset = cm->dict_page_offset_ >= 0 ? cm->dict_page_offset_ : cm->data_page_offset_;
      int64_t values_written = 0;
      std::vector<std::string> col_vals;
      std::vector<bool> col_nulls;
      while (OB_SUCC(ret) && page_offset >= 0 &&
             page_offset < (int64_t)file_data.size() && values_written < cm->num_values_ &&
             values_written < rg.num_rows_) {
        ThriftReader hdr((const unsigned char *)file_data.data() + page_offset,
                         (const unsigned char *)file_data.data() + file_data.size());
        int32_t page_type = -1;
        int32_t uncompressed_size = 0;
        int32_t compressed_size = 0;
        int32_t num_values = 0;
        int32_t encoding = 0;
        int32_t def_enc = 0;
        int32_t rep_enc = 0;
        int32_t dict_num_values = 0;
        int32_t v2_def_len = 0;
        int32_t v2_rep_len = 0;
        bool v2_is_compressed = true;
        bool header_ok = true;
        while (true) {
          unsigned char type = 0;
          int64_t id = 0;
          if (!hdr.read_field(type, id)) { header_ok = false; break; }
          if (0 == type) { break; }
          if (4 == type || 5 == type || 6 == type) {
            int64_t v = 0;
            if (!hdr.read_zigzag(v)) { header_ok = false; break; }
            if (1 == id) { page_type = (int32_t)v; }
            else if (2 == id) { uncompressed_size = (int32_t)v; }
            else if (3 == id) { compressed_size = (int32_t)v; }
            else if (5 == id && 0 == num_values) { /* data page header handled below */ }
          } else if (12 == type) {
            // nested struct: data_page_header(5), dictionary_page_header(7), data_page_header_v2(8)
            ThriftReader sub(hdr.p_, hdr.end_);
            if (5 == id) {
              while (true) {
                unsigned char st = 0;
                int64_t sid = 0;
                if (!sub.read_field(st, sid)) { header_ok = false; break; }
                if (0 == st) { break; }
                if (4 == st || 5 == st || 6 == st) {
                  int64_t v = 0;
                  if (!sub.read_zigzag(v)) { header_ok = false; break; }
                  if (1 == sid) { num_values = (int32_t)v; }
                  else if (2 == sid) { encoding = (int32_t)v; }
                  else if (3 == sid) { def_enc = (int32_t)v; }
                  else if (4 == sid) { rep_enc = (int32_t)v; }
                } else if (1 == st || 2 == st) {
                  continue;
                } else if (!thrift_skip_value(sub, st, 0)) { header_ok = false; break; }
              }
            } else if (7 == id) {
              while (true) {
                unsigned char st = 0;
                int64_t sid = 0;
                if (!sub.read_field(st, sid)) { header_ok = false; break; }
                if (0 == st) { break; }
                if (4 == st || 5 == st || 6 == st) {
                  int64_t v = 0;
                  if (!sub.read_zigzag(v)) { header_ok = false; break; }
                  if (1 == sid) { dict_num_values = (int32_t)v; }
                } else if (1 == st || 2 == st) {
                  continue;
                } else if (!thrift_skip_value(sub, st, 0)) { header_ok = false; break; }
              }
            } else if (8 == id) {
              // data_page_header_v2: levels are stored outside the compressed values block
              while (true) {
                unsigned char st = 0;
                int64_t sid = 0;
                if (!sub.read_field(st, sid)) { header_ok = false; break; }
                if (0 == st) { break; }
                if (4 == st || 5 == st || 6 == st) {
                  int64_t v = 0;
                  if (!sub.read_zigzag(v)) { header_ok = false; break; }
                  if (1 == sid) { num_values = (int32_t)v; }
                  else if (4 == sid) { encoding = (int32_t)v; }
                  else if (5 == sid) { v2_def_len = (int32_t)v; }
                  else if (6 == sid) { v2_rep_len = (int32_t)v; }
                } else if (1 == st || 2 == st) {
                  if (7 == sid) { v2_is_compressed = (1 == st); }
                } else if (!thrift_skip_value(sub, st, 0)) { header_ok = false; break; }
              }
            } else {
              if (!thrift_skip_value(sub, 12, 0)) { header_ok = false; }
            }
            if (!header_ok) { break; }
            hdr.p_ = sub.p_;
          } else if (!thrift_skip_value(hdr, type, 0)) {
            header_ok = false;
            break;
          }
        }
        if (!header_ok) {
          ret = OB_INVALID_ARGUMENT;
          LOG_WARN("failed to parse parquet page header", K(ret));
        } else {
          int64_t header_len = (int64_t)(hdr.p_ - ((const unsigned char *)file_data.data() + page_offset));
          if (compressed_size <= 0 || page_offset + header_len + compressed_size > (int64_t)file_data.size()) {
            ret = OB_INVALID_ARGUMENT;
            LOG_WARN("invalid parquet page size", K(ret), K(compressed_size));
          } else {
            std::string raw(file_data.data() + page_offset + header_len, (size_t)compressed_size);
            std::string page;
            if (3 == page_type) {
              // v2: repetition/definition levels are never compressed
              const int64_t levels_len = (int64_t)v2_rep_len + (int64_t)v2_def_len;
              if (levels_len < 0 || levels_len > (int64_t)raw.size()) {
                ret = OB_INVALID_ARGUMENT;
                LOG_WARN("invalid parquet v2 page levels", K(ret));
              } else {
                page.assign(raw, (size_t)levels_len, std::string::npos);
                if (!v2_is_compressed || 0 == cm->codec_) {
                  // values are stored as-is
                } else if (1 == cm->codec_) {
                  std::string inflated;
                  if (!snappy_decompress(page, inflated, 0)) {
                    ret = OB_INVALID_ARGUMENT;
                    LOG_WARN("failed to decompress snappy parquet page", K(ret));
                  } else {
                    page.swap(inflated);
                  }
                } else {
                  ret = OB_NOT_SUPPORTED;
                  LOG_WARN("unsupported parquet codec", K(ret), K(cm->codec_));
                }
              }
            } else if (0 == cm->codec_) {
              page.swap(raw);
            } else if (1 == cm->codec_) {
              if (!snappy_decompress(raw, page, (size_t)uncompressed_size)) {
                ret = OB_INVALID_ARGUMENT;
                LOG_WARN("failed to decompress snappy parquet page", K(ret));
              }
            } else {
              ret = OB_NOT_SUPPORTED;
              LOG_WARN("unsupported parquet codec", K(ret), K(cm->codec_));
            }
            if (OB_SUCC(ret)) {
              const unsigned char *p = (const unsigned char *)page.data();
              const unsigned char *pend = p + page.size();
              if (2 == page_type) { // dictionary page
                dictionary.clear();
                if (0 == encoding || 2 == encoding) {
                  int32_t t = cm->type_;
                  for (int32_t i = 0; i < dict_num_values; ++i) {
                    char buf[64];
                    if (1 == t) {
                      if (pend - p < 4) { ret = OB_INVALID_ARGUMENT; break; }
                      int32_t v = 0;
                      memcpy(&v, p, 4);
                      p += 4;
                      snprintf(buf, sizeof(buf), "%d", v);
                    } else if (2 == t) {
                      if (pend - p < 8) { ret = OB_INVALID_ARGUMENT; break; }
                      int64_t v = 0;
                      memcpy(&v, p, 8);
                      p += 8;
                      snprintf(buf, sizeof(buf), "%lld", (long long)v);
                    } else if (4 == t) {
                      if (pend - p < 4) { ret = OB_INVALID_ARGUMENT; break; }
                      float v = 0;
                      memcpy(&v, p, 4);
                      p += 4;
                      std::string s;
                      append_double(s, (double)v);
                      snprintf(buf, sizeof(buf), "%s", s.c_str());
                    } else if (5 == t) {
                      if (pend - p < 8) { ret = OB_INVALID_ARGUMENT; break; }
                      double v = 0;
                      memcpy(&v, p, 8);
                      p += 8;
                      std::string s;
                      append_double(s, v);
                      snprintf(buf, sizeof(buf), "%s", s.c_str());
                    } else {
                      ret = OB_NOT_SUPPORTED;
                      break;
                    }
                    dictionary.push_back((int32_t)dictionary.size());
                    dict_plain.append(buf); dict_plain.push_back('\x01');
                  }
                }
              } else if (0 == page_type || 3 == page_type) { // data page v1/v2
                int64_t remaining = num_values;
                std::vector<uint32_t> def_levels;
                if (out.max_def_ > 0) {
                  if (3 == page_type) {
                    if (v2_def_len > 0) {
                      std::vector<uint32_t> levels;
                      if (!decode_rle_bitpacked((const unsigned char *)raw.data() + v2_rep_len,
                                                (size_t)v2_def_len, 1, remaining, levels)) {
                        ret = OB_INVALID_ARGUMENT;
                        LOG_WARN("failed to decode parquet v2 definition levels", K(ret));
                      } else {
                        def_levels.swap(levels);
                      }
                    }
                  } else if (pend - p < 4) {
                    ret = OB_INVALID_ARGUMENT;
                  } else {
                    uint32_t level_len = 0;
                    memcpy(&level_len, p, 4);
                    p += 4;
                    if ((int64_t)level_len > pend - p) {
                      ret = OB_INVALID_ARGUMENT;
                    } else {
                      std::vector<uint32_t> levels;
                      if (!decode_rle_bitpacked(p, level_len, 1, remaining, levels)) {
                        ret = OB_INVALID_ARGUMENT;
                        LOG_WARN("failed to decode parquet definition levels", K(ret));
                      } else {
                        def_levels.swap(levels);
                      }
                      p += level_len;
                    }
                  }
                }
                if (OB_SUCC(ret)) {
                  int64_t non_null = 0;
                  for (int64_t i = 0; i < remaining; ++i) {
                    bool is_null = out.max_def_ > 0 && (i >= (int64_t)def_levels.size() || def_levels[i] < (uint32_t)out.max_def_);
                    col_nulls.push_back(is_null);
                    col_vals.push_back(std::string());
                    if (!is_null) { ++non_null; }
                  }
                  if ((5 == encoding || 8 == encoding) && !dictionary.empty()) {
                    // dictionary indices: 1 byte bit width + rle/bitpacked with 4-byte length
                    if (pend - p < 1) {
                      ret = OB_INVALID_ARGUMENT;
                    } else {
                      int bit_width = *p++;
                      uint32_t idx_len = 0;
                      if (pend - p < 4) {
                        ret = OB_INVALID_ARGUMENT;
                      } else {
                        memcpy(&idx_len, p, 4);
                        p += 4;
                        if ((int64_t)idx_len > pend - p) {
                          ret = OB_INVALID_ARGUMENT;
                        } else {
                          std::vector<uint32_t> idxs;
                          if (!decode_rle_bitpacked(p, idx_len, bit_width, non_null, idxs)) {
                            ret = OB_INVALID_ARGUMENT;
                            LOG_WARN("failed to decode parquet dictionary indices", K(ret));
                          } else {
                            int64_t vi = 0;
                            for (int64_t i = 0; i < remaining; ++i) {
                              size_t slot = col_vals.size() - (size_t)remaining + (size_t)i;
                              if (col_nulls[slot]) {
                                col_vals[slot].clear();
                              } else {
                                if (vi >= (int64_t)idxs.size()) {
                                  ret = OB_INVALID_ARGUMENT;
                                  break;
                                }
                                uint32_t di = idxs[vi++];
                                // dictionary was parsed as int64 for simplicity
                                const char *sep = dict_plain.c_str();
                                for (uint32_t k = 0; k < di; ++k) {
                                  sep = strchr(sep, '\x01');
                                  if (NULL == sep) { ret = OB_INVALID_ARGUMENT; break; }
                                  ++sep;
                                }
                                if (OB_SUCC(ret)) {
                                  const char *end_sep = strchr(sep, '\x01');
                                  if (NULL != end_sep) {
                                    col_vals[slot].assign(sep, end_sep - sep);
                                  }
                                }
                              }
                            }
                          }
                        }
                      }
                    }
                  } else if (0 == encoding) { // PLAIN
                    int64_t vi = 0;
                    for (int64_t i = 0; OB_SUCC(ret) && i < remaining; ++i) {
                      size_t slot = col_vals.size() - (size_t)remaining + (size_t)i;
                      if (col_nulls[slot]) { continue; }
                      std::string val;
                      switch (cm->type_) {
                        case 0: { // BOOLEAN
                          int64_t bit_index = vi;
                          int64_t byte_index = bit_index / 8;
                          if (p + byte_index >= pend) { ret = OB_INVALID_ARGUMENT; break; }
                          bool b = (p[byte_index] >> (bit_index % 8)) & 0x01;
                          val = b ? "true" : "false";
                          break;
                        }
                        case 1: { // INT32
                          if (pend - p < 4) { ret = OB_INVALID_ARGUMENT; break; }
                          int32_t v = 0;
                          memcpy(&v, p, 4);
                          p += 4;
                          if (6 == elem.converted_) {
                            val = format_days(v);
                          } else {
                            char buf[32];
                            snprintf(buf, sizeof(buf), "%d", v);
                            val = buf;
                          }
                          break;
                        }
                        case 2: { // INT64
                          if (pend - p < 8) { ret = OB_INVALID_ARGUMENT; break; }
                          int64_t v = 0;
                          memcpy(&v, p, 8);
                          p += 8;
                          if (9 == elem.converted_) {
                            val = format_timestamp_millis(v);
                          } else {
                            char buf[32];
                            snprintf(buf, sizeof(buf), "%lld", (long long)v);
                            val = buf;
                          }
                          break;
                        }
                        case 4: { // FLOAT
                          if (pend - p < 4) { ret = OB_INVALID_ARGUMENT; break; }
                          float fv = 0;
                          memcpy(&fv, p, 4);
                          p += 4;
                          append_double(val, (double)fv);
                          break;
                        }
                        case 5: { // DOUBLE
                          if (pend - p < 8) { ret = OB_INVALID_ARGUMENT; break; }
                          double dv = 0;
                          memcpy(&dv, p, 8);
                          p += 8;
                          append_double(val, dv);
                          break;
                        }
                        case 6: { // BYTE_ARRAY
                          if (pend - p < 4) { ret = OB_INVALID_ARGUMENT; break; }
                          uint32_t len = 0;
                          memcpy(&len, p, 4);
                          p += 4;
                          if ((int64_t)len > pend - p) { ret = OB_INVALID_ARGUMENT; break; }
                          val.assign((const char *)p, len);
                          p += len;
                          break;
                        }
                        default:
                          ret = OB_NOT_SUPPORTED;
                          break;
                      }
                      if (OB_SUCC(ret)) {
                        col_vals[slot] = val;
                        ++vi;
                      }
                    }
                  } else {
                    ret = OB_NOT_SUPPORTED;
                    LOG_WARN("unsupported parquet encoding", K(ret), K(encoding));
                  }
                }
              }
              values_written += num_values;
              page_offset += header_len + compressed_size;
            }
          }
        }
      }
      // copy rows of this row group
      for (int64_t i = 0; OB_SUCC(ret) && i < rg.num_rows_; ++i) {
        if (i < (int64_t)col_vals.size()) {
          out.values_[(size_t)(row_base + i)] = col_vals[(size_t)i];
          out.is_null_[(size_t)(row_base + i)] = col_nulls[(size_t)i];
        }
      }
      row_base += rg.num_rows_;
    }
  }
  return ret;
}

int read_parquet(const std::string &data, ParquetMeta &meta,
                 std::vector<ParquetColumnData> &columns)
{
  int ret = OB_SUCCESS;
  if (data.size() < 12 || 0 != strncmp(data.data(), "PAR1", 4) ||
      0 != strncmp(data.data() + data.size() - 4, "PAR1", 4)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid parquet file", K(ret));
  } else {
    uint32_t meta_len = 0;
    memcpy(&meta_len, data.data() + data.size() - 8, 4);
    if ((int64_t)meta_len + 8 > (int64_t)data.size()) {
      ret = OB_INVALID_ARGUMENT;
    } else if (!parse_file_meta_data((const unsigned char *)data.data() + data.size() - 8 - meta_len,
                                     (const unsigned char *)data.data() + data.size() - 8, meta)) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("failed to parse parquet metadata", K(ret));
    } else {
      // schema: element 0 is the root; flat schemas only
      for (size_t i = 1; OB_SUCC(ret) && i < meta.schema_.size(); ++i) {
        const ParquetSchemaElement &elem = meta.schema_[i];
        if (-1 == elem.type_) {
          ret = OB_NOT_SUPPORTED;
          LOG_WARN("nested parquet schema is not supported", K(ret));
        } else if (meta.row_groups_.empty()) {
          ParquetColumnData col;
          col.name_ = elem.name_;
          ret = parquet_physical_to_col_type(elem.type_, elem.converted_, elem.repetition_, col.type_);
          col.values_.clear();
          col.is_null_.clear();
          columns.push_back(col);
        } else {
          bool found = false;
          ParquetColumnMeta cm;
          for (size_t c = 0; c < meta.row_groups_[0].columns_.size(); ++c) {
            if (meta.row_groups_[0].columns_[c].path_ == elem.name_) {
              cm = meta.row_groups_[0].columns_[c];
              found = true;
              break;
            }
          }
          if (!found) {
            ret = OB_INVALID_ARGUMENT;
          } else {
            ParquetColumnData col;
            if (OB_FAIL(read_parquet_column(data, meta, elem, cm, col))) {
            } else {
              columns.push_back(col);
            }
          }
        }
      }
    }
  }
  return ret;
}

} // anonymous namespace

int parse_file_sql_options(const common::ObString &text, ObFileSqlOptions &opts)
{
  int ret = OB_SUCCESS;
  std::string s(text.ptr(), text.length());
  size_t pos = 0;
  while (pos <= s.size() && OB_SUCC(ret)) {
    size_t sep = s.find(';', pos);
    if (sep == std::string::npos) { sep = s.size(); }
    std::string item = s.substr(pos, sep - pos);
    size_t eq = item.find('=');
    if (eq != std::string::npos) {
      std::string key = lower_copy(trim_copy(item.substr(0, eq)));
      std::string value = trim_copy(item.substr(eq + 1));
      if (key == "enabled") {
        opts.enabled_ = value != "0" && lower_copy(value) != "false";
      } else if (key == "header") {
        opts.header_specified_ = true;
        opts.header_ = value != "0" && lower_copy(value) != "false";
      } else if (key == "delim") {
        opts.delim_ = value.empty() ? ',' : (int64_t)(unsigned char)value[0];
      } else if (key == "quote") {
        opts.quote_ = value.empty() ? '"' : (int64_t)(unsigned char)value[0];
      } else if (key == "escape") {
        opts.escape_ = value.empty() ? '\\' : (int64_t)(unsigned char)value[0];
      } else if (key == "nullstr") {
        if (!value.empty()) {
          opts.nullstr_.assign_ptr((char *)text.ptr() + (text.ptr() - s.data()) + 0, 0); // placeholder
        }
      } else if (key == "nullstr_value") {
        // nullstr is carried separately, see below
      } else if (key == "format") {
        // format is passed separately
      } else if (key == "sample") {
        int64_t v = 0;
        if (parse_int64(value, v)) { opts.sample_size_ = v; }
      }
    }
    if (sep == s.size()) { break; }
    pos = sep + 1;
  }
  // nullstr and format are encoded with explicit markers not containing ';'
  size_t np = lower_copy(s).find("nullstr_value=");
  if (np != std::string::npos) {
    size_t start = np + strlen("nullstr_value=");
    size_t end = s.find_first_of(";", start);
    if (end == std::string::npos) { end = s.size(); }
    std::string value = s.substr(start, end - start);
    if (!value.empty()) {
      opts.nullstr_.assign_ptr((char *)text.ptr() + start, (int32_t)value.size());
    }
  }
  size_t fp = lower_copy(s).find("format_value=");
  if (fp != std::string::npos) {
    size_t start = fp + strlen("format_value=");
    size_t end = s.find_first_of(";", start);
    if (end == std::string::npos) { end = s.size(); }
    std::string value = lower_copy(s.substr(start, end - start));
    if (!value.empty()) {
      opts.format_.assign_ptr((char *)text.ptr() + start, (int32_t)value.size());
    }
  }
  return ret;
}

bool file_sql_path_allowed(const common::ObString &secure_file_priv,
                           const common::ObString &path)
{
  if (secure_file_priv.empty() || path.empty()) {
    return false;
  }
  std::string priv = trim_copy(std::string(secure_file_priv.ptr(), secure_file_priv.length()));
  std::string full(path.ptr(), path.length());
  if (priv.empty()) { return false; }
  for (size_t i = 0; i < priv.size(); ++i) {
    if (priv[i] == '\\') { priv[i] = '/'; }
  }
  for (size_t i = 0; i < full.size(); ++i) {
    if (full[i] == '\\') { full[i] = '/'; }
  }
  std::string lp = lower_copy(priv);
  std::string lf = lower_copy(full);
  if ("/" == lp) {
    // a bare root allows every absolute path
    return true;
  }
  if (lf.size() < lp.size()) { return false; }
  if (0 != lf.compare(0, lp.size(), lp)) { return false; }
  if (lf.size() > lp.size() && lp[lp.size() - 1] != '/' && lf[lp.size()] != '/') {
    return false;
  }
  return true;
}

int infer_file_sql_schema(common::ObIAllocator &alloc,
                          const common::ObString &path,
                          ObFileSqlOptions &opts,
                          common::ObIArray<ObFileSqlColumn> &columns,
                          common::ObString &format_out)
{
  int ret = OB_SUCCESS;
  std::string data;
  std::string format = lower_copy(std::string(opts.format_.ptr(), opts.format_.length()));
  if (OB_FAIL(read_whole_file(path, data))) {
  } else {
    std::vector<std::string> names;
    std::vector<ObFileSqlColumnType> types;
    std::vector<bool> nullable;
    std::vector<int64_t> max_len;
    if (format.empty() || format == "csv" || format == "auto") {
      format = "csv";
      ret = infer_csv(data, opts, names, types, nullable, max_len);
    } else if (format == "newline_delimited" || format == "ndjson" || format == "json") {
      format = "json";
      ret = infer_jsonl(data, names, types, nullable, max_len);
    } else if (format == "parquet") {
      ParquetMeta meta;
      std::vector<ParquetColumnData> pcols;
      if (OB_FAIL(read_parquet(data, meta, pcols))) {
      } else {
        for (size_t i = 0; i < pcols.size(); ++i) {
          names.push_back(pcols[i].name_);
          types.push_back(pcols[i].type_);
          bool nullable_col = false;
          int64_t ml = 0;
          for (size_t r = 0; r < pcols[i].values_.size(); ++r) {
            if (pcols[i].is_null_[r]) { nullable_col = true; }
            ml = std::max<int64_t>(ml, (int64_t)pcols[i].values_[r].size());
          }
          nullable.push_back(nullable_col);
          max_len.push_back(ml);
        }
      }
    } else {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("unsupported file sql format", K(ret), K(format.c_str()));
    }
    if (OB_SUCC(ret)) {
      for (size_t i = 0; OB_SUCC(ret) && i < names.size(); ++i) {
        ObFileSqlColumn col;
        if (OB_FAIL(alloc_ob_string(alloc, names[i], col.name_))) {
        } else {
          col.type_ = types[i];
          col.nullable_ = nullable[i];
          col.max_len_ = max_len[i];
          if (OB_FAIL(columns.push_back(col))) {
          }
        }
      }
      if (OB_SUCC(ret)) {
        format_out.assign_ptr(opts.format_.ptr(), opts.format_.length());
      }
    }
  }
  return ret;
}

int read_file_sql_json(common::ObIAllocator &alloc,
                       const common::ObString &path,
                       const ObFileSqlOptions &opts,
                       common::ObString &json_out)
{
  int ret = OB_SUCCESS;
  std::string data;
  std::string format = lower_copy(std::string(opts.format_.ptr(), opts.format_.length()));
  if (OB_FAIL(read_whole_file(path, data))) {
  } else {
    std::string out;
    std::vector<std::string> names;
    std::vector<ObFileSqlColumnType> types;
    std::vector<bool> nullable;
    std::vector<int64_t> max_len;
    ObFileSqlOptions local_opts = opts;
    if (format.empty() || format == "csv" || format == "auto") {
      if (OB_FAIL(infer_csv(data, local_opts, names, types, nullable, max_len))) {
      } else if (OB_FAIL(csv_to_json(data, local_opts, names, types, out))) {
      }
    } else if (format == "newline_delimited" || format == "ndjson" || format == "json") {
      if (OB_FAIL(infer_jsonl(data, names, types, nullable, max_len))) {
      } else if (OB_FAIL(jsonl_to_json(data, names, types, out))) {
      }
    } else if (format == "parquet") {
      ParquetMeta meta;
      std::vector<ParquetColumnData> pcols;
      if (OB_FAIL(read_parquet(data, meta, pcols))) {
      } else {
        out.push_back('[');
        for (int64_t r = 0; r < meta.num_rows_; ++r) {
          if (r > 0) { out.push_back(','); }
          out.push_back('{');
          for (size_t c = 0; c < pcols.size(); ++c) {
            if (c > 0) { out.push_back(','); }
            append_json_escaped(out, pcols[c].name_);
            out.push_back(':');
            if (r >= (int64_t)pcols[c].values_.size() || pcols[c].is_null_[(size_t)r]) {
              out += "null";
            } else {
              append_column_value(out, pcols[c].type_, pcols[c].values_[(size_t)r]);
            }
          }
          out.push_back('}');
        }
        out.push_back(']');
      }
    } else {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("unsupported file sql format", K(ret), K(format.c_str()));
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(alloc_ob_string(alloc, out, json_out))) {
      }
    }
  }
  return ret;
}

// Shell-style '*'/'?' matching, portable across the Win32 and dirent paths.
namespace
{
bool glob_match(const char *name, const char *pattern)
{
  const char *star = NULL;
  const char *resume = NULL;
  while ('\0' != *name) {
    if ('?' == *pattern || *pattern == *name) {
      ++name;
      ++pattern;
    } else if ('*' == *pattern) {
      star = pattern++;
      resume = name;
    } else if (NULL != star) {
      pattern = star + 1;
      name = ++resume;
    } else {
      return false;
    }
  }
  while ('*' == *pattern) {
    ++pattern;
  }
  return '\0' == *pattern;
}
} // anonymous namespace

int glob_file_sql(common::ObIAllocator &alloc,
                  const common::ObString &pattern,
                  common::ObString &json_out)
{
  int ret = OB_SUCCESS;
  std::string pat(pattern.ptr(), pattern.length());
  std::string dir;
  std::string file_pat;
  size_t slash = pat.find_last_of("/\\");
  if (slash == std::string::npos) {
    dir = ".";
    file_pat = pat;
  } else {
    dir = pat.substr(0, slash);
    file_pat = pat.substr(slash + 1);
    if (dir.empty()) { dir = pat.substr(0, 1); }
  }
  std::vector<std::string> files;
#ifdef _WIN32
  WIN32_FIND_DATAA fd;
  std::string search = dir + "\\*";
  HANDLE h = FindFirstFileA(search.c_str(), &fd);
  if (INVALID_HANDLE_VALUE != h) {
    do {
      if (0 != (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) { continue; }
      if (!glob_match(fd.cFileName, file_pat.c_str())) { continue; }
      files.push_back(dir + "/" + std::string(fd.cFileName));
    } while (FindNextFileA(h, &fd));
    FindClose(h);
  }
#else
  DIR *d = opendir(dir.c_str());
  if (NULL != d) {
    struct dirent *ent = NULL;
    while (NULL != (ent = readdir(d))) {
      if (0 == strcmp(ent->d_name, ".") || 0 == strcmp(ent->d_name, "..")) { continue; }
      if (!glob_match(ent->d_name, file_pat.c_str())) { continue; }
      std::string full = dir + "/" + ent->d_name;
      struct stat st;
      if (0 == stat(full.c_str(), &st) && S_ISREG(st.st_mode)) {
        files.push_back(full);
      }
    }
    closedir(d);
  }
#endif
  std::sort(files.begin(), files.end(), StrLess());
  std::string out("[");
  for (size_t i = 0; i < files.size(); ++i) {
    if (i > 0) { out.push_back(','); }
    out += "{\"file\":";
    append_json_escaped(out, files[i]);
    out.push_back('}');
  }
  out.push_back(']');
  if (OB_SUCC(ret)) {
    ret = alloc_ob_string(alloc, out, json_out);
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
