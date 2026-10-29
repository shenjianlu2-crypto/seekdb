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

#define USING_LOG_PREFIX SQL_REWRITE
#include "observer/mysql/ob_file_sql_rewrite.h"

#include <string>
#include <vector>
#include <algorithm>
#include <cstring>
#include <cstdlib>

#include "lib/string/ob_sql_string.h"
#include "share/ob_errno.h"
#include "sql/engine/basic/ob_file_sql_utils.h"
#include "sql/session/ob_sql_session_info.h"

namespace oceanbase
{
namespace observer
{

namespace
{

const char *const ENABLE_FLAG_VAR = "__enable_file_sql";

char lower_c(char c)
{
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool starts_with_ci(const char *s, int64_t len, const char *kw)
{
  const int64_t kw_len = static_cast<int64_t>(strlen(kw));
  if (len < kw_len) {
    return false;
  }
  for (int64_t i = 0; i < kw_len; ++i) {
    if (lower_c(s[i]) != lower_c(kw[i])) {
      return false;
    }
  }
  return true;
}

bool is_ident_char(char c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '$';
}

void skip_blank(const char *s, int64_t len, int64_t &pos)
{
  while (pos < len) {
    const char c = s[pos];
    if (' ' == c || '\t' == c || '\n' == c || '\r' == c) {
      ++pos;
    } else if ('-' == c && pos + 1 < len && '-' == s[pos + 1]) {
      pos += 2;
      while (pos < len && '\n' != s[pos]) { ++pos; }
    } else if ('/' == c && pos + 1 < len && '*' == s[pos + 1]) {
      pos += 2;
      while (pos + 1 < len && !('/' == s[pos] && '*' == s[pos + 1])) { ++pos; }
      pos += 2;
    } else {
      break;
    }
  }
}

// Advances *pos past the string literal starting at *pos (which must be a quote).
// Returns false when the literal is not terminated.
bool skip_quoted(const char *s, int64_t len, int64_t &pos)
{
  const char quote = s[pos];
  ++pos;
  while (pos < len) {
    if ('\\' == s[pos] && quote != '`') {
      pos += 2;
    } else if (quote == s[pos]) {
      if (pos + 1 < len && quote == s[pos + 1]) {
        pos += 2;
      } else {
        ++pos;
        return true;
      }
    } else {
      ++pos;
    }
  }
  return false;
}

// Finds the next occurrence of 'kw' outside of quoted regions.
int64_t find_keyword(const char *s, int64_t len, int64_t from, const char *kw)
{
  const int64_t kw_len = static_cast<int64_t>(strlen(kw));
  int64_t pos = from;
  while (pos < len) {
    const char c = s[pos];
    if ('\'' == c || '"' == c || '`' == c) {
      if (!skip_quoted(s, len, pos)) {
        return -1;
      }
    } else if (starts_with_ci(s + pos, len - pos, kw)) {
      const bool left_ok = (pos == 0) || !is_ident_char(s[pos - 1]);
      const bool right_ok = (pos + kw_len >= len) || !is_ident_char(s[pos + kw_len]);
      if (left_ok && right_ok) {
        return pos;
      }
      ++pos;
    } else {
      ++pos;
    }
  }
  return -1;
}

// Parses "name(args)" at position 'pos'.  Produces the argument text range.
struct CallInfo
{
  int64_t name_begin_;
  int64_t lparen_pos_;
  int64_t args_begin_;
  int64_t args_end_;
  int64_t end_pos_;
  std::string name_;
  CallInfo() : name_begin_(0), lparen_pos_(0), args_begin_(0), args_end_(0), end_pos_(0) {}
};

bool parse_call(const char *s, int64_t len, int64_t pos, CallInfo &call)
{
  bool ok = false;
  int64_t p = pos;
  call.name_begin_ = p;
  while (p < len && is_ident_char(s[p])) { ++p; }
  if (p == call.name_begin_) {
  } else {
    call.name_.assign(s + call.name_begin_, static_cast<size_t>(p - call.name_begin_));
    for (size_t i = 0; i < call.name_.size(); ++i) { call.name_[i] = lower_c(call.name_[i]); }
    skip_blank(s, len, p);
    if (p < len && '(' == s[p]) {
      call.lparen_pos_ = p;
      ++p;
      call.args_begin_ = p;
      int64_t depth = 1;
      while (p < len && depth > 0) {
        const char c = s[p];
        if ('\'' == c || '"' == c || '`' == c) {
          if (!skip_quoted(s, len, p)) { break; }
        } else if ('(' == c) {
          ++depth;
          ++p;
        } else if (')' == c) {
          --depth;
          if (0 == depth) {
            call.args_end_ = p;
            call.end_pos_ = p + 1;
            ok = true;
          }
          ++p;
        } else {
          ++p;
        }
      }
    }
  }
  return ok;
}

std::string trim_copy(const std::string &s)
{
  size_t b = 0;
  size_t e = s.size();
  while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\n' || s[b] == '\r')) { ++b; }
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\n' || s[e - 1] == '\r')) { --e; }
  return s.substr(b, e - b);
}

std::string lower_copy(const std::string &s)
{
  std::string out = s;
  for (size_t i = 0; i < out.size(); ++i) { out[i] = lower_c(out[i]); }
  return out;
}

// Splits the argument text on top level commas.
int split_args(const char *s, int64_t begin, int64_t end, std::vector<std::string> &args)
{
  int64_t pos = begin;
  int64_t start = begin;
  int64_t depth = 0;
  while (pos < end) {
    const char c = s[pos];
    if ('\'' == c || '"' == c || '`' == c) {
      if (!skip_quoted(s, end, pos)) {
        return OB_ERR_PARSE_SQL;
      }
    } else if ('(' == c) {
      ++depth;
      ++pos;
    } else if (')' == c) {
      --depth;
      ++pos;
    } else if (',' == c && 0 == depth) {
      args.push_back(trim_copy(std::string(s + start, static_cast<size_t>(pos - start))));
      start = pos + 1;
      ++pos;
    } else {
      ++pos;
    }
  }
  std::string last = trim_copy(std::string(s + start, static_cast<size_t>(end - start)));
  if (!last.empty()) {
    args.push_back(last);
  }
  return OB_SUCCESS;
}

// Converts a quoted SQL literal into its raw value.
bool unquote(const std::string &text, std::string &out)
{
  out.clear();
  if (text.size() < 2) {
    return false;
  }
  const char quote = text[0];
  if ((quote != '\'' && quote != '"') || text[text.size() - 1] != quote) {
    return false;
  }
  size_t i = 1;
  const size_t end = text.size() - 1;
  while (i < end) {
    const char c = text[i];
    if ('\\' == c && i + 1 < end) {
      const char n = text[i + 1];
      switch (n) {
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case '0': out.push_back('\0'); break;
        case 'b': out.push_back('\b'); break;
        case 'Z': out.push_back('\032'); break;
        default: out.push_back(n); break;
      }
      i += 2;
    } else if (quote == c && i + 1 < end && quote == text[i + 1]) {
      out.push_back(quote);
      i += 2;
    } else {
      out.push_back(c);
      ++i;
    }
  }
  return true;
}

bool parse_bool_text(const std::string &text, bool &value)
{
  const std::string v = lower_copy(trim_copy(text));
  bool ok = true;
  if (v == "true" || v == "on" || v == "1" || v == "yes") {
    value = true;
  } else if (v == "false" || v == "off" || v == "0" || v == "no") {
    value = false;
  } else {
    ok = false;
  }
  return ok;
}

const char *type_name(const sql::ObFileSqlColumnType type)
{
  const char *name = "VARCHAR";
  switch (type) {
    case sql::FS_COL_BIGINT: name = "BIGINT"; break;
    case sql::FS_COL_DOUBLE: name = "DOUBLE"; break;
    case sql::FS_COL_BOOLEAN: name = "BOOLEAN"; break;
    case sql::FS_COL_DATE: name = "DATE"; break;
    case sql::FS_COL_DATETIME: name = "DATETIME"; break;
    case sql::FS_COL_NULL: name = "NULL"; break;
    case sql::FS_COL_VARCHAR:
    default: name = "VARCHAR"; break;
  }
  return name;
}

void append_sql_literal_escaped(std::string &out, const char *data, int64_t len)
{
  out.push_back('\'');
  for (int64_t i = 0; i < len; ++i) {
    const char c = data[i];
    if ('\\' == c || '\'' == c) {
      out.push_back('\\');
    }
    out.push_back(c);
  }
  out.push_back('\'');
}

bool is_plain_column_name(const std::string &name)
{
  bool plain = !name.empty();
  if (plain && name[0] >= '0' && name[0] <= '9') {
    plain = false;
  }
  for (size_t i = 0; plain && i < name.size(); ++i) {
    const char c = name[i];
    plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || '_' == c;
  }
  return plain;
}

void append_quoted_ident(std::string &out, const std::string &name)
{
  out.push_back('`');
  for (size_t i = 0; i < name.size(); ++i) {
    if ('`' == name[i]) { out.push_back('`'); }
    out.push_back(name[i]);
  }
  out.push_back('`');
}

// Generates "json_table('<json>', '$[*]' COLUMNS (...))"
void build_json_table(std::string &out,
                      const std::string &json_text,
                      const common::ObIArray<sql::ObFileSqlColumn> &columns)
{
  out += "json_table(";
  append_sql_literal_escaped(out, json_text.data(), static_cast<int64_t>(json_text.size()));
  out += ", '$[*]' COLUMNS (";
  if (columns.count() <= 0) {
    out += "`__file_sql_empty` BIGINT PATH '$.__file_sql_empty'";
  }
  for (int64_t i = 0; i < columns.count(); ++i) {
    const sql::ObFileSqlColumn &col = columns.at(i);
    const std::string name(col.name_.ptr(), static_cast<size_t>(col.name_.length()));
    if (i > 0) { out += ", "; }
    append_quoted_ident(out, name);
    out += ' ';
    const char *tname = type_name(col.type_);
    out += tname;
    if (0 == strcmp(tname, "VARCHAR")) {
      out += "(65535)";
    }
    out += " PATH '";
    if (is_plain_column_name(name)) {
      out += "$.";
      out += name;
    } else {
      out += "$.\"";
      for (size_t k = 0; k < name.size(); ++k) {
        if ('"' == name[k] || '\\' == name[k]) { out.push_back('\\'); }
        out.push_back(name[k]);
      }
      out += '"';
    }
    out += '\'';
  }
  out += "))";
}

// Interprets the parsed arguments of a read_* / glob call.
int interpret_read_args(const std::vector<std::string> &args,
                        const std::string &func,
                        sql::ObFileSqlOptions &opts,
                        std::string &path,
                        bool &is_glob)
{
  int ret = OB_SUCCESS;
  is_glob = (func == "glob");
  if (args.empty()) {
    ret = OB_ERR_PARSE_SQL;
  } else if (!unquote(args[0], path) || path.empty()) {
    ret = OB_ERR_PARSE_SQL;
  }

  std::string default_format = "csv";
  if ("read_csv" == func) {
    default_format = "csv";
  } else if ("read_json" == func || "read_jsonl" == func || "read_ndjson" == func) {
    default_format = "json";
  } else if ("read_parquet" == func) {
    default_format = "parquet";
  } else if (is_glob) {
    default_format = "";
  } else {
    ret = OB_ERR_UNEXPECTED;
  }
  std::string format = default_format;
  bool format_specified = false;

  for (size_t i = 1; OB_SUCC(ret) && i < args.size(); ++i) {
    const std::string arg = args[i];
    const size_t eq = arg.find('=');
    if (std::string::npos == eq) {
      // positional argument: only the format name is allowed
      std::string value;
      if (!unquote(arg, value)) {
        ret = OB_ERR_PARSE_SQL;
      } else {
        format = lower_copy(trim_copy(value));
        format_specified = true;
      }
    } else {
      const std::string key = lower_copy(trim_copy(arg.substr(0, eq)));
      const std::string raw = trim_copy(arg.substr(eq + 1));
      std::string value;
      const bool quoted = unquote(raw, value);
      if (!quoted) { value = raw; }
      if ("format" == key) {
        format = lower_copy(trim_copy(value));
        format_specified = true;
      } else if ("header" == key) {
        bool flag = true;
        if (!parse_bool_text(value, flag)) {
          ret = OB_ERR_PARSE_SQL;
        } else {
          opts.header_ = flag;
          opts.header_specified_ = true;
        }
      } else if ("sample_size" == key || "sample" == key) {
        char *end_ptr = NULL;
        const long long v = strtoll(value.c_str(), &end_ptr, 10);
        if (NULL == end_ptr || '\0' != *end_ptr) {
          ret = OB_ERR_PARSE_SQL;
        } else if (-1 != v) {
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "sample_size other than -1");
        } else {
          opts.sample_size_ = v;
        }
      } else if ("delim" == key || "delimiter" == key) {
        if (!value.empty()) { opts.delim_ = static_cast<int64_t>(static_cast<unsigned char>(value[0])); }
      } else if ("quote" == key) {
        if (!value.empty()) { opts.quote_ = static_cast<int64_t>(static_cast<unsigned char>(value[0])); }
      } else if ("escape" == key) {
        if (!value.empty()) { opts.escape_ = static_cast<int64_t>(static_cast<unsigned char>(value[0])); }
      } else if ("nullstr" == key || "null_str" == key) {
        // only meaningful for COPY export; ignored while reading
      } else if ("all_varchar" == key || "auto_detect" == key || "union_by_name" == key) {
        // accepted and ignored: schema inference is always deterministic
      } else {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "file SQL option");
      }
    }
  }

  if (OB_SUCC(ret)) {
    if (is_glob) {
      // no format for glob
    } else if ("read_csv" == func) {
      if (format_specified && "csv" != format && "auto" != format) {
        ret = OB_ERR_PARSE_SQL;
      }
      opts.format_.assign_ptr("csv", 3);
    } else if ("read_json" == func || "read_jsonl" == func || "read_ndjson" == func) {
      if (format_specified &&
          "json" != format && "newline_delimited" != format && "ndjson" != format) {
        ret = OB_ERR_PARSE_SQL;
      }
      opts.format_.assign_ptr("newline_delimited", 17);
    } else if ("read_parquet" == func) {
      if (format_specified && "parquet" != format) {
        ret = OB_ERR_PARSE_SQL;
      }
      opts.format_.assign_ptr("parquet", 7);
    }
  }
  return ret;
}

int copy_to_allocator(common::ObIAllocator &alloc, const std::string &text, common::ObString &out)
{
  int ret = OB_SUCCESS;
  if (text.empty()) {
    out.reset();
  } else {
    char *buf = static_cast<char *>(alloc.alloc(static_cast<int64_t>(text.size())));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("failed to allocate rewritten statement", K(ret), K(text.size()));
    } else {
      MEMCPY(buf, text.data(), static_cast<int64_t>(text.size()));
      out.assign_ptr(buf, static_cast<int32_t>(text.size()));
    }
  }
  return ret;
}

// Builds a UNION ALL statement describing the inferred file schema.
int build_describe(const common::ObIAllocator &allocator,
                   const common::ObIArray<sql::ObFileSqlColumn> &columns,
                   std::string &out)
{
  int ret = OB_SUCCESS;
  UNUSED(allocator);
  out += "select `column_name`, `column_type`, `null`, `key`, `default`, `extra` from (";
  if (columns.count() <= 0) {
    out += "select 1 as `__o`, 'id' as `column_name`, 'BIGINT' as `column_type`, "
           "'NO' as `null`, NULL as `key`, NULL as `default`, NULL as `extra`";
  }
  for (int64_t i = 0; i < columns.count(); ++i) {
    const sql::ObFileSqlColumn &col = columns.at(i);
    if (i > 0) { out += " union all "; }
    out += "select ";
    out += std::to_string(i + 1);
    out += " as `__o`, ";
    std::string name(col.name_.ptr(), static_cast<size_t>(col.name_.length()));
    append_sql_literal_escaped(out, name.data(), static_cast<int64_t>(name.size()));
    out += " as `column_name`, '";
    out += type_name(col.type_);
    out += "' as `column_type`, '";
    out += col.nullable_ ? "YES" : "NO";
    out += "' as `null`, NULL as `key`, NULL as `default`, NULL as `extra`";
  }
  out += ") `__file_sql_describe` order by `__o`";
  return ret;
}

bool statement_starts_with(const char *s, int64_t len, int64_t &pos, const char *kw)
{
  bool matched = false;
  int64_t p = pos;
  skip_blank(s, len, p);
  if (starts_with_ci(s + p, len - p, kw)) {
    const int64_t kw_len = static_cast<int64_t>(strlen(kw));
    if (p + kw_len >= len || !is_ident_char(s[p + kw_len])) {
      matched = true;
      pos = p + kw_len;
    }
  }
  return matched;
}

int rewrite_enable_flag_set(const char *s, int64_t len, int64_t pos, std::string &out, bool &changed)
{
  int ret = OB_SUCCESS;
  changed = false;
  int64_t p = pos;
  skip_blank(s, len, p);
  if (starts_with_ci(s + p, len - p, "session")) {
    p += 7;
    skip_blank(s, len, p);
  } else if (starts_with_ci(s + p, len - p, "global")) {
    p += 6;
    skip_blank(s, len, p);
  }
  if (starts_with_ci(s + p, len - p, "enable_file_sql")) {
    p += static_cast<int64_t>(strlen("enable_file_sql"));
    skip_blank(s, len, p);
    if (p < len && '=' == s[p]) {
      ++p;
    }
    const std::string value = trim_copy(std::string(s + p, static_cast<size_t>(len - p)));
    bool flag = false;
    if (!parse_bool_text(value, flag) && !value.empty() && ':' != value[0]) {
      // unknown text is treated as OFF, matching the boolean sysvar semantics
      flag = false;
    }
    out += flag ? "set @__enable_file_sql = 1" : "set @__enable_file_sql = 0";
    changed = true;
  }
  return ret;
}

// Replaces every @@[session.|local.]enable_file_sql reference.
void replace_enable_flag_reference(const char *s, int64_t len, std::string &out, bool &changed)
{
  int64_t pos = 0;
  while (pos < len) {
    const int64_t hit = find_keyword(s, len, pos, "enable_file_sql");
    if (hit < 0) {
      out.append(s + pos, static_cast<size_t>(len - pos));
      break;
    }
    int64_t name_pos = hit;
    while (name_pos > 0 && (s[name_pos - 1] == ' ' || s[name_pos - 1] == '\t')) { --name_pos; }
    int64_t begin = name_pos;
    bool qualified = false;
    if (name_pos >= 9 && s[name_pos - 1] == '.' &&
        starts_with_ci(s + name_pos - 8, 7, "session")) {
      begin = name_pos - 8;
      qualified = true;
    } else if (name_pos >= 7 && s[name_pos - 1] == '.' &&
               starts_with_ci(s + name_pos - 6, 5, "local")) {
      begin = name_pos - 6;
      qualified = true;
    }
    if (begin >= 2 && s[begin - 1] == '@' && s[begin - 2] == '@') {
      out.append(s + pos, static_cast<size_t>(begin - 2 - pos));
      out += "cast(ifnull(@__enable_file_sql, 0) as signed)";
      pos = hit + static_cast<int64_t>(strlen("enable_file_sql"));
      changed = true;
    } else if (qualified) {
      out.append(s + pos, static_cast<size_t>(begin - pos));
      pos = hit + static_cast<int64_t>(strlen("enable_file_sql"));
    } else {
      out.append(s + pos, static_cast<size_t>(hit + 15 - pos));
      pos = hit + static_cast<int64_t>(strlen("enable_file_sql"));
    }
  }
}

bool alias_follows(const char *s, int64_t len, int64_t pos);
int64_t find_top_level_keyword(const char *s, int64_t len, int64_t from, const char *kw);

// Rewrites the file table functions inside a single SELECT-like statement.
int rewrite_read_calls(common::ObIAllocator &alloc,
                       const common::ObString &secure_file_priv,
                       bool enabled,
                       const char *s,
                       int64_t len,
                       std::string &out,
                       bool &changed)
{
  int ret = OB_SUCCESS;
  static const char *const FUNCS[] = {
    "read_csv", "read_json", "read_jsonl", "read_parquet", "read_ndjson", "glob"
  };
  int64_t pos = 0;
  changed = false;
  while (OB_SUCC(ret) && pos < len) {
    int64_t hit = -1;
    for (size_t i = 0; i < sizeof(FUNCS) / sizeof(FUNCS[0]); ++i) {
      const int64_t found = find_keyword(s, len, pos, FUNCS[i]);
      if (found >= 0 && (hit < 0 || found < hit)) {
        hit = found;
      }
    }
    if (hit < 0) {
      out.append(s + pos, static_cast<size_t>(len - pos));
      break;
    }
    CallInfo call;
    if (!parse_call(s, len, hit, call)) {
      ret = OB_ERR_PARSE_SQL;
      LOG_WARN("failed to parse file table function call", K(ret));
    } else {
      std::vector<std::string> args;
      sql::ObFileSqlOptions opts;
      std::string path;
      bool is_glob = false;
      if (OB_FAIL(split_args(s, call.args_begin_, call.args_end_, args))) {
      } else if (OB_FAIL(interpret_read_args(args, call.name_, opts, path, is_glob))) {
        LOG_WARN("invalid file table function arguments", K(ret), K(call.name_.c_str()));
      } else if (!enabled) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "file SQL (enable_file_sql is off)");
      } else {
        common::ObString path_str;
        path_str.assign_ptr(path.data(), static_cast<int32_t>(path.size()));
        if (!sql::file_sql_path_allowed(secure_file_priv, path_str)) {
          ret = OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS;
          LOG_WARN("file path is not allowed by secure_file_priv", K(ret));
        } else {
          common::ObString json_str;
          common::ObSEArray<sql::ObFileSqlColumn, 16> columns;
          if (is_glob) {
            if (OB_FAIL(sql::glob_file_sql(alloc, path_str, json_str))) {
            } else {
              sql::ObFileSqlColumn col;
              col.name_.assign_ptr("file", 4);
              col.type_ = sql::FS_COL_VARCHAR;
              col.nullable_ = false;
              col.max_len_ = 1024;
              if (OB_FAIL(columns.push_back(col))) {
              }
            }
          } else {
            common::ObString format_out;
            if (OB_FAIL(sql::infer_file_sql_schema(alloc, path_str, opts, columns, format_out))) {
            } else if (OB_FAIL(sql::read_file_sql_json(alloc, path_str, opts, json_str))) {
            }
          }
          if (OB_SUCC(ret)) {
            std::string json_text(json_str.ptr(), static_cast<size_t>(json_str.length()));
            out.append(s + pos, static_cast<size_t>(hit - pos));
            build_json_table(out, json_text, columns);
            if (!alias_follows(s, len, call.end_pos_)) {
              out += " `__file_sql_t`";
            }
            pos = call.end_pos_;
            changed = true;
          }
        }
      }
    }
  }
  return ret;
}

int rewrite_describe(common::ObIAllocator &alloc,
                     const common::ObString &secure_file_priv,
                     bool enabled,
                     const char *s,
                     int64_t len,
                     int64_t pos,
                     std::string &out,
                     bool &changed)
{
  int ret = OB_SUCCESS;
  changed = false;
  int64_t p = pos;
  bool applicable = false;
  skip_blank(s, len, p);
  if (!starts_with_ci(s + p, len - p, "select")) {
    // ordinary DESCRIBE of a table, leave it untouched
  } else {
    p += 6;
    const int64_t from_pos = find_keyword(s, len, p, "from");
    if (from_pos < 0) {
    } else {
      int64_t q = from_pos + 4;
      skip_blank(s, len, q);
      CallInfo call;
      if (!parse_call(s, len, q, call)) {
      } else {
        static const char *const FUNCS[] = {
          "read_csv", "read_json", "read_jsonl", "read_parquet", "read_ndjson", "glob"
        };
        for (size_t i = 0; i < sizeof(FUNCS) / sizeof(FUNCS[0]); ++i) {
          if (call.name_ == FUNCS[i]) { applicable = true; }
        }
        if (applicable) {
          std::vector<std::string> args;
          sql::ObFileSqlOptions opts;
          std::string path;
          bool is_glob = false;
          if (OB_FAIL(split_args(s, call.args_begin_, call.args_end_, args))) {
          } else if (OB_FAIL(interpret_read_args(args, call.name_, opts, path, is_glob))) {
          } else if (!enabled) {
            ret = OB_NOT_SUPPORTED;
            LOG_USER_ERROR(OB_NOT_SUPPORTED, "file SQL (enable_file_sql is off)");
          } else {
            common::ObString path_str;
            path_str.assign_ptr(path.data(), static_cast<int32_t>(path.size()));
            if (!sql::file_sql_path_allowed(secure_file_priv, path_str)) {
              ret = OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS;
            } else {
              common::ObSEArray<sql::ObFileSqlColumn, 16> columns;
              if (is_glob) {
                sql::ObFileSqlColumn col;
                col.name_.assign_ptr("file", 4);
                col.type_ = sql::FS_COL_VARCHAR;
                col.nullable_ = false;
                col.max_len_ = 1024;
                if (OB_FAIL(columns.push_back(col))) {
                }
              } else {
                common::ObString format_out;
                if (OB_FAIL(sql::infer_file_sql_schema(alloc, path_str, opts, columns, format_out))) {
                }
              }
              if (OB_SUCC(ret)) {
                if (OB_FAIL(build_describe(alloc, columns, out))) {
                } else {
                  changed = true;
                }
              }
            }
          }
        }
      }
    }
  }
  return ret;
}

// Finds the next occurrence of 'kw' at parenthesis depth 0.
int64_t find_top_level_keyword(const char *s, int64_t len, int64_t from, const char *kw)
{
  const int64_t kw_len = static_cast<int64_t>(strlen(kw));
  int64_t pos = from;
  int64_t depth = 0;
  while (pos < len) {
    const char c = s[pos];
    if ('\'' == c || '"' == c || '`' == c) {
      if (!skip_quoted(s, len, pos)) {
        return -1;
      }
    } else if ('(' == c) {
      ++depth;
      ++pos;
    } else if (')' == c) {
      --depth;
      ++pos;
    } else if (0 == depth && starts_with_ci(s + pos, len - pos, kw)) {
      const bool left_ok = (pos == 0) || !is_ident_char(s[pos - 1]);
      const bool right_ok = (pos + kw_len >= len) || !is_ident_char(s[pos + kw_len]);
      if (left_ok && right_ok) {
        return pos;
      }
      ++pos;
    } else {
      ++pos;
    }
  }
  return -1;
}

// Derives the output column name of a select list item, matching the name that
// the SQL engine would assign to the corresponding derived-table column.
bool item_output_name(const std::string &item, std::string &name)
{
  bool ok = true;
  const std::string text = trim_copy(item);
  int64_t as_pos = -1;
  int64_t pos = 0;
  const int64_t len = static_cast<int64_t>(text.size());
  while (pos < len) {
    const char c = text[pos];
    if ('\'' == c || '"' == c || '`' == c) {
      if (!skip_quoted(text.data(), len, pos)) { break; }
    } else if (starts_with_ci(text.data() + pos, len - pos, "as")) {
      const bool left_ok = pos > 0 && (text[pos - 1] == ' ' || text[pos - 1] == '\t' ||
                                       text[pos - 1] == '\n' || text[pos - 1] == '\r');
      const bool right_ok = pos + 2 < len && (text[pos + 2] == ' ' || text[pos + 2] == '\t' ||
                                              text[pos + 2] == '\n' || text[pos + 2] == '\r');
      if (left_ok && right_ok) { as_pos = pos; }
      pos += 2;
    } else {
      ++pos;
    }
  }
  if (as_pos >= 0) {
    name = trim_copy(text.substr(as_pos + 2));
    if (!name.empty() && ('`' == name[0] || '\'' == name[0] || '"' == name[0])) {
      std::string unquoted;
      if (unquote(name, unquoted)) { name = unquoted; }
    }
  } else if (is_plain_column_name(text)) {
    name = text;
  } else {
    // qualified identifier: use the last component
    const size_t dot = text.rfind('.');
    const std::string tail = (std::string::npos == dot) ? text : text.substr(dot + 1);
    if (is_plain_column_name(tail)) {
      name = tail;
    } else {
      name = text;
    }
  }
  if (name.empty()) {
    ok = false;
  }
  return ok;
}

// Returns true when a table alias follows the given position.
bool alias_follows(const char *s, int64_t len, int64_t pos)
{
  bool follows = false;
  int64_t p = pos;
  skip_blank(s, len, p);
  if (p < len) {
    static const char *const STOP_WORDS[] = {
      "where", "order", "group", "having", "limit", "join", "inner", "left", "right",
      "full", "cross", "on", "using", "union", "natural", "into", "for", "window"
    };
    bool stop = (',' == s[p] || ')' == s[p] || ';' == s[p]);
    for (size_t i = 0; !stop && !follows && i < sizeof(STOP_WORDS) / sizeof(STOP_WORDS[0]); ++i) {
      if (starts_with_ci(s + p, len - p, STOP_WORDS[i])) {
        const int64_t kw_len = static_cast<int64_t>(strlen(STOP_WORDS[i]));
        if (p + kw_len >= len || !is_ident_char(s[p + kw_len])) {
          stop = true;
        }
      }
    }
    follows = !stop;
  }
  return follows;
}

int rewrite_copy(common::ObIAllocator &alloc,
                 const common::ObString &secure_file_priv,
                 bool enabled,
                 const char *s,
                 int64_t len,
                 int64_t pos,
                 std::string &out,
                 bool &changed)
{
  int ret = OB_SUCCESS;
  changed = false;
  int64_t p = pos;
  skip_blank(s, len, p);
  if (p < len && '(' != s[p]) {
    // COPY table TO ... is not supported, only query export
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "COPY of a table, only COPY (query) TO is supported");
    return ret;
  }
  if (p >= len) {
    ret = OB_ERR_PARSE_SQL;
    return ret;
  }
  // locate the matching close parenthesis of the query
  int64_t q_end = -1;
  int64_t i = p + 1;
  int64_t depth = 1;
  while (i < len && depth > 0) {
    const char c = s[i];
    if ('\'' == c || '"' == c || '`' == c) {
      if (!skip_quoted(s, len, i)) { break; }
    } else if ('(' == c) {
      ++depth;
      ++i;
    } else if (')' == c) {
      --depth;
      if (0 == depth) { q_end = i; }
      ++i;
    } else {
      ++i;
    }
  }
  int64_t to_pos = -1;
  if (q_end < 0) {
    ret = OB_ERR_PARSE_SQL;
  } else {
    to_pos = find_top_level_keyword(s, len, q_end + 1, "to");
    if (to_pos < 0) {
      ret = OB_ERR_PARSE_SQL;
    }
  }
  std::string path;
  bool header = false;
  std::string nullstr;
  if (OB_SUCC(ret)) {
    int64_t q = to_pos + 2;
    skip_blank(s, len, q);
    int64_t path_begin = q;
    if (q < len && ('\'' == s[q] || '"' == s[q])) {
      if (!skip_quoted(s, len, q)) {
        ret = OB_ERR_PARSE_SQL;
      } else {
        unquote(std::string(s + path_begin, static_cast<size_t>(q - path_begin)), path);
      }
    } else {
      ret = OB_ERR_PARSE_SQL;
    }
    // option list: (format csv, header, nullstr 'NULL')
    if (OB_SUCC(ret)) {
      int64_t opt_lparen = -1;
      {
        int64_t scan = q;
        while (scan < len) {
          const char c = s[scan];
          if ('\'' == c || '"' == c || '`' == c) {
            if (!skip_quoted(s, len, scan)) { break; }
          } else if ('(' == c) {
            opt_lparen = scan;
            break;
          } else {
            ++scan;
          }
        }
      }
      if (opt_lparen >= 0) {
        int64_t o = opt_lparen + 1;
        int64_t o_end = -1;
        int64_t d = 1;
        while (o < len && d > 0) {
          const char c = s[o];
          if ('\'' == c || '"' == c || '`' == c) {
            if (!skip_quoted(s, len, o)) { break; }
          } else if ('(' == c) {
            ++d;
            ++o;
          } else if (')' == c) {
            --d;
            if (0 == d) { o_end = o; }
            ++o;
          } else {
            ++o;
          }
        }
        std::vector<std::string> opts;
        if (o_end < 0) {
          ret = OB_ERR_PARSE_SQL;
        } else if (OB_FAIL(split_args(s, opt_lparen + 1, o_end, opts))) {
        } else {
          for (size_t k = 0; OB_SUCC(ret) && k < opts.size(); ++k) {
            const std::string raw = opts[k];
            const size_t eq = raw.find('=');
            std::string key;
            std::string value;
            if (std::string::npos == eq) {
              // DuckDB style options may separate the value with a blank: "format csv"
              const std::string trimmed = trim_copy(raw);
              const size_t blank = trimmed.find_first_of(" \t");
              if (std::string::npos != blank) {
                key = lower_copy(trim_copy(trimmed.substr(0, blank)));
                value = trim_copy(trimmed.substr(blank + 1));
                std::string unquoted;
                if (unquote(value, unquoted)) { value = unquoted; }
              } else {
                key = lower_copy(trimmed);
                value = "true";
              }
            } else {
              key = lower_copy(trim_copy(raw.substr(0, eq)));
              value = trim_copy(raw.substr(eq + 1));
              std::string unquoted;
              if (unquote(value, unquoted)) { value = unquoted; }
            }
            if ("format" == key || "type" == key) {
              const std::string fmt = lower_copy(trim_copy(value));
              if ("csv" != fmt) {
                ret = OB_NOT_SUPPORTED;
                LOG_USER_ERROR(OB_NOT_SUPPORTED, "COPY format other than csv");
              }
            } else if ("header" == key) {
              bool flag = true;
              if (!parse_bool_text(value, flag)) {
                ret = OB_ERR_PARSE_SQL;
              } else {
                header = flag;
              }
            } else if ("nullstr" == key || "null_str" == key || "null" == key) {
              nullstr = value;
            } else if ("delimiter" == key || "delim" == key || "sep" == key ||
                       "quote" == key || "escape" == key || "compression" == key) {
              // accepted and ignored, the exported shape uses the CSV defaults
            } else {
              ret = OB_NOT_SUPPORTED;
              LOG_USER_ERROR(OB_NOT_SUPPORTED, "COPY option");
            }
          }
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    common::ObString path_str;
    path_str.assign_ptr(path.data(), static_cast<int32_t>(path.size()));
    if (!file_sql_path_allowed(secure_file_priv, path_str)) {
      ret = OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS;
      LOG_WARN("copy target is not allowed by secure_file_priv", K(ret));
    }
  }
  // rewrite the nested query as well so COPY can read files directly
  std::string inner;
  if (OB_SUCC(ret)) {
    bool inner_changed = false;
    if (OB_FAIL(rewrite_read_calls(alloc, secure_file_priv, enabled,
                                   s + p + 1, q_end - p - 1, inner, inner_changed))) {
    }
  }
  std::vector<std::string> names;
  bool names_ok = true;
  if (OB_SUCC(ret) && header) {
    const int64_t select_pos = find_top_level_keyword(inner.data(),
                                                      static_cast<int64_t>(inner.size()), 0, "select");
    const int64_t from_pos = select_pos < 0 ? -1 :
        find_top_level_keyword(inner.data(), static_cast<int64_t>(inner.size()),
                               select_pos + 6, "from");
    if (select_pos < 0 || from_pos < 0) {
      names_ok = false;
    } else {
      std::vector<std::string> items;
      if (OB_FAIL(split_args(inner.data(), select_pos + 6, from_pos, items))) {
      } else {
        for (size_t k = 0; names_ok && k < items.size(); ++k) {
          std::string name;
          if (!item_output_name(items[k], name)) {
            names_ok = false;
          } else {
            names.push_back(name);
          }
        }
        if (items.empty()) { names_ok = false; }
      }
    }
  }
  if (OB_SUCC(ret)) {
    if (header && names_ok) {
      out += "select ";
      for (size_t k = 0; k < names.size(); ++k) {
        if (k > 0) { out += ", "; }
        append_quoted_ident(out, names[k]);
      }
      out += " from (select 0 as `__file_sql_ord`";
      for (size_t k = 0; k < names.size(); ++k) {
        out += ", ";
        append_sql_literal_escaped(out, names[k].data(), static_cast<int64_t>(names[k].size()));
        out += " as ";
        append_quoted_ident(out, names[k]);
      }
      out += " union all select 1 as `__file_sql_ord`";
      for (size_t k = 0; k < names.size(); ++k) {
        out += ", ";
        if (nullstr.empty()) {
          out += "cast(";
          append_quoted_ident(out, names[k]);
          out += " as char)";
        } else {
          out += "ifnull(cast(";
          append_quoted_ident(out, names[k]);
          out += " as char), ";
          append_sql_literal_escaped(out, nullstr.data(), static_cast<int64_t>(nullstr.size()));
          out += ")";
        }
        out += " as ";
        append_quoted_ident(out, names[k]);
      }
      out += " from (";
      out += inner;
      out += ") `__file_sql_copy_src`) `__file_sql_copy_all` order by `__file_sql_ord`";
    } else {
      out += "select * from (";
      out += inner;
      out += ") `__file_sql_copy_src`";
    }
    out += " into outfile ";
    append_sql_literal_escaped(out, path.data(), static_cast<int64_t>(path.size()));
    out += " fields terminated by ',' optionally enclosed by '\"' lines terminated by '\n'";
    changed = true;
  }
  return ret;
}

int rewrite_one_statement(common::ObIAllocator &alloc,
                          const common::ObString &secure_file_priv,
                          bool enabled,
                          const char *s,
                          int64_t len,
                          std::string &out,
                          bool &changed)
{
  int ret = OB_SUCCESS;
  changed = false;
  int64_t pos = 0;
  skip_blank(s, len, pos);
  if (statement_starts_with(s, len, pos, "copy")) {
    if (OB_FAIL(rewrite_copy(alloc, secure_file_priv, enabled, s, len, pos, out, changed))) {
    }
  } else if (statement_starts_with(s, len, pos, "set")) {
    int64_t probe = pos;
    skip_blank(s, len, probe);
    if (probe < len && '@' == s[probe]) {
      // ordinary user variable assignment
    } else if (OB_FAIL(rewrite_enable_flag_set(s, len, pos, out, changed))) {
    }
  } else if (statement_starts_with(s, len, pos, "describe") ||
             statement_starts_with(s, len, pos, "desc")) {
    if (OB_FAIL(rewrite_describe(alloc, secure_file_priv, enabled, s, len, pos, out, changed))) {
    }
  } else {
    std::string rewritten;
    bool replaced = false;
    if (OB_FAIL(rewrite_read_calls(alloc, secure_file_priv, enabled, s, len, rewritten, replaced))) {
    } else {
      std::string final_sql;
      bool flag_replaced = false;
      replace_enable_flag_reference(rewritten.data(),
                                    static_cast<int64_t>(rewritten.size()),
                                    final_sql,
                                    flag_replaced);
      if (replaced || flag_replaced) {
        out = final_sql;
        changed = true;
      }
    }
  }
  return ret;
}

bool statement_may_touch_files(const char *s, int64_t len)
{
  static const char *const KEYS[] = {
    "read_csv", "read_json", "read_jsonl", "read_parquet", "read_ndjson", "glob",
    "enable_file_sql", "copy"
  };
  bool found = false;
  for (size_t i = 0; !found && i < sizeof(KEYS) / sizeof(KEYS[0]); ++i) {
    if (find_keyword(s, len, 0, KEYS[i]) >= 0) {
      found = true;
    }
  }
  return found;
}

bool lookup_session_flag(sql::ObSQLSessionInfo &session)
{
  bool enabled = false;
  common::ObObj value;
  const common::ObString name(static_cast<int32_t>(strlen(ENABLE_FLAG_VAR)),
                              const_cast<char *>(ENABLE_FLAG_VAR));
  if (OB_SUCCESS == session.get_user_variable_value(name, value) && !value.is_null()) {
    if (value.is_integer_type()) {
      enabled = 0 != value.get_int();
    } else if (value.is_number() || value.is_double()) {
      enabled = 0.0 != value.get_double();
    } else if (value.is_string_type()) {
      bool parsed = false;
      std::string text(value.get_string().ptr(), static_cast<size_t>(value.get_string().length()));
      if (parse_bool_text(text, parsed)) {
        enabled = parsed;
      }
    }
  }
  return enabled;
}

// Errors raised on purpose while analysing a file SQL call must reach the user;
// anything else is an internal hiccup and the original text is executed as is.
bool file_sql_error_is_user_facing(int err)
{
  return OB_ERR_PARSE_SQL == err || OB_NOT_SUPPORTED == err ||
         OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS == err;
}

} // namespace

int rewrite_file_sql_statements(sql::ObSQLSessionInfo &session,
                                common::ObIAllocator &alloc,
                                common::ObIArray<common::ObString> &queries)
{
  int ret = OB_SUCCESS;
  common::ObString secure_file_priv;
  bool enabled = lookup_session_flag(session);
  const int priv_ret = session.get_secure_file_priv(secure_file_priv);
  if (OB_SUCCESS != priv_ret) {
    // Fail closed: without a trustworthy secure_file_priv nothing is rewritten
    // (read_csv() stays an unknown function) and the user statement is intact.
    LOG_WARN("skip file SQL rewrite: secure_file_priv is unavailable", K(priv_ret));
    return ret;
  } else if (secure_file_priv.empty()) {
    secure_file_priv.assign_ptr("/", 1);
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < queries.count(); ++i) {
    const common::ObString &sql = queries.at(i);
    if (sql.empty() || !statement_may_touch_files(sql.ptr(), sql.length())) {
    } else {
      std::string rewritten;
      bool changed = false;
      const int rewrite_ret = rewrite_one_statement(alloc, secure_file_priv, enabled,
                                                    sql.ptr(), sql.length(), rewritten, changed);
      if (OB_SUCCESS != rewrite_ret) {
        if (file_sql_error_is_user_facing(rewrite_ret)) {
          ret = rewrite_ret;
        } else {
          // An internal failure must not turn a valid statement into an error;
          // the original text is executed and reports its own diagnostics.
          LOG_WARN("ignore file SQL rewrite failure", K(rewrite_ret));
        }
      } else if (changed) {
        common::ObString new_sql;
        if (OB_FAIL(copy_to_allocator(alloc, rewritten, new_sql))) {
          LOG_WARN("ignore file SQL rewrite: allocation failed", K(ret));
          ret = OB_SUCCESS;
        } else {
          queries.at(i).assign(new_sql.ptr(), new_sql.length());
          LOG_TRACE("file SQL statement rewritten", K(sql), K(new_sql));
        }
      }
    }
  }
  return ret;
}

} // namespace observer
} // namespace oceanbase
