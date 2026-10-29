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
#include "observer/mysql/ob_ai_column_rewrite.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "common/mysqlclient/ob_isql_client.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "lib/rc/context.h"
#include "lib/string/ob_sql_string.h"
#include "lib/time/ob_time_utility.h"
#include "query/api/query/session/ob_inner_sql_connection_access.h"
#include "share/ob_errno.h"
#include "share/rc/ob_context.h"
#include "share/rc/ob_server_runtime.h"
#include "sql/session/ob_sql_session_info.h"

namespace oceanbase
{
namespace observer
{

namespace
{

const char *const STRICT_FLAG_VAR = "__ai_column_strict_read";
const char *const AI_TASK_TABLE = "oceanbase.__all_ai_column_task";
const char *const AI_CATALOG_VIEW = "oceanbase.__all_ai_column";
const char *const AI_COLUMN_VIEW = "oceanbase.__seekdb_all_column";
const char *const JSON_SYSTEM_PROMPT =
    "You are an AI assistant for database generated columns. Return exactly one valid JSON "
    "value and nothing else.";
const char *const INTEGER_SYSTEM_PROMPT =
    "You are an AI assistant for database generated columns. Return exactly one base-10 "
    "integer value and nothing else.";
const int64_t WORKER_BATCH_SIZE = 64;
const int64_t RETRY_DELAY_SECOND = 0;

char ai_lower_c(char c)
{
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool ai_starts_with_ci(const char *s, int64_t len, const char *kw)
{
  const int64_t kw_len = static_cast<int64_t>(strlen(kw));
  if (len < kw_len) {
    return false;
  }
  for (int64_t i = 0; i < kw_len; ++i) {
    if (ai_lower_c(s[i]) != ai_lower_c(kw[i])) {
      return false;
    }
  }
  return true;
}

bool ai_is_ident_char(char c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '$';
}

void ai_skip_blank(const char *s, int64_t len, int64_t &pos)
{
  while (pos < len) {
    const char c = s[pos];
    if (' ' == c || '\t' == c || '\n' == c || '\r' == c) {
      ++pos;
    } else if ('-' == c && pos + 1 < len && '-' == s[pos + 1]) {
      pos += 2;
      while (pos < len && '\n' != s[pos]) { ++pos; }
    } else if ('#' == c) {
      while (pos < len && '\n' != s[pos]) { ++pos; }
    } else if ('/' == c && pos + 1 < len && '*' == s[pos + 1]) {
      pos += 2;
      while (pos + 1 < len && !('/' == s[pos] && '*' == s[pos + 1])) { ++pos; }
      pos = (pos + 1 < len) ? pos + 2 : len;
    } else {
      break;
    }
  }
}

bool ai_skip_quoted(const char *s, int64_t len, int64_t &pos)
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

std::string ai_trim_copy(const std::string &in)
{
  size_t begin = 0;
  size_t end = in.size();
  while (begin < end) {
    const char c = in[begin];
    if (' ' == c || '\t' == c || '\n' == c || '\r' == c) { ++begin; } else { break; }
  }
  while (end > begin) {
    const char c = in[end - 1];
    if (' ' == c || '\t' == c || '\n' == c || '\r' == c) { --end; } else { break; }
  }
  return in.substr(begin, end - begin);
}

std::string ai_lower_copy(const std::string &in)
{
  std::string out(in);
  for (size_t i = 0; i < out.size(); ++i) {
    out[i] = ai_lower_c(out[i]);
  }
  return out;
}

bool equals_ci(const std::string &a, const std::string &b)
{
  return a.size() == b.size() && ai_lower_copy(a) == ai_lower_copy(b);
}

int64_t ai_find_keyword(const char *s, int64_t len, int64_t from, const char *kw)
{
  const int64_t kw_len = static_cast<int64_t>(strlen(kw));
  int64_t pos = from > 0 ? from : 0;
  while (pos < len) {
    const char c = s[pos];
    if ('\'' == c || '"' == c || '`' == c) {
      if (!ai_skip_quoted(s, len, pos)) {
        return -1;
      }
    } else if (ai_starts_with_ci(s + pos, len - pos, kw)) {
      const bool left_ok = (pos == 0) || !ai_is_ident_char(s[pos - 1]);
      const bool right_ok = (pos + kw_len >= len) || !ai_is_ident_char(s[pos + kw_len]);
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

int64_t ai_find_top_level_keyword(const char *s, int64_t len, int64_t from, const char *kw)
{
  const int64_t kw_len = static_cast<int64_t>(strlen(kw));
  int64_t pos = from > 0 ? from : 0;
  int64_t depth = 0;
  while (pos < len) {
    const char c = s[pos];
    if ('\'' == c || '"' == c || '`' == c) {
      if (!ai_skip_quoted(s, len, pos)) {
        return -1;
      }
    } else if ('(' == c) {
      ++depth;
      ++pos;
    } else if (')' == c) {
      --depth;
      ++pos;
    } else if (0 == depth && ai_starts_with_ci(s + pos, len - pos, kw)) {
      const bool left_ok = (pos == 0) || !ai_is_ident_char(s[pos - 1]);
      const bool right_ok = (pos + kw_len >= len) || !ai_is_ident_char(s[pos + kw_len]);
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

bool find_matching_paren(const char *s, int64_t len, int64_t open_pos, int64_t &close_pos)
{
  int64_t depth = 0;
  int64_t pos = open_pos;
  while (pos < len) {
    const char c = s[pos];
    if ('\'' == c || '"' == c || '`' == c) {
      if (!ai_skip_quoted(s, len, pos)) {
        return false;
      }
    } else if ('(' == c) {
      ++depth;
      ++pos;
    } else if (')' == c) {
      --depth;
      if (0 == depth) {
        close_pos = pos;
        return true;
      }
      ++pos;
    } else {
      ++pos;
    }
  }
  return false;
}

int ai_split_args(const char *s, int64_t begin, int64_t end, std::vector<std::string> &args)
{
  int ret = OB_SUCCESS;
  int64_t pos = begin;
  int64_t start = begin;
  int64_t depth = 0;
  while (OB_SUCC(ret) && pos < end) {
    const char c = s[pos];
    if ('\'' == c || '"' == c || '`' == c) {
      if (!ai_skip_quoted(s, end, pos)) {
        ret = OB_ERR_PARSE_SQL;
      }
    } else if ('(' == c) {
      ++depth;
      ++pos;
    } else if (')' == c) {
      --depth;
      ++pos;
    } else if (',' == c && 0 == depth) {
      args.push_back(ai_trim_copy(std::string(s + start, static_cast<size_t>(pos - start))));
      start = pos + 1;
      ++pos;
    } else {
      ++pos;
    }
  }
  if (OB_SUCC(ret)) {
    std::string last = ai_trim_copy(std::string(s + start, static_cast<size_t>(end - start)));
    if (!last.empty()) {
      args.push_back(last);
    }
  }
  return ret;
}

bool ai_unquote(const std::string &text, std::string &out)
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

bool ai_parse_bool_text(const std::string &text, bool &value)
{
  const std::string v = ai_lower_copy(ai_trim_copy(text));
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

std::string read_word_ci(const char *s, int64_t len, int64_t &pos)
{
  std::string word;
  ai_skip_blank(s, len, pos);
  const int64_t begin = pos;
  while (pos < len && ai_is_ident_char(s[pos])) { ++pos; }
  if (pos > begin) {
    word.assign(s + begin, static_cast<size_t>(pos - begin));
    for (size_t i = 0; i < word.size(); ++i) {
      word[i] = ai_lower_c(word[i]);
    }
  }
  return word;
}

bool parse_identifier(const char *s, int64_t len, int64_t &pos, std::string &out)
{
  out.clear();
  ai_skip_blank(s, len, pos);
  if (pos >= len) {
    return false;
  }
  if ('`' == s[pos]) {
    ++pos;
    while (pos < len) {
      if ('`' == s[pos]) {
        if (pos + 1 < len && '`' == s[pos + 1]) {
          out.push_back('`');
          pos += 2;
        } else {
          ++pos;
          break;
        }
      } else {
        out.push_back(s[pos]);
        ++pos;
      }
    }
    return !out.empty();
  }
  const int64_t begin = pos;
  while (pos < len && ai_is_ident_char(s[pos])) { ++pos; }
  if (pos == begin) {
    return false;
  }
  out.assign(s + begin, static_cast<size_t>(pos - begin));
  return true;
}

std::string sql_escape(const std::string &in)
{
  std::string out;
  out.reserve(in.size() + 8);
  for (size_t i = 0; i < in.size(); ++i) {
    const char c = in[i];
    if ('\'' == c) {
      out += "''";
    } else if ('\\' == c) {
      out += "\\\\";
    } else if ('\0' == c) {
      // drop embedded NUL bytes
    } else {
      out += c;
    }
  }
  return out;
}

std::string sql_str(const std::string &in)
{
  return std::string("'") + sql_escape(in) + "'";
}

std::string quote_ident(const std::string &name)
{
  std::string out("`");
  for (size_t i = 0; i < name.size(); ++i) {
    if ('`' == name[i]) {
      out += "``";
    } else {
      out += name[i];
    }
  }
  out += "`";
  return out;
}

std::string json_escape(const std::string &in)
{
  std::string out;
  out.reserve(in.size() + 8);
  char buf[8];
  for (size_t i = 0; i < in.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(in[i]);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned int>(c));
          out += buf;
        } else {
          out.push_back(static_cast<char>(c));
        }
        break;
    }
  }
  return out;
}

std::string hex_encode(const std::string &in)
{
  static const char *const DIGITS = "0123456789abcdef";
  std::string out;
  out.reserve(in.size() * 2);
  for (size_t i = 0; i < in.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(in[i]);
    out.push_back(DIGITS[c >> 4]);
    out.push_back(DIGITS[c & 0x0f]);
  }
  return out;
}

std::string make_request_id()
{
  static std::atomic<uint64_t> sequence(0);
  const uint64_t seq = sequence.fetch_add(1);
  const int64_t now = common::ObTimeUtility::current_time();
  char buf[64];
  snprintf(buf, sizeof(buf), "%llx%llx", static_cast<unsigned long long>(now),
           static_cast<unsigned long long>(seq));
  return std::string(buf);
}

std::string make_retry_delay()
{
  char buf[96];
  snprintf(buf, sizeof(buf), "date_add(now(6), interval 300000 microsecond)");
  return std::string(buf);
}

// ---------------------------------------------------------------------------
// metadata model
// ---------------------------------------------------------------------------

struct AiColumnSpec
{
  int64_t table_id_ = 0;
  int64_t column_id_ = 0;
  std::string column_;
  std::string model_;
  std::string kind_;        // complete / embed
  std::string result_type_; // json / int / varchar / vector
  std::string expr_;
  std::string system_prompt_;
  int64_t max_retries_ = 0;
  int64_t dim_ = 0;
};

struct AiTableMeta
{
  std::string db_;
  std::string table_;
  int64_t table_id_ = 0;
  std::vector<AiColumnSpec> cols_;
  std::vector<std::string> pks_;
  std::vector<bool> pk_is_string_;
};

std::string table_key(const std::string &db, const std::string &table)
{
  return db + "\x01" + table;
}

// ---------------------------------------------------------------------------
// inner SQL helper
// ---------------------------------------------------------------------------

class InnerSql
{
public:
  InnerSql() {}
  ~InnerSql() {}
  void set_session(sql::ObSQLSessionInfo *session) { session_ = session; }
  int connect()
  {
    int ret = OB_SUCCESS;
    if (OB_ISNULL(conn_)) {
      if (OB_NOT_NULL(session_)) {
        ret = query::ObInnerSQLConnectionAccess::create_connection_with_external_session(
            session_, guard_);
      } else {
        ret = common::create_inner_sql_connection_for_proxy(false, 0, guard_);
      }
      if (OB_FAIL(ret)) {
        LOG_WARN("failed to create inner sql connection", K(ret));
      } else if (OB_ISNULL(conn_ = guard_.get_ptr())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("inner sql connection is null", K(ret));
      }
    }
    return ret;
  }
  int write(const std::string &sql, int64_t &affected_rows)
  {
    int ret = OB_SUCCESS;
    affected_rows = 0;
    if (OB_FAIL(connect())) {
    } else {
      common::ObString stmt(static_cast<int32_t>(sql.size()), const_cast<char *>(sql.data()));
      if (OB_FAIL(conn_->execute_write(stmt, affected_rows))) {
        LOG_WARN("ai column inner sql write failed", K(ret), K(sql.c_str()));
      }
    }
    return ret;
  }
  int read(const std::string &sql, std::vector<std::vector<std::string>> &rows)
  {
    int ret = OB_SUCCESS;
    if (OB_FAIL(connect())) {
    } else {
      common::ObString stmt(static_cast<int32_t>(sql.size()), const_cast<char *>(sql.data()));
      common::ObISQLClient::ReadResult read_result;
      common::sqlclient::ObMySQLResult *result = nullptr;
      if (OB_FAIL(conn_->execute_read(stmt, read_result))) {
        LOG_WARN("ai column inner sql read failed", K(ret), K(sql.c_str()));
      } else if (OB_ISNULL(result = read_result.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        const int64_t column_count = result->get_column_count();
        while (OB_SUCC(ret) && OB_SUCC(result->next())) {
          std::vector<std::string> row;
          row.reserve(static_cast<size_t>(column_count));
          for (int64_t i = 0; OB_SUCC(ret) && i < column_count; ++i) {
            common::ObString value;
            if (OB_SUCCESS == result->get_varchar(i, value)) {
              row.push_back(value.empty()
                                ? std::string()
                                : std::string(value.ptr(), static_cast<size_t>(value.length())));
            } else {
              // get_varchar only accepts string columns; fall back to the
              // numeric accessors for integer and floating point results.
              int64_t int_val = 0;
              uint64_t uint_val = 0;
              double double_val = 0;
              if (OB_SUCCESS == result->get_int(i, int_val)) {
                row.push_back(std::to_string(int_val));
              } else if (OB_SUCCESS == result->get_uint(i, uint_val)) {
                row.push_back(std::to_string(uint_val));
              } else if (OB_SUCCESS == result->get_double(i, double_val)) {
                row.push_back(std::to_string(double_val));
              } else {
                row.push_back(std::string());
              }
            }
          }
          if (OB_SUCC(ret)) {
            rows.push_back(std::move(row));
          }
        }
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
        }
        const int close_ret = result->close();
        if (OB_SUCC(ret) && OB_FAIL(close_ret)) {
        }
      }
    }
    return ret;
  }
private:
  common::sqlclient::ObISQLConnectionGuard guard_;
  common::sqlclient::ObISQLConnection *conn_ = nullptr;
  sql::ObSQLSessionInfo *session_ = nullptr;
};

// ---------------------------------------------------------------------------
// AI column registry
// ---------------------------------------------------------------------------

class AiColumnRegistry
{
public:
  static AiColumnRegistry &instance()
  {
    static AiColumnRegistry registry;
    return registry;
  }

  int ensure_loaded()
  {
    int ret = OB_SUCCESS;
    std::lock_guard<std::mutex> guard(lock_);
    if (!loaded_) {
      if (OB_FAIL(bootstrap())) {
      } else if (OB_FAIL(load_all())) {
      } else {
        loaded_ = true;
      }
    }
    return ret;
  }

  bool has_any()
  {
    std::lock_guard<std::mutex> guard(lock_);
    return !tables_.empty();
  }

  bool is_ai_table(const std::string &db, const std::string &table)
  {
    std::lock_guard<std::mutex> guard(lock_);
    return tables_.end() != tables_.find(table_key(db, table));
  }

  // Returns null when the table has no AI columns.
  std::shared_ptr<const AiTableMeta> get(const std::string &db, const std::string &table)
  {
    std::shared_ptr<const AiTableMeta> result;
    std::lock_guard<std::mutex> guard(lock_);
    auto iter = tables_.find(table_key(db, table));
    if (tables_.end() != iter) {
      if (!iter->second || dirty_.end() != dirty_.find(iter->first)) {
        std::shared_ptr<AiTableMeta> loaded = std::make_shared<AiTableMeta>();
        if (OB_SUCCESS == load_table(db, table, *loaded)) {
          iter->second = loaded;
          dirty_.erase(iter->first);
        }
      }
      result = iter->second;
    }
    return result;
  }

  void mark_dirty(const std::string &db, const std::string &table)
  {
    std::lock_guard<std::mutex> guard(lock_);
    const std::string key = table_key(db, table);
    dirty_.insert(key);
    if (tables_.end() == tables_.find(key)) {
      tables_.emplace(key, nullptr);
    }
  }

  void erase(const std::string &db, const std::string &table)
  {
    std::lock_guard<std::mutex> guard(lock_);
    const std::string key = table_key(db, table);
    tables_.erase(key);
    dirty_.erase(key);
  }

  void erase_db(const std::string &db)
  {
    std::lock_guard<std::mutex> guard(lock_);
    const std::string prefix = db + "\x01";
    for (auto iter = tables_.begin(); iter != tables_.end();) {
      if (0 == iter->first.compare(0, prefix.size(), prefix)) {
        dirty_.erase(iter->first);
        iter = tables_.erase(iter);
      } else {
        ++iter;
      }
    }
  }

  std::vector<int64_t> table_ids_of_db(const std::string &db)
  {
    std::vector<int64_t> ids;
    std::lock_guard<std::mutex> guard(lock_);
    for (auto iter = tables_.begin(); iter != tables_.end(); ++iter) {
      if (iter->second && 0 != iter->second->table_id_) {
        const std::string prefix = db + "\x01";
        if (0 == iter->first.compare(0, prefix.size(), prefix)) {
          ids.push_back(iter->second->table_id_);
        }
      }
    }
    return ids;
  }

  void reload()
  {
    std::lock_guard<std::mutex> guard(lock_);
    loaded_ = false;
    tables_.clear();
    dirty_.clear();
  }

private:
  AiColumnRegistry() {}
  int bootstrap();
  int load_all();
  int load_table(const std::string &db, const std::string &table, AiTableMeta &meta);

  std::mutex lock_;
  bool loaded_ = false;
  std::map<std::string, std::shared_ptr<AiTableMeta>> tables_;
  std::set<std::string> dirty_;
};

int AiColumnRegistry::bootstrap()
{
  int ret = OB_SUCCESS;
  InnerSql sql;
  std::vector<std::vector<std::string>> ignored;
  std::string create_task =
      "create table if not exists oceanbase.__all_ai_column_task("
      "task_id bigint auto_increment primary key,"
      "table_id bigint not null,"
      "column_id bigint not null,"
      "database_name varchar(128) not null default '',"
      "table_name varchar(128) not null default '',"
      "column_name varchar(128) not null default '',"
      "rowkey_predicate varchar(1024) not null default '',"
      "request_id varchar(64) not null default '',"
      "state bigint not null default 0,"
      "retry_count bigint not null default 0,"
      "max_retries bigint not null default 0,"
      "eligible_ts datetime(6) not null default current_timestamp(6),"
      "result_type varchar(32) not null default '',"
      "expression longtext,"
      "system_prompt longtext,"
      "model_name varchar(128) not null default '',"
      "kind varchar(32) not null default '',"
      "dim bigint not null default 0,"
      "key idx_ai_column_task_table(table_id, state, eligible_ts))";
  const char *const doc = "convert(unhex(substr(c.comment,4)) using utf8mb4)";
  std::string create_catalog =
      std::string("create or replace view oceanbase.__all_ai_column as select "
                  "c.table_id as table_id, c.column_id as column_id, c.column_name as column_name, "
                  "t.table_name as table_name, d.database_name as database_name, "
                  "json_unquote(json_extract(") + doc + ",'$.model')) as model_name, "
                  "json_unquote(json_extract(" + doc + ",'$.kind')) as kind, "
                  "json_unquote(json_extract(" + doc + ",'$.type')) as result_type, "
                  "cast(ifnull(json_extract(" + doc + ",'$.max_retries'),0) as signed) as max_retries, "
                  "cast(ifnull(json_extract(" + doc + ",'$.dim'),0) as signed) as dim, "
                  "json_unquote(json_extract(" + doc + ",'$.expr')) as expression, "
                  "json_unquote(json_extract(" + doc + ",'$.sys')) as system_prompt, "
                  "case when json_unquote(json_extract(" + doc + ",'$.kind')) = 'complete' "
                  "then concat('AI_COMPLETE(', char(39), json_unquote(json_extract(" + doc + ",'$.model')), "
                  "char(39), ', ', json_unquote(json_extract(" + doc + ",'$.expr')), "
                  "case when ifnull(json_unquote(json_extract(" + doc + ",'$.sys')),'') = '' then '' "
                  "else concat(', ', char(39), '{\"system\": \"', json_unquote(json_extract(" + doc + ",'$.sys')), "
                  "'\"}', char(39)) end, char(39), ')') "
                  "else concat('AI_EMBED(', char(39), json_unquote(json_extract(" + doc + ",'$.model')), "
                  "char(39), ', ', json_unquote(json_extract(" + doc + ",'$.expr')), ')') end as normalized_expr, "
                  "js.column_id as state_column_id, jq.column_id as request_column_id, "
                  "jp.column_id as result_spec_column_id, c.comment as comment "
                  "from oceanbase.__all_column c "
                  "join oceanbase.__all_table t on t.table_id = c.table_id "
                  "join oceanbase.__all_database d on d.database_id = t.database_id "
                  "left join oceanbase.__all_column js on js.table_id = c.table_id "
                  "and js.column_name = concat(c.column_name,'$state') "
                  "left join oceanbase.__all_column jq on jq.table_id = c.table_id "
                  "and jq.column_name = concat(c.column_name,'$req') "
                  "left join oceanbase.__all_column jp on jp.table_id = c.table_id "
                  "and jp.column_name = concat(c.column_name,'$spec') "
                  "where c.comment like 'ai:%'";
  std::string create_columns =
      "create or replace view oceanbase.__seekdb_all_column as select "
      "gmt_create, gmt_modified, table_id, column_id, column_name, rowkey_position, "
      "index_position, order_in_rowkey, partition_key_position, data_type, data_length, "
      "data_precision, data_scale, zero_fill, nullable, on_update_current_timestamp, "
      "autoincrement, "
      "case when column_name like '%$state' or column_name like '%$req' "
      "or column_name like '%$spec' then 1 else is_hidden end as is_hidden, "
      "collation_type, orig_default_value, cur_default_value, comment, schema_version, "
      "column_flags, prev_column_id, extended_type_info, orig_default_value_v2, "
      "cur_default_value_v2, srs_id, udt_set_id, sub_data_type, skip_index_attr, "
      "lob_chunk_size, local_session_vars "
      "from oceanbase.__all_column";
  int64_t affected = 0;
  if (OB_FAIL(sql.write(create_task, affected))) {
    LOG_WARN("failed to create ai column task table", K(ret));
  } else {
    // The task table is created once per data directory; add any column that a
    // newer server version introduced so upgrades do not require a reset.
    struct TaskColumn
    {
      const char *name_;
      const char *definition_;
    };
    static const TaskColumn kTaskColumns[] = {
        {"table_id", "bigint not null"},
        {"column_id", "bigint not null"},
        {"database_name", "varchar(128) not null default ''"},
        {"table_name", "varchar(128) not null default ''"},
        {"column_name", "varchar(128) not null default ''"},
        {"rowkey_predicate", "varchar(1024) not null default ''"},
        {"request_id", "varchar(64) not null default ''"},
        {"state", "bigint not null default 0"},
        {"retry_count", "bigint not null default 0"},
        {"max_retries", "bigint not null default 0"},
        {"eligible_ts", "datetime(6) not null default current_timestamp(6)"},
        {"result_type", "varchar(32) not null default ''"},
        {"expression", "longtext"},
        {"system_prompt", "longtext"},
        {"model_name", "varchar(128) not null default ''"},
        {"kind", "varchar(32) not null default ''"},
        {"dim", "bigint not null default 0"}};
    std::vector<std::vector<std::string>> existing;
    const std::string probe =
        "select lower(column_name) from information_schema.columns where table_schema = "
        "'oceanbase' and table_name = '__all_ai_column_task'";
    if (OB_FAIL(sql.read(probe, existing))) {
      LOG_WARN("failed to inspect ai column task table", K(ret));
    } else {
      std::set<std::string> present;
      for (size_t i = 0; i < existing.size(); ++i) {
        if (!existing[i].empty()) {
          present.insert(existing[i][0]);
        }
      }
      for (size_t i = 0; OB_SUCC(ret) && i < sizeof(kTaskColumns) / sizeof(kTaskColumns[0]); ++i) {
        if (present.count(kTaskColumns[i].name_) > 0) {
          continue;
        }
        const std::string alter = std::string("alter table oceanbase.__all_ai_column_task add column ") +
                                  kTaskColumns[i].name_ + " " + kTaskColumns[i].definition_;
        if (OB_FAIL(sql.write(alter, affected))) {
          LOG_WARN("failed to add ai column task column", K(ret), K(alter.c_str()));
        }
      }
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(sql.write(create_catalog, affected))) {
    LOG_WARN("failed to create ai column catalog view", K(ret));
  } else if (OB_FAIL(sql.write(create_columns, affected))) {
    LOG_WARN("failed to create ai column shadow column view", K(ret));
  }
  return ret;
}

int AiColumnRegistry::load_all()
{
  int ret = OB_SUCCESS;
  InnerSql sql;
  std::vector<std::vector<std::string>> rows;
  std::string query = std::string("select database_name, table_name from ") + AI_CATALOG_VIEW +
                      " group by database_name, table_name";
  if (OB_FAIL(sql.read(query, rows))) {
    LOG_WARN("failed to load ai column tables", K(ret));
  } else {
    for (size_t i = 0; i < rows.size(); ++i) {
      if (rows[i].size() >= 2) {
        tables_.emplace(table_key(rows[i][0], rows[i][1]), nullptr);
      }
    }
  }
  return ret;
}

int AiColumnRegistry::load_table(const std::string &db, const std::string &table, AiTableMeta &meta)
{
  int ret = OB_SUCCESS;
  InnerSql sql;
  meta.db_ = db;
  meta.table_ = table;
  std::vector<std::vector<std::string>> rows;
  std::string query = std::string("select table_id, column_id, column_name, ifnull(model_name,''), "
                                  "ifnull(kind,''), ifnull(result_type,''), ifnull(expression,''), "
                                  "ifnull(system_prompt,''), max_retries, dim from ") + AI_CATALOG_VIEW +
                      " where database_name = " + sql_str(db) + " and table_name = " + sql_str(table) +
                      " order by column_id";
  if (OB_FAIL(sql.read(query, rows))) {
    LOG_WARN("failed to load ai column metadata", K(ret), K(db.c_str()), K(table.c_str()));
  } else {
    for (size_t i = 0; i < rows.size(); ++i) {
      if (rows[i].size() < 10) {
        continue;
      }
      AiColumnSpec spec;
      spec.table_id_ = atoll(rows[i][0].c_str());
      spec.column_id_ = atoll(rows[i][1].c_str());
      spec.column_ = rows[i][2];
      spec.model_ = rows[i][3];
      spec.kind_ = rows[i][4];
      spec.result_type_ = rows[i][5];
      spec.expr_ = rows[i][6];
      spec.system_prompt_ = rows[i][7];
      spec.max_retries_ = atoll(rows[i][8].c_str());
      spec.dim_ = atoll(rows[i][9].c_str());
      if (0 == meta.table_id_) {
        meta.table_id_ = spec.table_id_;
      }
      meta.cols_.push_back(spec);
    }
  }
  if (OB_SUCC(ret) && 0 != meta.table_id_) {
    std::vector<std::vector<std::string>> pk_rows;
    std::string pk_query = std::string("select column_name from oceanbase.__all_column where table_id = ") +
                           std::to_string(meta.table_id_) +
                           " and rowkey_position > 0 order by rowkey_position";
    if (OB_FAIL(sql.read(pk_query, pk_rows))) {
      LOG_WARN("failed to load primary key columns", K(ret), K(meta.table_id_));
    } else {
      for (size_t i = 0; i < pk_rows.size(); ++i) {
        if (!pk_rows[i].empty()) {
          meta.pks_.push_back(pk_rows[i][0]);
          meta.pk_is_string_.push_back(false);
        }
      }
    }
    if (OB_SUCC(ret) && !meta.pks_.empty()) {
      std::string type_query =
          std::string("select column_name, lower(ifnull(data_type,'')) from information_schema.columns "
                      "where table_schema = ") + sql_str(db) + " and table_name = " + sql_str(table);
      std::vector<std::vector<std::string>> type_rows;
      if (OB_FAIL(sql.read(type_query, type_rows))) {
        LOG_WARN("failed to load primary key types", K(ret), K(db.c_str()), K(table.c_str()));
      } else {
        for (size_t i = 0; i < type_rows.size(); ++i) {
          if (type_rows[i].size() < 2) {
            continue;
          }
          const std::string type = ai_lower_copy(type_rows[i][1]);
          const bool is_string = type.find("char") != std::string::npos ||
                                 type.find("text") != std::string::npos ||
                                 type.find("binary") != std::string::npos ||
                                 type.find("blob") != std::string::npos ||
                                 type.find("enum") != std::string::npos ||
                                 type.find("set") != std::string::npos;
          for (size_t j = 0; j < meta.pks_.size(); ++j) {
            if (equals_ci(meta.pks_[j], type_rows[i][0])) {
              meta.pk_is_string_[j] = is_string;
            }
          }
        }
      }
    }
  }
  if (OB_SUCC(ret) && meta.cols_.empty() && meta.pks_.empty()) {
    // not an AI table: nothing to do
  }
  return ret;
}

// ---------------------------------------------------------------------------
// session switch emulation
// ---------------------------------------------------------------------------

bool lookup_strict_flag(sql::ObSQLSessionInfo &session)
{
  bool enabled = false;
  common::ObObj value;
  const common::ObString name(static_cast<int32_t>(strlen(STRICT_FLAG_VAR)),
                              const_cast<char *>(STRICT_FLAG_VAR));
  if (OB_SUCCESS == session.get_user_variable_value(name, value) && !value.is_null()) {
    if (value.is_integer_type()) {
      enabled = 0 != value.get_int();
    } else if (value.is_number() || value.is_double()) {
      enabled = 0.0 != value.get_double();
    } else if (value.is_string_type()) {
      bool parsed = false;
      std::string text(value.get_string().ptr(), static_cast<size_t>(value.get_string().length()));
      if (ai_parse_bool_text(text, parsed)) {
        enabled = parsed;
      }
    }
  }
  return enabled;
}

bool rewrite_strict_set(const std::string &in, std::string &out, bool &changed)
{
  changed = false;
  const char *s = in.data();
  const int64_t len = static_cast<int64_t>(in.size());
  int64_t pos = 0;
  const std::string head = read_word_ci(s, len, pos);
  if (head != "set") {
    return false;
  }
  if (ai_find_keyword(s, len, pos, "ai_column_strict_read") < 0) {
    return false;
  }
  int64_t name_pos = ai_find_keyword(s, len, pos, "ai_column_strict_read");
  if (name_pos >= 2 && '@' == s[name_pos - 1]) {
    return false;
  }
  int64_t p = name_pos + static_cast<int64_t>(strlen("ai_column_strict_read"));
  ai_skip_blank(s, len, p);
  if (p < len && '=' == s[p]) {
    ++p;
  }
  const std::string value = ai_trim_copy(std::string(s + p, static_cast<size_t>(len - p)));
  bool flag = false;
  if (!ai_parse_bool_text(value, flag)) {
    flag = false;
  }
  out = flag ? "set @__ai_column_strict_read = 1" : "set @__ai_column_strict_read = 0";
  changed = true;
  return true;
}

void replace_strict_reference(const char *s, int64_t len, std::string &out, bool &changed)
{
  int64_t pos = 0;
  const int64_t kw_len = static_cast<int64_t>(strlen("ai_column_strict_read"));
  while (pos < len) {
    const int64_t hit = ai_find_keyword(s, len, pos, "ai_column_strict_read");
    if (hit < 0) {
      out.append(s + pos, static_cast<size_t>(len - pos));
      break;
    }
    int64_t name_pos = hit;
    int64_t begin = name_pos;
    bool qualified = false;
    if (name_pos >= 9 && '.' == s[name_pos - 1]) {
      if (ai_starts_with_ci(s + name_pos - 8, 7, "session")) {
        begin = name_pos - 8;
        qualified = true;
      } else if (name_pos >= 7 && ai_starts_with_ci(s + name_pos - 6, 5, "local")) {
        begin = name_pos - 6;
        qualified = true;
      }
    }
    if (begin >= 2 && '@' == s[begin - 1] && '@' == s[begin - 2]) {
      out.append(s + pos, static_cast<size_t>(begin - 2 - pos));
      out += "cast(ifnull(@__ai_column_strict_read, 0) as signed)";
      pos = hit + kw_len;
      changed = true;
    } else if (qualified) {
      out.append(s + pos, static_cast<size_t>(begin - pos));
      pos = hit + kw_len;
    } else {
      out.append(s + pos, static_cast<size_t>(hit + kw_len - pos));
      pos = hit + kw_len;
    }
  }
}

// ---------------------------------------------------------------------------
// DDL helpers
// ---------------------------------------------------------------------------

struct DdlColumn
{
  int64_t def_begin_ = 0;
  int64_t hit_ = 0;
  int64_t decl_end_ = 0;
  std::string prefix_;
  std::string name_;
  std::string type_text_;
  std::string expr_;
  std::string model_;
  std::string kind_;
  std::string result_type_;
  std::string sys_prompt_;
  std::string spec_;
  int64_t dim_ = 0;
  int64_t max_retries_ = 0;
  bool has_with_ = false;
};

bool parse_column_prefix(const std::string &raw, bool is_alter, std::string &name,
                         std::string &type_text)
{
  const std::string prefix = ai_trim_copy(raw);
  const char *s = prefix.data();
  const int64_t len = static_cast<int64_t>(prefix.size());
  int64_t pos = 0;
  if (is_alter) {
    const std::string first = read_word_ci(s, len, pos);
    if ("add" == first || "modify" == first) {
      const int64_t save = pos;
      const std::string second = read_word_ci(s, len, pos);
      if ("column" != second) {
        pos = save;
      }
    } else {
      return false;
    }
  }
  if (!parse_identifier(s, len, pos, name)) {
    return false;
  }
  type_text = ai_trim_copy(prefix.substr(static_cast<size_t>(pos)));
  return !type_text.empty();
}

bool parse_type_text(const std::string &type_text, std::string &result_type, int64_t &dim)
{
  const std::string text = ai_lower_copy(ai_trim_copy(type_text));
  size_t end = 0;
  while (end < text.size() && !isspace(static_cast<unsigned char>(text[end])) && text[end] != '(') {
    ++end;
  }
  const std::string base = text.substr(0, end);
  bool ok = true;
  result_type.clear();
  dim = 0;
  if ("json" == base) {
    result_type = "json";
  } else if ("vector" == base) {
    result_type = "vector";
    const size_t lparen = text.find('(');
    const size_t rparen = text.find(')', lparen == std::string::npos ? 0 : lparen);
    if (std::string::npos == lparen || std::string::npos == rparen) {
      ok = false;
    } else {
      const std::string inside = ai_trim_copy(text.substr(lparen + 1, rparen - lparen - 1));
      dim = atoll(inside.c_str());
      if (dim <= 0) {
        ok = false;
      }
    }
  } else if ("bigint" == base || "int" == base || "integer" == base || "smallint" == base ||
             "tinyint" == base || "mediumint" == base) {
    result_type = "int";
  } else if ("varchar" == base || "char" == base || "text" == base || "longtext" == base ||
             "mediumtext" == base) {
    result_type = "varchar";
  } else {
    ok = false;
  }
  return ok;
}

bool parse_ai_call(const std::string &expr, std::string &func, std::vector<std::string> &args)
{
  const char *s = expr.data();
  const int64_t len = static_cast<int64_t>(expr.size());
  int64_t pos = 0;
  std::string name;
  if (!parse_identifier(s, len, pos, name)) {
    return false;
  }
  ai_skip_blank(s, len, pos);
  if (pos >= len || '(' != s[pos]) {
    return false;
  }
  int64_t close = 0;
  if (!find_matching_paren(s, len, pos, close)) {
    return false;
  }
  std::vector<std::string> tmp;
  if (OB_SUCCESS != ai_split_args(s, pos + 1, close, tmp)) {
    return false;
  }
  func = ai_lower_copy(name);
  args.swap(tmp);
  return true;
}

std::string build_meta_json(const DdlColumn &col)
{
  std::string json = "{";
  json += "\"model\":\"" + json_escape(col.model_) + "\",";
  json += "\"kind\":\"" + col.kind_ + "\",";
  json += "\"type\":\"" + col.result_type_ + "\",";
  json += "\"dim\":" + std::to_string(col.dim_) + ",";
  json += "\"max_retries\":" + std::to_string(col.max_retries_) + ",";
  json += "\"expr\":\"" + json_escape(col.expr_) + "\",";
  json += "\"sys\":\"" + json_escape(col.sys_prompt_) + "\"";
  json += "}";
  return json;
}

// Finds "ai column" declarations at the definition level of a CREATE/ALTER statement.
int scan_ai_columns(const char *s, int64_t len, bool create_mode, int64_t scan_from,
                    std::vector<DdlColumn> &cols)
{
  int ret = OB_SUCCESS;
  const int64_t level = create_mode ? 1 : 0;
  int64_t pos = scan_from;
  int64_t depth = 0;
  int64_t seg_begin = scan_from;
  bool in_columns = !create_mode;
  while (OB_SUCC(ret) && pos < len) {
    const char c = s[pos];
    if ('\'' == c || '"' == c || '`' == c) {
      if (!ai_skip_quoted(s, len, pos)) {
        ret = OB_ERR_PARSE_SQL;
      }
    } else if ('(' == c) {
      ++depth;
      ++pos;
      if (create_mode && 1 == depth && !in_columns) {
        in_columns = true;
        seg_begin = pos;
      }
    } else if (')' == c) {
      --depth;
      ++pos;
    } else if (',' == c && depth == level) {
      ++pos;
      seg_begin = pos;
    } else if (ai_is_ident_char(c) && (pos == 0 || !ai_is_ident_char(s[pos - 1])) &&
               depth == level && ai_starts_with_ci(s + pos, len - pos, "ai")) {
      int64_t p = pos + 2;
      if (p < len && !ai_is_ident_char(s[p])) {
        ai_skip_blank(s, len, p);
        if (p + 6 <= len && ai_starts_with_ci(s + p, len - p, "column") &&
            (p + 6 >= len || !ai_is_ident_char(s[p + 6]))) {
          int64_t q = p + 6;
          ai_skip_blank(s, len, q);
          int64_t close = 0;
          if (q >= len || '(' != s[q] || !find_matching_paren(s, len, q, close)) {
            ret = OB_ERR_PARSE_SQL;
            LOG_WARN("malformed AI COLUMN declaration", K(ret));
          } else {
            DdlColumn col;
            col.def_begin_ = seg_begin;
            col.hit_ = pos;
            col.prefix_ = std::string(s + seg_begin, static_cast<size_t>(pos - seg_begin));
            col.expr_ = ai_trim_copy(std::string(s + q + 1, static_cast<size_t>(close - q - 1)));
            col.decl_end_ = close + 1;
            int64_t r = close + 1;
            ai_skip_blank(s, len, r);
            if (r + 4 <= len && ai_starts_with_ci(s + r, len - r, "with") &&
                (r + 4 >= len || !ai_is_ident_char(s[r + 4]))) {
              int64_t wp = r + 4;
              ai_skip_blank(s, len, wp);
              int64_t wclose = 0;
              if (wp < len && '(' == s[wp] && find_matching_paren(s, len, wp, wclose)) {
                col.has_with_ = true;
                const std::string options(s + wp + 1, static_cast<size_t>(wclose - wp - 1));
                std::vector<std::string> pairs;
                if (OB_FAIL(ai_split_args(s + wp + 1, 0, static_cast<int64_t>(options.size()), pairs))) {
                  LOG_WARN("failed to parse AI COLUMN options", K(ret));
                } else {
                  for (size_t i = 0; OB_SUCC(ret) && i < pairs.size(); ++i) {
                    const size_t eq = pairs[i].find('=');
                    if (std::string::npos == eq) {
                      continue;
                    }
                    const std::string key = ai_lower_copy(ai_trim_copy(pairs[i].substr(0, eq)));
                    const std::string val = ai_trim_copy(pairs[i].substr(eq + 1));
                    if ("max_retries" == key) {
                      col.max_retries_ = atoll(val.c_str());
                    } else if ("sync_mode" == key) {
                      std::string mode;
                      if (ai_unquote(val, mode)) {
                        mode = ai_lower_copy(mode);
                      } else {
                        mode = ai_lower_copy(val);
                      }
                      if ("async" != mode) {
                        ret = OB_NOT_SUPPORTED;
                        LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN sync mode");
                      }
                    }
                  }
                }
                col.decl_end_ = wclose + 1;
              }
            }
            if (OB_SUCC(ret)) {
              cols.push_back(col);
              pos = col.decl_end_;
              seg_begin = pos;
            }
          }
        } else {
          ++pos;
        }
      } else {
        ++pos;
      }
    } else {
      ++pos;
    }
  }
  return ret;
}

int validate_and_fill_ddl_column(DdlColumn &col, const std::vector<std::string> &ai_names,
                                 bool is_alter)
{
  int ret = OB_SUCCESS;
  std::string type_text;
  if (!parse_column_prefix(col.prefix_, is_alter, col.name_, type_text)) {
    ret = OB_ERR_PARSE_SQL;
    LOG_WARN("failed to parse AI COLUMN definition", K(ret), K(col.prefix_.c_str()));
  } else if (!parse_type_text(type_text, col.result_type_, col.dim_)) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN result type");
  } else {
    std::string func;
    std::vector<std::string> args;
    if (!parse_ai_call(col.expr_, func, args) || args.size() < 2) {
      ret = OB_ERR_PARSE_SQL;
      LOG_WARN("failed to parse AI COLUMN expression", K(ret), K(col.expr_.c_str()));
    } else if ("ai_complete" != func && "ai_embed" != func) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN function");
    } else if (!ai_unquote(args[0], col.model_)) {
      ret = OB_ERR_PARSE_SQL;
      LOG_WARN("AI COLUMN model name must be a string literal", K(ret));
    } else {
      col.expr_ = args[1];
      col.kind_ = ("ai_embed" == func) ? "embed" : "complete";
      int64_t requested_dim = 0;
      if ("ai_embed" == func && args.size() >= 3) {
        requested_dim = atoll(args[2].c_str());
      }
      if ("embed" == col.kind_ && "vector" != col.result_type_) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI_EMBED on a non vector column");
      } else if ("complete" == col.kind_ && "vector" == col.result_type_) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI_COMPLETE on a vector column");
      } else if ("embed" == col.kind_ && requested_dim > 0 && requested_dim != col.dim_) {
        ret = OB_ERR_INVALID_VECTOR_DIM;
        LOG_USER_ERROR(OB_ERR_INVALID_VECTOR_DIM, static_cast<uint32_t>(col.dim_),
                       static_cast<uint32_t>(requested_dim));
      } else if ("embed" == col.kind_) {
        col.dim_ = (requested_dim > 0) ? requested_dim : col.dim_;
      }
    }
    // an AI column cannot be derived from another AI column of the same table
    if (OB_SUCC(ret)) {
      const char *e = col.expr_.data();
      const int64_t elen = static_cast<int64_t>(col.expr_.size());
      int64_t pos = 0;
      while (OB_SUCC(ret) && pos < elen) {
        const char c = e[pos];
        if ('\'' == c || '"' == c) {
          ai_skip_quoted(e, elen, pos);
        } else if (ai_is_ident_char(c) && (pos == 0 || !ai_is_ident_char(e[pos - 1]))) {
          const int64_t begin = pos;
          while (pos < elen && ai_is_ident_char(e[pos])) { ++pos; }
          const std::string ident(e + begin, static_cast<size_t>(pos - begin));
          for (size_t i = 0; OB_SUCC(ret) && i < ai_names.size(); ++i) {
            if (equals_ci(ident, ai_names[i])) {
              ret = OB_NOT_SUPPORTED;
            }
          }
        } else {
          ++pos;
        }
      }
    }
    if (OB_SUCC(ret)) {
      if ("complete" == col.kind_) {
        if ("json" == col.result_type_) {
          col.sys_prompt_ = JSON_SYSTEM_PROMPT;
        } else if ("int" == col.result_type_) {
          col.sys_prompt_ = INTEGER_SYSTEM_PROMPT;
        }
        col.spec_ = std::string("AI_COMPLETE(") + sql_str(col.model_) + ", " + col.expr_;
        if (!col.sys_prompt_.empty()) {
          col.spec_ += ", " + sql_str(std::string("{\"system\":\"") + json_escape(col.sys_prompt_) + "\"}");
        }
        col.spec_ += ")";
      } else {
        col.spec_ = std::string("AI_EMBED(") + sql_str(col.model_) + ", " + col.expr_;
        if (col.dim_ > 0) {
          col.spec_ += ", " + std::to_string(col.dim_);
        }
        col.spec_ += ")";
      }
    }
  }
  return ret;
}

int build_ddl_replacement(const DdlColumn &col, bool is_alter, std::string &replacement)
{
  int ret = OB_SUCCESS;
  const std::string meta = build_meta_json(col);
  const std::string hex = hex_encode(meta);
  std::string prefix = ai_trim_copy(col.prefix_);
  if (is_alter) {
    // keep the original "add column" text as written by the user
    prefix = col.prefix_;
  }
  replacement = prefix + " comment 'ai:" + hex + "'";
  const std::string add_prefix = is_alter ? ", add column " : ", ";
  replacement += add_prefix + quote_ident(col.name_ + "$state") + " int";
  replacement += add_prefix + quote_ident(col.name_ + "$req") + " varchar(64)";
  replacement += add_prefix + quote_ident(col.name_ + "$spec") + " longtext";
  return ret;
}

// ---------------------------------------------------------------------------
// statement utilities
// ---------------------------------------------------------------------------

std::string strip_statement(const std::string &in)
{
  std::string text = ai_trim_copy(in);
  while (!text.empty() && ';' == text[text.size() - 1]) {
    text = ai_trim_copy(text.substr(0, text.size() - 1));
  }
  return text;
}

// Collects every identifier token of a statement, skipping string literals.
void collect_identifiers(const std::string &text, std::vector<std::string> &out)
{
  const char *s = text.data();
  const int64_t len = static_cast<int64_t>(text.size());
  int64_t pos = 0;
  while (pos < len) {
    const char c = s[pos];
    if ('\'' == c || '"' == c) {
      if (!ai_skip_quoted(s, len, pos)) {
        break;
      }
    } else if ('`' == c) {
      std::string name;
      if (parse_identifier(s, len, pos, name)) {
        out.push_back(name);
      } else {
        ++pos;
      }
    } else if (ai_is_ident_char(c)) {
      const int64_t begin = pos;
      while (pos < len && ai_is_ident_char(s[pos])) { ++pos; }
      out.push_back(std::string(s + begin, static_cast<size_t>(pos - begin)));
    } else {
      ++pos;
    }
  }
}

bool contains_identifier(const std::vector<std::string> &idents, const std::string &name)
{
  for (size_t i = 0; i < idents.size(); ++i) {
    if (equals_ci(idents[i], name)) {
      return true;
    }
  }
  return false;
}

bool parse_qualified_name(const char *s, int64_t len, int64_t &pos,
                          std::string &db, std::string &table)
{
  std::string first;
  db.clear();
  table.clear();
  if (!parse_identifier(s, len, pos, first)) {
    return false;
  }
  const int64_t save = pos;
  ai_skip_blank(s, len, pos);
  if (pos < len && '.' == s[pos]) {
    int64_t next_pos = pos + 1;
    std::string next;
    if (parse_identifier(s, len, next_pos, next)) {
      db = first;
      table = next;
      pos = next_pos;
      return true;
    }
  }
  pos = save;
  table = first;
  return true;
}

std::string session_default_db(sql::ObSQLSessionInfo &session)
{
  std::string db;
  common::ObString name = session.get_database_name();
  if (!name.empty()) {
    db.assign(name.ptr(), static_cast<size_t>(name.length()));
  }
  return db;
}

std::string qualified_name(const std::string &db, const std::string &table)
{
  return quote_ident(db) + "." + quote_ident(table);
}

std::string meta_qualified(const AiTableMeta &meta)
{
  return qualified_name(meta.db_, meta.table_);
}

// Renders the "`id` = 10" style predicate text of the current row.
std::string build_predicate_expr(const AiTableMeta &meta)
{
  if (meta.pks_.empty()) {
    return std::string("''");
  }
  std::string expr = "concat(";
  for (size_t i = 0; i < meta.pks_.size(); ++i) {
    if (i > 0) {
      expr += ", ";
    }
    std::string label = "`" + meta.pks_[i] + "` = ";
    if (i > 0) {
      label = " and `" + meta.pks_[i] + "` = ";
    }
    expr += sql_str(label) + ", ";
    const bool is_string = i < meta.pk_is_string_.size() && meta.pk_is_string_[i];
    if (is_string) {
      expr += "ifnull(quote(" + quote_ident(meta.pks_[i]) + "), 'NULL')";
    } else {
      expr += "ifnull(cast(" + quote_ident(meta.pks_[i]) + " as char), 'NULL')";
    }
  }
  expr += ")";
  return expr;
}

std::string build_status_expr(const AiColumnSpec &col)
{
  const std::string value = quote_ident(col.column_);
  const std::string state = quote_ident(col.column_ + "$state");
  return "case when " + value + " is not null then 'READY' when " + state +
         " = 2 then 'FAILED' else 'PENDING' end";
}

// Replaces bare (unqualified, unquoted) occurrences of the retry-only STATUS
// pseudo column with its materialized state expression.
void substitute_status(const std::string &cond, const std::string &pseudo, std::string &out)
{
  const char *s = cond.data();
  const int64_t len = static_cast<int64_t>(cond.size());
  int64_t pos = 0;
  out.clear();
  while (pos < len) {
    const char c = s[pos];
    if ('\'' == c || '"' == c) {
      const int64_t begin = pos;
      if (!ai_skip_quoted(s, len, pos)) {
        out.append(s + begin, static_cast<size_t>(len - begin));
        break;
      }
      out.append(s + begin, static_cast<size_t>(pos - begin));
    } else if ('`' == c) {
      const int64_t begin = pos;
      std::string name;
      if (parse_identifier(s, len, pos, name)) {
        out.append(s + begin, static_cast<size_t>(pos - begin));
      } else {
        out.push_back(c);
        ++pos;
      }
    } else if (ai_is_ident_char(c)) {
      const int64_t begin = pos;
      while (pos < len && ai_is_ident_char(s[pos])) { ++pos; }
      const std::string name(s + begin, static_cast<size_t>(pos - begin));
      if (equals_ci(name, "status")) {
        int64_t prev = begin - 1;
        while (prev >= 0 && (s[prev] == ' ' || s[prev] == '\t' || s[prev] == '\n')) { --prev; }
        if (prev >= 0 && '.' == s[prev]) {
          out += name;
        } else {
          out += pseudo;
        }
      } else {
        out += name;
      }
    } else {
      out.push_back(c);
      ++pos;
    }
  }
}

// Rewrites oceanbase.__all_column into the shadow view that marks the hidden
// helper columns of AI columns.
void replace_all_column_reference(const std::string &in, std::string &out, bool &changed)
{
  changed = false;
  const char *s = in.data();
  const int64_t len = static_cast<int64_t>(in.size());
  int64_t pos = 0;
  out.clear();
  while (pos < len) {
    const char c = s[pos];
    if ('\'' == c || '"' == c) {
      const int64_t begin = pos;
      if (!ai_skip_quoted(s, len, pos)) {
        out.append(s + begin, static_cast<size_t>(len - begin));
        break;
      }
      out.append(s + begin, static_cast<size_t>(pos - begin));
    } else if ('`' == c) {
      const int64_t begin = pos;
      std::string name;
      if (parse_identifier(s, len, pos, name)) {
        if (equals_ci(name, "__all_column")) {
          out += "`__seekdb_all_column`";
          changed = true;
        } else {
          out.append(s + begin, static_cast<size_t>(pos - begin));
        }
      } else {
        out.push_back(c);
        ++pos;
      }
    } else if (ai_is_ident_char(c)) {
      const int64_t begin = pos;
      while (pos < len && ai_is_ident_char(s[pos])) { ++pos; }
      const std::string name(s + begin, static_cast<size_t>(pos - begin));
      if (equals_ci(name, "__all_column")) {
        out += "__seekdb_all_column";
        changed = true;
      } else {
        out += name;
      }
    } else {
      out.push_back(c);
      ++pos;
    }
  }
}

int ai_copy_to_allocator(common::ObIAllocator &alloc, const std::string &text, common::ObString &out)
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

// ---------------------------------------------------------------------------
// request scoped auxiliary statements
// ---------------------------------------------------------------------------

struct AiColumnRegistryAction
{
  bool erase_ = false;
  std::string db_;
  std::string table_;
};

struct AiColumnRequestContext
{
  std::vector<std::vector<std::string>> posts_;
  std::vector<std::vector<AiColumnRegistryAction>> actions_;
};

AiColumnRequestContext &ai_column_ctx()
{
  static thread_local AiColumnRequestContext ctx;
  return ctx;
}

std::shared_ptr<const AiTableMeta> get_table_meta(const std::string &db, const std::string &table)
{
  AiColumnRegistry &registry = AiColumnRegistry::instance();
  if (OB_SUCCESS != registry.ensure_loaded()) {
    LOG_WARN_RET(OB_ERR_UNEXPECTED, "failed to load AI column registry");
  }
  return registry.get(db, table);
}

// ---------------------------------------------------------------------------
// strict read
// ---------------------------------------------------------------------------

int check_strict_read(sql::ObSQLSessionInfo &session,
                      const std::string &text,
                      const AiTableMeta &meta)
{
  int ret = OB_SUCCESS;
  if (meta.cols_.empty() || !lookup_strict_flag(session)) {
    return ret;
  }
  std::vector<std::string> idents;
  collect_identifiers(text, idents);
  for (size_t i = 0; OB_SUCC(ret) && i < meta.cols_.size(); ++i) {
    const AiColumnSpec &col = meta.cols_[i];
    if (!contains_identifier(idents, col.column_)) {
      continue;
    }
    InnerSql sql;
    sql.set_session(&session);
    std::vector<std::vector<std::string>> rows;
    const std::string count_sql = "select count(*) from " + meta_qualified(meta) +
                                  " where " + quote_ident(col.column_) + " is null";
    if (OB_FAIL(sql.read(count_sql, rows))) {
      LOG_WARN("failed to check unfinished AI column tasks", K(ret), K(count_sql.c_str()));
    } else if (!rows.empty() && !rows[0].empty() && "0" != rows[0][0]) {
      LOG_USER_ERROR(OB_AI_COLUMN_UNFINISHED_TASK, meta.table_id_, col.column_id_);
      ret = OB_AI_COLUMN_UNFINISHED_TASK;
    }
  }
  return ret;
}

int build_task_insert(const AiTableMeta &meta,
                      const AiColumnSpec &col,
                      const std::string &predicate_expr,
                      const std::string &rid,
                      const std::string &where_sql,
                      std::string &sql_text)
{
  int ret = OB_SUCCESS;
  sql_text = "insert into oceanbase.__all_ai_column_task(table_id, column_id, database_name, "
             "table_name, column_name, rowkey_predicate, request_id, state, retry_count, "
             "max_retries, result_type, expression, system_prompt, model_name, kind, dim) select ";
  sql_text += std::to_string(meta.table_id_) + ", " + std::to_string(col.column_id_) + ", ";
  sql_text += sql_str(meta.db_) + ", " + sql_str(meta.table_) + ", " + sql_str(col.column_) + ", ";
  sql_text += predicate_expr + ", " + sql_str(rid) + ", 0, 0, " +
              std::to_string(col.max_retries_) + ", ";
  sql_text += sql_str(col.result_type_) + ", " + sql_str(col.expr_) + ", " +
              sql_str(col.system_prompt_) + ", ";
  sql_text += sql_str(col.model_) + ", " + sql_str(col.kind_) + ", " +
              std::to_string(col.dim_);
  sql_text += " from " + meta_qualified(meta) + " where " + where_sql;
  return ret;
}

const AiColumnSpec *find_ai_column(const AiTableMeta &meta, const std::string &name)
{
  const AiColumnSpec *found = nullptr;
  for (size_t i = 0; i < meta.cols_.size(); ++i) {
    if (equals_ci(meta.cols_[i].column_, name)) {
      found = &meta.cols_[i];
      break;
    }
  }
  return found;
}

bool is_primary_key(const AiTableMeta &meta, const std::string &name)
{
  for (size_t i = 0; i < meta.pks_.size(); ++i) {
    if (equals_ci(meta.pks_[i], name)) {
      return true;
    }
  }
  return false;
}

std::string build_invalidation_set(const AiColumnSpec &col, const std::string &rid)
{
  std::string text;
  text += ", " + quote_ident(col.column_) + " = null";
  text += ", " + quote_ident(col.column_ + "$state") + " = 0";
  text += ", " + quote_ident(col.column_ + "$req") + " = " + sql_str(rid);
  return text;
}

std::string build_publish_expr(const std::string &model,
                               const std::string &kind,
                               const std::string &expression,
                               const std::string &system_prompt,
                               int64_t dim)
{
  std::string expr;
  if ("embed" == kind) {
    expr = "AI_EMBED(" + sql_str(model) + ", " + expression;
    if (dim > 0) {
      expr += ", " + std::to_string(dim);
    }
    expr += ")";
  } else {
    expr = "AI_COMPLETE(" + sql_str(model) + ", " + expression;
    if (!system_prompt.empty()) {
      expr += ", " + sql_str(std::string("{\"system\":\"") + json_escape(system_prompt) + "\"}");
    }
    expr += ")";
  }
  return expr;
}

void add_post_statement(int64_t stmt_index, const std::string &sql_text)
{
  AiColumnRequestContext &ctx = ai_column_ctx();
  if (stmt_index >= 0 && static_cast<size_t>(stmt_index) < ctx.posts_.size()) {
    ctx.posts_[static_cast<size_t>(stmt_index)].push_back(sql_text);
  }
}

void add_registry_action(int64_t stmt_index, bool erase,
                         const std::string &db, const std::string &table)
{
  AiColumnRequestContext &ctx = ai_column_ctx();
  if (stmt_index >= 0 && static_cast<size_t>(stmt_index) < ctx.actions_.size()) {
    AiColumnRegistryAction action;
    action.erase_ = erase;
    action.db_ = db;
    action.table_ = table;
    ctx.actions_[static_cast<size_t>(stmt_index)].push_back(action);
  }
}

// ---------------------------------------------------------------------------
// DDL handlers
// ---------------------------------------------------------------------------

int rewrite_create_table(sql::ObSQLSessionInfo &session,
                         int64_t stmt_index,
                         const std::string &text,
                         int64_t pos,
                         std::string &out,
                         bool &changed)
{
  int ret = OB_SUCCESS;
  changed = false;
  const char *s = text.data();
  const int64_t len = static_cast<int64_t>(text.size());
  int64_t p = pos;
  ai_skip_blank(s, len, p);
  const int64_t save = p;
  const std::string first = read_word_ci(s, len, p);
  if ("if" == first) {
    const std::string second = read_word_ci(s, len, p);
    const std::string third = read_word_ci(s, len, p);
    if ("not" != second || "exists" != third) {
      p = save;
    }
  } else {
    p = save;
  }
  std::string db;
  std::string table;
  if (!parse_qualified_name(s, len, p, db, table)) {
    return ret;
  }
  if (db.empty()) {
    db = session_default_db(session);
  }
  std::vector<DdlColumn> cols;
  if (OB_FAIL(scan_ai_columns(s, len, true, p, cols))) {
    LOG_WARN("failed to scan AI COLUMN declarations", K(ret));
  } else if (cols.empty()) {
    // ordinary create table
  } else {
    std::vector<std::string> ai_names;
    for (size_t i = 0; i < cols.size(); ++i) {
      std::string name;
      std::string type_text;
      if (parse_column_prefix(cols[i].prefix_, false, name, type_text)) {
        ai_names.push_back(name);
      }
    }
    std::string rewritten = text;
    for (int64_t i = static_cast<int64_t>(cols.size()) - 1; OB_SUCC(ret) && i >= 0; --i) {
      DdlColumn &col = cols[static_cast<size_t>(i)];
      std::string replacement;
      if (OB_FAIL(validate_and_fill_ddl_column(col, ai_names, false))) {
        LOG_WARN("invalid AI COLUMN definition", K(ret), K(col.prefix_.c_str()));
      } else if (OB_FAIL(build_ddl_replacement(col, false, replacement))) {
      } else {
        rewritten.replace(static_cast<size_t>(col.def_begin_),
                          static_cast<size_t>(col.decl_end_ - col.def_begin_),
                          replacement);
      }
    }
    if (OB_SUCC(ret)) {
      out = rewritten;
      changed = true;
      add_registry_action(stmt_index, false, db, table);
    }
  }
  return ret;
}

int rewrite_retry(int64_t stmt_index,
                  sql::ObSQLSessionInfo &session,
                  const std::string &text,
                  int64_t pos,
                  const std::string &db_in,
                  const std::string &table,
                  std::string &out,
                  bool &changed)
{
  int ret = OB_SUCCESS;
  changed = false;
  const char *s = text.data();
  const int64_t len = static_cast<int64_t>(text.size());
  int64_t p = pos;
  const std::string ai_word = read_word_ci(s, len, p);
  if ("ai" != ai_word) {
    return ret;
  }
  const std::string column_word = read_word_ci(s, len, p);
  if ("column" != column_word) {
    return ret;
  }
  std::string column;
  if (!parse_identifier(s, len, p, column)) {
    ret = OB_ERR_PARSE_SQL;
    LOG_WARN("failed to parse AI COLUMN RETRY target", K(ret));
    return ret;
  }
  const std::string db = db_in.empty() ? session_default_db(session) : db_in;
  std::shared_ptr<const AiTableMeta> meta = get_table_meta(db, table);
  const AiColumnSpec *spec = (meta.get() != nullptr) ? find_ai_column(*meta, column) : nullptr;
  if (OB_ISNULL(spec)) {
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "retrying a column that is not an AI column");
    ret = OB_NOT_SUPPORTED;
  } else {
    int64_t q = p;
    ai_skip_blank(s, len, q);
    int64_t r = q;
    int64_t depth = 0;
    bool extra_action = false;
    while (r < len) {
      const char c = s[r];
      if ('\'' == c || '"' == c || '`' == c) {
        if (!ai_skip_quoted(s, len, r)) {
          break;
        }
      } else if ('(' == c) {
        ++depth;
        ++r;
      } else if (')' == c) {
        --depth;
        ++r;
      } else if (',' == c && 0 == depth) {
        extra_action = true;
        break;
      } else {
        ++r;
      }
    }
    if (extra_action) {
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "combining AI COLUMN RETRY with another ALTER action");
      ret = OB_NOT_SUPPORTED;
    } else {
      std::string cond;
      if (q < len) {
        const int64_t save = q;
        const std::string where_word = read_word_ci(s, len, q);
        if ("where" != where_word) {
          q = save;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "combining AI COLUMN RETRY with another ALTER action");
          ret = OB_NOT_SUPPORTED;
        } else {
          cond = ai_trim_copy(text.substr(static_cast<size_t>(q)));
        }
      }
      if (OB_SUCC(ret)) {
        const std::string rid = make_request_id();
        std::string rewritten_cond;
        substitute_status(cond, build_status_expr(*spec), rewritten_cond);
        std::string update_sql = "update " + meta_qualified(*meta) + " set " +
                                 quote_ident(spec->column_) + " = null" +
                                 ", " + quote_ident(spec->column_ + "$state") + " = 0" +
                                 ", " + quote_ident(spec->column_ + "$req") + " = " + sql_str(rid);
        if (!rewritten_cond.empty()) {
          update_sql += " where " + rewritten_cond;
        }
        add_post_statement(stmt_index, update_sql);
        std::string insert_sql;
        if (OB_FAIL(build_task_insert(*meta, *spec, build_predicate_expr(*meta), rid,
                                      quote_ident(spec->column_ + "$req") + " = " + sql_str(rid),
                                      insert_sql))) {
        } else {
          add_post_statement(stmt_index, insert_sql);
          out = "set @__ai_column_retry = 1";
          changed = true;
        }
      }
    }
  }
  return ret;
}

int rewrite_alter_table(int64_t stmt_index,
                        sql::ObSQLSessionInfo &session,
                        const std::string &text,
                        int64_t pos,
                        std::string &out,
                        bool &changed)
{
  int ret = OB_SUCCESS;
  changed = false;
  const char *s = text.data();
  const int64_t len = static_cast<int64_t>(text.size());
  int64_t p = pos;
  std::string db;
  std::string table;
  if (!parse_qualified_name(s, len, p, db, table)) {
    return ret;
  }
  const std::string resolved_db = db.empty() ? session_default_db(session) : db;
  int64_t action_pos = p;
  const std::string action = read_word_ci(s, len, action_pos);
  if ("retry" == action) {
    return rewrite_retry(stmt_index, session, text, action_pos, resolved_db, table, out, changed);
  }
  std::vector<DdlColumn> cols;
  if (OB_FAIL(scan_ai_columns(s, len, false, p, cols))) {
    LOG_WARN("failed to scan AI COLUMN declarations", K(ret));
  } else if (cols.empty()) {
    // ordinary alter table
  } else {
    std::vector<std::string> ai_names;
    for (size_t i = 0; i < cols.size(); ++i) {
      std::string name;
      std::string type_text;
      if (parse_column_prefix(cols[i].prefix_, true, name, type_text)) {
        ai_names.push_back(name);
      }
    }
    std::shared_ptr<const AiTableMeta> meta = get_table_meta(resolved_db, table);
    if (meta.get() != nullptr) {
      for (size_t i = 0; i < meta->cols_.size(); ++i) {
        ai_names.push_back(meta->cols_[i].column_);
      }
    }
    std::string rewritten = text;
    for (int64_t i = static_cast<int64_t>(cols.size()) - 1; OB_SUCC(ret) && i >= 0; --i) {
      DdlColumn &col = cols[static_cast<size_t>(i)];
      std::string replacement;
      if (OB_FAIL(validate_and_fill_ddl_column(col, ai_names, true))) {
        LOG_WARN("invalid AI COLUMN definition", K(ret), K(col.prefix_.c_str()));
      } else if (OB_FAIL(build_ddl_replacement(col, true, replacement))) {
      } else {
        rewritten.replace(static_cast<size_t>(col.def_begin_),
                          static_cast<size_t>(col.decl_end_ - col.def_begin_),
                          replacement);
      }
    }
    if (OB_SUCC(ret)) {
      out = rewritten;
      changed = true;
      add_registry_action(stmt_index, false, resolved_db, table);
    }
  }
  return ret;
}

int rewrite_drop(int64_t stmt_index,
                 sql::ObSQLSessionInfo &session,
                 const std::string &text,
                 int64_t pos,
                 std::string &out,
                 bool &changed)
{
  int ret = OB_SUCCESS;
  changed = false;
  const char *s = text.data();
  const int64_t len = static_cast<int64_t>(text.size());
  int64_t p = pos;
  std::string kind = read_word_ci(s, len, p);
  if ("temporary" == kind) {
    kind = read_word_ci(s, len, p);
  }
  const std::string default_db = session_default_db(session);
  if ("database" == kind || "schema" == kind) {
    std::string db;
    std::string name;
    if (!parse_qualified_name(s, len, p, db, name)) {
      return ret;
    }
    const std::string target_db = db.empty() ? name : db;
    add_post_statement(stmt_index,
                       "delete from oceanbase.__all_ai_column_task where database_name = " +
                           sql_str(target_db));
    add_registry_action(stmt_index, true, target_db, std::string());
  } else if ("table" == kind || "tables" == kind) {
    int64_t q = p;
    ai_skip_blank(s, len, q);
    const int64_t save = q;
    const std::string first = read_word_ci(s, len, q);
    if ("if" == first) {
      const std::string second = read_word_ci(s, len, q);
      const std::string third = read_word_ci(s, len, q);
      if ("not" != second || "exists" != third) {
        q = save;
      }
    } else {
      q = save;
    }
    while (q < len) {
      std::string db;
      std::string table;
      if (!parse_qualified_name(s, len, q, db, table)) {
        break;
      }
      const std::string resolved_db = db.empty() ? default_db : db;
      if (AiColumnRegistry::instance().is_ai_table(resolved_db, table)) {
        add_post_statement(stmt_index,
                           "delete from oceanbase.__all_ai_column_task where database_name = " +
                               sql_str(resolved_db) + " and table_name = " + sql_str(table));
        add_registry_action(stmt_index, true, resolved_db, table);
      }
      ai_skip_blank(s, len, q);
      if (q < len && ',' == s[q]) {
        ++q;
        continue;
      }
      break;
    }
  }
  return ret;
}

// ---------------------------------------------------------------------------
// DML handlers
// ---------------------------------------------------------------------------

int rewrite_insert(int64_t stmt_index,
                   sql::ObSQLSessionInfo &session,
                   const std::string &text,
                   int64_t pos,
                   std::string &out,
                   bool &changed)
{
  int ret = OB_SUCCESS;
  changed = false;
  const char *s = text.data();
  const int64_t len = static_cast<int64_t>(text.size());
  int64_t into = ai_find_top_level_keyword(s, len, pos, "into");
  if (into < 0) {
    return ret;
  }
  int64_t p = into + static_cast<int64_t>(strlen("into"));
  std::string db;
  std::string table;
  if (!parse_qualified_name(s, len, p, db, table)) {
    return ret;
  }
  const std::string resolved_db = db.empty() ? session_default_db(session) : db;
  std::shared_ptr<const AiTableMeta> meta = get_table_meta(resolved_db, table);
  if (meta.get() == nullptr || meta->cols_.empty()) {
    return ret;
  }
  if (OB_FAIL(check_strict_read(session, text, *meta))) {
  } else {
    int64_t q = p;
    ai_skip_blank(s, len, q);
    std::vector<std::string> col_names;
    int64_t values_pos = q;
    if (q < len && '(' == s[q]) {
      int64_t close = 0;
      if (!find_matching_paren(s, len, q, close)) {
        ret = OB_ERR_PARSE_SQL;
      } else {
        std::vector<std::string> raw_names;
        if (OB_FAIL(ai_split_args(s, q + 1, close, raw_names))) {
        } else {
          for (size_t i = 0; i < raw_names.size(); ++i) {
            std::string name;
            const std::string &raw = raw_names[i];
            int64_t np = 0;
            if (parse_identifier(raw.data(), static_cast<int64_t>(raw.size()), np, name)) {
              col_names.push_back(name);
            }
          }
        }
        values_pos = close + 1;
      }
    }
    int64_t values_kw = ai_find_top_level_keyword(s, len, values_pos, "values");
    if (OB_SUCC(ret) && values_kw < 0) {
      // insert ... select / insert ... set: no task is created
      LOG_TRACE("unsupported insert form for AI column", K(text.c_str()));
      ret = OB_SUCCESS;
    } else if (OB_SUCC(ret)) {
      int64_t t = values_kw + static_cast<int64_t>(strlen("values"));
      std::vector<std::vector<std::string>> tuples;
      while (t < len) {
        ai_skip_blank(s, len, t);
        if (t >= len || '(' != s[t]) {
          break;
        }
        int64_t close = 0;
        if (!find_matching_paren(s, len, t, close)) {
          ret = OB_ERR_PARSE_SQL;
          break;
        }
        std::vector<std::string> values;
        if (OB_FAIL(ai_split_args(s, t + 1, close, values))) {
          break;
        }
        tuples.push_back(values);
        t = close + 1;
        ai_skip_blank(s, len, t);
        if (t < len && ',' == s[t]) {
          ++t;
          continue;
        }
        break;
      }
      if (OB_SUCC(ret) && !tuples.empty()) {
        const std::string rid = make_request_id();
        const std::string predicate_expr = build_predicate_expr(*meta);
        for (size_t i = 0; OB_SUCC(ret) && i < tuples.size(); ++i) {
          std::string where_sql;
          bool pk_ok = true;
          for (size_t k = 0; k < meta->pks_.size() && pk_ok; ++k) {
            int64_t found = -1;
            for (size_t c = 0; c < col_names.size(); ++c) {
              if (equals_ci(col_names[c], meta->pks_[k])) {
                found = static_cast<int64_t>(c);
                break;
              }
            }
            if (found < 0 || static_cast<size_t>(found) >= tuples[i].size()) {
              pk_ok = false;
              break;
            }
            if (!where_sql.empty()) {
              where_sql += " and ";
            }
            where_sql += quote_ident(meta->pks_[k]) + " = " + tuples[i][static_cast<size_t>(found)];
          }
          if (!pk_ok) {
            LOG_TRACE("insert without primary key value does not enqueue AI column tasks",
                      K(text.c_str()));
            continue;
          }
          for (size_t c = 0; OB_SUCC(ret) && c < meta->cols_.size(); ++c) {
            if (contains_identifier(col_names, meta->cols_[c].column_)) {
              continue;
            }
            std::string insert_sql;
            if (OB_FAIL(build_task_insert(*meta, meta->cols_[c], predicate_expr, rid, where_sql,
                                          insert_sql))) {
            } else {
              add_post_statement(stmt_index, insert_sql);
            }
          }
        }
      }
    }
  }
  return ret;
}

int rewrite_update(int64_t stmt_index,
                   sql::ObSQLSessionInfo &session,
                   const std::string &text,
                   int64_t pos,
                   std::string &out,
                   bool &changed)
{
  int ret = OB_SUCCESS;
  changed = false;
  const char *s = text.data();
  const int64_t len = static_cast<int64_t>(text.size());
  int64_t p = pos;
  std::string db;
  std::string table;
  if (!parse_qualified_name(s, len, p, db, table)) {
    return ret;
  }
  const std::string resolved_db = db.empty() ? session_default_db(session) : db;
  const int64_t set_pos = ai_find_top_level_keyword(s, len, p, "set");
  if (set_pos < 0) {
    return ret;
  }
  std::shared_ptr<const AiTableMeta> meta = get_table_meta(resolved_db, table);
  if (meta.get() == nullptr || meta->cols_.empty()) {
    return ret;
  }
  if (OB_FAIL(check_strict_read(session, text, *meta))) {
  } else {
    int64_t boundary = len;
    const char *const keywords[] = {"where", "order", "limit"};
    for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); ++i) {
      const int64_t hit = ai_find_top_level_keyword(s, len,
                                                 set_pos + static_cast<int64_t>(strlen("set")),
                                                 keywords[i]);
      if (hit >= 0 && hit < boundary) {
        boundary = hit;
      }
    }
    const std::string set_body = text.substr(
        static_cast<size_t>(set_pos) + strlen("set"),
        static_cast<size_t>(boundary) - static_cast<size_t>(set_pos) - strlen("set"));
    std::vector<std::string> assignments;
    if (OB_FAIL(ai_split_args(set_body.data(), 0, static_cast<int64_t>(set_body.size()),
                           assignments))) {
    } else {
      std::vector<std::string> assigned;
      for (size_t i = 0; i < assignments.size(); ++i) {
        int64_t ap = 0;
        std::string alias;
        std::string column;
        if (parse_qualified_name(assignments[i].data(),
                                 static_cast<int64_t>(assignments[i].size()), ap, alias, column)) {
          assigned.push_back(column);
        }
      }
      bool pk_changed = false;
      for (size_t i = 0; i < assigned.size(); ++i) {
        if (is_primary_key(*meta, assigned[i])) {
          pk_changed = true;
        }
      }
      std::vector<size_t> targets;
      for (size_t i = 0; i < meta->cols_.size(); ++i) {
        if (contains_identifier(assigned, meta->cols_[i].column_)) {
          continue;
        }
        bool hit = pk_changed;
        if (!hit) {
          std::vector<std::string> expr_idents;
          collect_identifiers(meta->cols_[i].expr_, expr_idents);
          for (size_t k = 0; !hit && k < expr_idents.size(); ++k) {
            if (contains_identifier(assigned, expr_idents[k])) {
              hit = true;
            }
          }
        }
        if (hit) {
          targets.push_back(i);
        }
      }
      if (!targets.empty()) {
        const std::string rid = make_request_id();
        std::string append;
        for (size_t i = 0; i < targets.size(); ++i) {
          append += build_invalidation_set(meta->cols_[targets[i]], rid);
        }
        const std::string predicate_expr = build_predicate_expr(*meta);
        std::string rewritten = text;
        if (boundary < len) {
          rewritten.insert(static_cast<size_t>(boundary), append + " ");
        } else {
          rewritten += append;
        }
        out = rewritten;
        changed = true;
        for (size_t i = 0; OB_SUCC(ret) && i < targets.size(); ++i) {
          const AiColumnSpec &col = meta->cols_[targets[i]];
          std::string insert_sql;
          if (OB_FAIL(build_task_insert(*meta, col, predicate_expr, rid,
                                        quote_ident(col.column_ + "$req") + " = " + sql_str(rid),
                                        insert_sql))) {
          } else {
            add_post_statement(stmt_index, insert_sql);
          }
        }
      }
    }
  }
  return ret;
}

int rewrite_delete(int64_t stmt_index,
                   sql::ObSQLSessionInfo &session,
                   const std::string &text,
                   int64_t pos,
                   std::string &out,
                   bool &changed)
{
  int ret = OB_SUCCESS;
  changed = false;
  const char *s = text.data();
  const int64_t len = static_cast<int64_t>(text.size());
  const int64_t from_pos = ai_find_top_level_keyword(s, len, pos, "from");
  if (from_pos < 0) {
    return ret;
  }
  int64_t p = from_pos + static_cast<int64_t>(strlen("from"));
  std::string db;
  std::string table;
  if (!parse_qualified_name(s, len, p, db, table)) {
    return ret;
  }
  const std::string resolved_db = db.empty() ? session_default_db(session) : db;
  std::shared_ptr<const AiTableMeta> meta = get_table_meta(resolved_db, table);
  if (meta.get() == nullptr || meta->cols_.empty()) {
    return ret;
  }
  if (OB_FAIL(check_strict_read(session, text, *meta))) {
  } else {
    const int64_t where_pos = ai_find_top_level_keyword(s, len, p, "where");
    std::string cond;
    if (where_pos >= 0) {
      int64_t cond_end = len;
      const int64_t order_pos = ai_find_top_level_keyword(s, len, where_pos, "order");
      if (order_pos >= 0 && order_pos < cond_end) {
        cond_end = order_pos;
      }
      const int64_t limit_pos = ai_find_top_level_keyword(s, len, where_pos, "limit");
      if (limit_pos >= 0 && limit_pos < cond_end) {
        cond_end = limit_pos;
      }
      cond = ai_trim_copy(text.substr(static_cast<size_t>(where_pos) + strlen("where"),
                                   static_cast<size_t>(cond_end) - static_cast<size_t>(where_pos) -
                                       strlen("where")));
    }
    if (cond.empty()) {
      add_post_statement(stmt_index,
                         "delete from oceanbase.__all_ai_column_task where database_name = " +
                             sql_str(meta->db_) + " and table_name = " + sql_str(meta->table_));
    } else {
      InnerSql sql;
      sql.set_session(&session);
      std::vector<std::vector<std::string>> rows;
      const std::string read_sql = "select " + build_predicate_expr(*meta) + " from " +
                                   meta_qualified(*meta) + " where " + cond;
      if (OB_FAIL(sql.read(read_sql, rows))) {
        LOG_WARN("failed to read deleted row keys", K(ret), K(read_sql.c_str()));
      } else if (!rows.empty()) {
        std::string in_list;
        for (size_t i = 0; i < rows.size(); ++i) {
          if (rows[i].empty()) {
            continue;
          }
          if (!in_list.empty()) {
            in_list += ", ";
          }
          in_list += sql_str(rows[i][0]);
        }
        if (!in_list.empty()) {
          add_post_statement(stmt_index,
                             "delete from oceanbase.__all_ai_column_task where database_name = " +
                                 sql_str(meta->db_) + " and table_name = " + sql_str(meta->table_) +
                                 " and rowkey_predicate in (" + in_list + ")");
        }
      }
    }
  }
  return ret;
}

// ---------------------------------------------------------------------------
// generic statements
// ---------------------------------------------------------------------------

void scan_table_refs(sql::ObSQLSessionInfo &session,
                     const std::string &text,
                     std::vector<std::pair<std::string, std::string>> &refs)
{
  const std::string default_db = session_default_db(session);
  AiColumnRegistry &registry = AiColumnRegistry::instance();
  const char *s = text.data();
  const int64_t len = static_cast<int64_t>(text.size());
  int64_t pos = 0;
  std::set<std::string> seen;
  while (pos < len) {
    const char c = s[pos];
    if ('\'' == c || '"' == c) {
      if (!ai_skip_quoted(s, len, pos)) {
        break;
      }
    } else if (ai_is_ident_char(c) || '`' == c) {
      int64_t p = pos;
      std::string db;
      std::string table;
      if (!parse_qualified_name(s, len, p, db, table)) {
        ++pos;
        continue;
      }
      pos = p;
      if (table.empty()) {
        continue;
      }
      const std::string resolved_db = db.empty() ? default_db : db;
      const std::string key = table_key(resolved_db, table);
      if (seen.insert(key).second && registry.is_ai_table(resolved_db, table)) {
        refs.push_back(std::make_pair(resolved_db, table));
      }
    } else {
      ++pos;
    }
  }
}

int rewrite_generic(sql::ObSQLSessionInfo &session,
                    const std::string &text,
                    std::string &out,
                    bool &changed)
{
  int ret = OB_SUCCESS;
  changed = false;
  std::vector<std::pair<std::string, std::string>> refs;
  scan_table_refs(session, text, refs);
  for (size_t i = 0; OB_SUCC(ret) && i < refs.size(); ++i) {
    std::shared_ptr<const AiTableMeta> meta = get_table_meta(refs[i].first, refs[i].second);
    if (meta.get() != nullptr && !meta->cols_.empty()) {
      if (OB_FAIL(check_strict_read(session, text, *meta))) {
      }
    }
  }
  if (OB_SUCC(ret)) {
    std::string rewritten;
    bool col_changed = false;
    replace_all_column_reference(text, rewritten, col_changed);
    const std::string &base = col_changed ? rewritten : text;
    std::string strict_out;
    bool strict_changed = false;
    replace_strict_reference(base.data(), static_cast<int64_t>(base.size()),
                             strict_out, strict_changed);
    if (col_changed || strict_changed) {
      out = strict_out;
      changed = true;
    }
  }
  return ret;
}

int rewrite_one_statement(sql::ObSQLSessionInfo &session,
                          int64_t stmt_index,
                          const std::string &raw,
                          std::string &out,
                          bool &changed)
{
  int ret = OB_SUCCESS;
  changed = false;
  const std::string text = strip_statement(raw);
  if (text.empty()) {
    return ret;
  }
  const char *s = text.data();
  const int64_t len = static_cast<int64_t>(text.size());
  int64_t pos = 0;
  const std::string head = read_word_ci(s, len, pos);
  if (head.empty()) {
    return ret;
  }
  if ("set" == head) {
    std::string rewritten;
    bool set_changed = false;
    if (rewrite_strict_set(text, rewritten, set_changed) && set_changed) {
      out = rewritten;
      changed = true;
    }
  } else if ("create" == head) {
    const std::string second = read_word_ci(s, len, pos);
    if ("table" == second) {
      ret = rewrite_create_table(session, stmt_index, text, pos, out, changed);
    } else {
      ret = rewrite_generic(session, text, out, changed);
    }
  } else if ("alter" == head) {
    const std::string second = read_word_ci(s, len, pos);
    if ("table" == second) {
      ret = rewrite_alter_table(stmt_index, session, text, pos, out, changed);
    } else {
      ret = rewrite_generic(session, text, out, changed);
    }
  } else if ("insert" == head || "replace" == head) {
    ret = rewrite_insert(stmt_index, session, text, pos, out, changed);
  } else if ("update" == head) {
    ret = rewrite_update(stmt_index, session, text, pos, out, changed);
  } else if ("delete" == head) {
    ret = rewrite_delete(stmt_index, session, text, pos, out, changed);
  } else if ("drop" == head) {
    ret = rewrite_drop(stmt_index, session, text, pos, out, changed);
  } else {
    ret = rewrite_generic(session, text, out, changed);
  }
  return ret;
}

// ---------------------------------------------------------------------------
// background materialization worker
// ---------------------------------------------------------------------------

struct AiTaskRow
{
  int64_t task_id_ = 0;
  int64_t table_id_ = 0;
  int64_t column_id_ = 0;
  std::string db_;
  std::string table_;
  std::string column_;
  std::string predicate_;
  std::string rid_;
  int64_t state_ = 0;
  int64_t retry_count_ = 0;
  int64_t max_retries_ = 0;
  std::string result_type_;
  std::string expression_;
  std::string system_prompt_;
  std::string model_;
  std::string kind_;
  int64_t dim_ = 0;
};

struct AiTaskGroup
{
  int64_t table_id_ = 0;
  std::string db_;
  std::string table_;
  std::string predicate_;
  std::string rid_;
  std::vector<AiTaskRow> rows_;
};

std::atomic<bool> g_ai_column_worker_started(false);

int fetch_ai_column_tasks(InnerSql &sql, std::vector<AiTaskRow> &rows)
{
  int ret = OB_SUCCESS;
  const std::string query =
      std::string("select task_id, table_id, column_id, database_name, table_name, column_name, "
                  "rowkey_predicate, request_id, state, retry_count, max_retries, result_type, "
                  "ifnull(expression,''), ifnull(system_prompt,''), model_name, kind, dim "
                  "from oceanbase.__all_ai_column_task where state in (0, 3) "
                  "and eligible_ts <= now(6) order by task_id limit ") +
      std::to_string(WORKER_BATCH_SIZE);
  std::vector<std::vector<std::string>> data;
  if (OB_FAIL(sql.read(query, data))) {
    LOG_WARN("failed to fetch AI column tasks", K(ret));
  } else {
    for (size_t i = 0; i < data.size(); ++i) {
      if (data[i].size() < 17) {
        continue;
      }
      AiTaskRow row;
      row.task_id_ = atoll(data[i][0].c_str());
      row.table_id_ = atoll(data[i][1].c_str());
      row.column_id_ = atoll(data[i][2].c_str());
      row.db_ = data[i][3];
      row.table_ = data[i][4];
      row.column_ = data[i][5];
      row.predicate_ = data[i][6];
      row.rid_ = data[i][7];
      row.state_ = atoll(data[i][8].c_str());
      row.retry_count_ = atoll(data[i][9].c_str());
      row.max_retries_ = atoll(data[i][10].c_str());
      row.result_type_ = data[i][11];
      row.expression_ = data[i][12];
      row.system_prompt_ = data[i][13];
      row.model_ = data[i][14];
      row.kind_ = data[i][15];
      row.dim_ = atoll(data[i][16].c_str());
      rows.push_back(row);
    }
  }
  return ret;
}

int delete_ai_task_rows(InnerSql &sql, const std::vector<int64_t> &task_ids)
{
  int ret = OB_SUCCESS;
  if (task_ids.empty()) {
    return ret;
  }
  std::string list;
  for (size_t i = 0; i < task_ids.size(); ++i) {
    if (!list.empty()) {
      list += ", ";
    }
    list += std::to_string(task_ids[i]);
  }
  const std::string stmt = "delete from oceanbase.__all_ai_column_task where task_id in (" +
                           list + ")";
  int64_t affected = 0;
  if (OB_FAIL(sql.write(stmt, affected))) {
    LOG_WARN("failed to delete AI column tasks", K(ret), K(stmt.c_str()));
  }
  return ret;
}

bool ai_task_group_retryable(const AiTaskGroup &group)
{
  int64_t max_retries = group.rows_.empty() ? 0 : group.rows_[0].max_retries_;
  for (size_t i = 1; i < group.rows_.size(); ++i) {
    if (group.rows_[i].max_retries_ < max_retries) {
      max_retries = group.rows_[i].max_retries_;
    }
  }
  for (size_t i = 0; i < group.rows_.size(); ++i) {
    if (group.rows_[i].retry_count_ < max_retries) {
      return true;
    }
  }
  return false;
}

std::string build_req_guard(const AiTaskGroup &group)
{
  std::string guard;
  for (size_t i = 0; i < group.rows_.size(); ++i) {
    if (!guard.empty()) {
      guard += " and ";
    }
    const std::string req = quote_ident(group.rows_[i].column_ + "$req");
    guard += "(" + req + " = " + sql_str(group.rid_) + " or " + req + " is null)";
  }
  return guard;
}

int process_ai_task_group(InnerSql &sql, const AiTaskGroup &group)
{
  int ret = OB_SUCCESS;
  std::vector<int64_t> task_ids;
  std::string task_id_list;
  for (size_t i = 0; i < group.rows_.size(); ++i) {
    task_ids.push_back(group.rows_[i].task_id_);
    if (!task_id_list.empty()) {
      task_id_list += ", ";
    }
    task_id_list += std::to_string(group.rows_[i].task_id_);
  }
  const std::string table_name = qualified_name(group.db_, group.table_);
  std::string update_sql = "update " + table_name + " set ";
  for (size_t i = 0; i < group.rows_.size(); ++i) {
    const AiTaskRow &row = group.rows_[i];
    update_sql += quote_ident(row.column_) + " = " +
                  build_publish_expr(row.model_, row.kind_, row.expression_,
                                     row.system_prompt_, row.dim_) + ", ";
    update_sql += quote_ident(row.column_ + "$state") + " = 1, ";
  }
  update_sql.resize(update_sql.size() - 2);
  const std::string guard = build_req_guard(group);
  std::string where_sql = group.predicate_;
  if (!guard.empty()) {
    if (!where_sql.empty()) {
      where_sql += " and ";
    }
    where_sql += guard;
  }
  if (!where_sql.empty()) {
    update_sql += " where " + where_sql;
  }
  int64_t affected = 0;
  if (OB_SUCCESS == sql.write(update_sql, affected)) {
    if (OB_FAIL(delete_ai_task_rows(sql, task_ids))) {
    }
  } else if (ai_task_group_retryable(group)) {
    const std::string retry_sql =
        "update oceanbase.__all_ai_column_task set state = 3, retry_count = retry_count + 1, "
        "eligible_ts = date_add(now(6), interval 300000 microsecond) where task_id in (" +
        task_id_list + ")";
    int64_t retried = 0;
    if (OB_FAIL(sql.write(retry_sql, retried))) {
      LOG_WARN("failed to schedule AI column task retry", K(ret));
    }
  } else {
    std::string fail_sql = "update " + table_name + " set ";
    for (size_t i = 0; i < group.rows_.size(); ++i) {
      fail_sql += quote_ident(group.rows_[i].column_ + "$state") + " = 2, ";
    }
    fail_sql.resize(fail_sql.size() - 2);
    if (!where_sql.empty()) {
      fail_sql += " where " + where_sql;
    }
    int64_t failed = 0;
    if (OB_FAIL(sql.write(fail_sql, failed))) {
      LOG_WARN("failed to mark AI column as failed", K(ret), K(fail_sql.c_str()));
    }
    if (OB_FAIL(delete_ai_task_rows(sql, task_ids))) {
    }
  }
  return ret;
}

void ai_column_worker_loop()
{
  int ret = OB_SUCCESS;
  std::unique_ptr<InnerSql> sql(new (std::nothrow) InnerSql());
  // A detached thread has no tenant entity or thread local context of its own;
  // both are required before the inner SQL proxy can serve reads and writes.
  CREATE_WITH_TEMP_ENTITY(RESOURCE_OWNER, common::OB_SERVER_RUNTIME_ID) {
    while (OB_NOT_NULL(sql.get())) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      ret = OB_SUCCESS;
      if (OB_SUCCESS != share::check_server_runtime_ready()) {
        continue;
      }
      lib::ContextTLOptGuard context_guard(true);
      lib::ContextParam param;
      param.set_properties(lib::USE_TL_PAGE_OPTIONAL)
          .set_page_size(OB_MALLOC_REQ_NORMAL_BLOCK_SIZE);
      CREATE_WITH_TEMP_CONTEXT(param) {
        std::vector<AiTaskRow> rows;
        if (OB_SUCCESS == fetch_ai_column_tasks(*sql, rows) && !rows.empty()) {
          std::vector<AiTaskGroup> groups;
          std::map<std::string, size_t> index;
          for (size_t i = 0; i < rows.size(); ++i) {
            const std::string key = std::to_string(rows[i].table_id_) + "\x01" +
                                    rows[i].predicate_ + "\x01" + rows[i].rid_;
            auto iter = index.find(key);
            if (index.end() == iter) {
              AiTaskGroup group;
              group.table_id_ = rows[i].table_id_;
              group.db_ = rows[i].db_;
              group.table_ = rows[i].table_;
              group.predicate_ = rows[i].predicate_;
              group.rid_ = rows[i].rid_;
              group.rows_.push_back(rows[i]);
              index.insert(std::make_pair(key, groups.size()));
              groups.push_back(group);
            } else {
              groups[iter->second].rows_.push_back(rows[i]);
            }
          }
          // Rows are published in parallel: a slow or unreachable endpoint
          // costs one network timeout per row, and a failed request must not
          // delay the remaining rows of the same batch.
          const size_t max_parallel = 8;
          std::vector<std::thread> publishers;
          for (size_t base = 0; base < groups.size(); base += max_parallel) {
            const size_t end = std::min(base + max_parallel, groups.size());
            publishers.clear();
            for (size_t i = base; i < end; ++i) {
              publishers.emplace_back([&groups, i]() {
                int ret = OB_SUCCESS;
                CREATE_WITH_TEMP_ENTITY(RESOURCE_OWNER, common::OB_SERVER_RUNTIME_ID) {
                  lib::ContextTLOptGuard guard(true);
                  lib::ContextParam param;
                  param.set_properties(lib::USE_TL_PAGE_OPTIONAL)
                      .set_page_size(OB_MALLOC_REQ_NORMAL_BLOCK_SIZE);
                  CREATE_WITH_TEMP_CONTEXT(param) {
                    InnerSql sql;
                    (void)process_ai_task_group(sql, groups[i]);
                  }
                }
              });
            }
            for (size_t i = 0; i < publishers.size(); ++i) {
              publishers[i].join();
            }
          }
        }
      }
    }
  }
}

} // namespace

int rewrite_ai_column_statements(sql::ObSQLSessionInfo &session,
                                 common::ObIAllocator &alloc,
                                 common::ObIArray<common::ObString> &queries)
{
  int ret = OB_SUCCESS;
  AiColumnRequestContext &ctx = ai_column_ctx();
  ctx.posts_.clear();
  ctx.actions_.clear();
  const size_t count = static_cast<size_t>(queries.count());
  ctx.posts_.resize(count);
  ctx.actions_.resize(count);
  if (OB_FAIL(start_ai_column_worker())) {
    LOG_WARN("failed to start AI column worker", K(ret));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < queries.count(); ++i) {
    const common::ObString &stmt = queries.at(i);
    if (stmt.empty() || OB_ISNULL(stmt.ptr())) {
      continue;
    }
    const std::string original(stmt.ptr(), static_cast<size_t>(stmt.length()));
    std::string rewritten;
    bool changed = false;
    if (OB_FAIL(rewrite_one_statement(session, i, original, rewritten, changed))) {
      LOG_WARN("failed to rewrite AI column statement", K(ret), K(original.c_str()));
    } else if (changed) {
      common::ObString new_sql;
      if (OB_FAIL(ai_copy_to_allocator(alloc, rewritten, new_sql))) {
      } else {
        queries.at(i).assign(new_sql.ptr(), new_sql.length());
        LOG_TRACE("AI column statement rewritten", K(original.c_str()), K(rewritten.c_str()));
      }
    }
  }
  return ret;
}

bool ai_column_has_post_statements()
{
  AiColumnRequestContext &ctx = ai_column_ctx();
  for (size_t i = 0; i < ctx.posts_.size(); ++i) {
    if (!ctx.posts_[i].empty()) {
      return true;
    }
  }
  return false;
}

int run_ai_column_post_statements(sql::ObSQLSessionInfo &session, int64_t stmt_index)
{
  int ret = OB_SUCCESS;
  AiColumnRequestContext &ctx = ai_column_ctx();
  if (stmt_index < 0 || static_cast<size_t>(stmt_index) >= ctx.posts_.size()) {
    return ret;
  }
  std::vector<std::string> &posts = ctx.posts_[static_cast<size_t>(stmt_index)];
  std::vector<AiColumnRegistryAction> &actions = ctx.actions_[static_cast<size_t>(stmt_index)];
  for (size_t i = 0; i < actions.size(); ++i) {
    if (actions[i].erase_) {
      if (actions[i].table_.empty()) {
        AiColumnRegistry::instance().erase_db(actions[i].db_);
      } else {
        AiColumnRegistry::instance().erase(actions[i].db_, actions[i].table_);
      }
    } else {
      AiColumnRegistry::instance().mark_dirty(actions[i].db_, actions[i].table_);
    }
  }
  actions.clear();
  if (!posts.empty()) {
    InnerSql sql;
    sql.set_session(&session);
    for (size_t i = 0; i < posts.size(); ++i) {
      int64_t affected = 0;
      if (OB_FAIL(sql.write(posts[i], affected))) {
        LOG_WARN("AI column auxiliary statement failed", K(ret), K(posts[i].c_str()));
        ret = OB_SUCCESS;
      } else {
        LOG_TRACE("AI column auxiliary statement executed", K(posts[i].c_str()), K(affected));
      }
    }
    posts.clear();
  }
  return ret;
}

int start_ai_column_worker()
{
  int ret = OB_SUCCESS;
  bool expected = false;
  if (g_ai_column_worker_started.compare_exchange_strong(expected, true)) {
    std::thread worker(ai_column_worker_loop);
    worker.detach();
  }
  return ret;
}

} // namespace observer
} // namespace oceanbase
