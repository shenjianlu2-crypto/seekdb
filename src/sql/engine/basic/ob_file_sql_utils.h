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

#ifndef OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_SQL_UTILS_H_
#define OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_SQL_UTILS_H_

#include "lib/string/ob_string.h"
#include "lib/allocator/ob_allocator.h"
#include "lib/container/ob_iarray.h"

namespace oceanbase
{
namespace sql
{

enum ObFileSqlColumnType
{
  FS_COL_NULL = 0,
  FS_COL_BIGINT,
  FS_COL_DOUBLE,
  FS_COL_BOOLEAN,
  FS_COL_DATE,
  FS_COL_DATETIME,
  FS_COL_VARCHAR
};

struct ObFileSqlColumn
{
  common::ObString name_;
  ObFileSqlColumnType type_;
  bool nullable_;
  int64_t max_len_;
  ObFileSqlColumn()
    : name_(), type_(FS_COL_NULL), nullable_(false), max_len_(0) {}
};

struct ObFileSqlOptions
{
  bool enabled_;
  bool header_;
  bool header_specified_;
  int64_t delim_;
  int64_t quote_;
  int64_t escape_;
  int64_t sample_size_;
  common::ObString nullstr_;
  common::ObString format_;
  ObFileSqlOptions()
    : enabled_(false), header_(true), header_specified_(false), delim_(','),
      quote_('"'), escape_('\\'), sample_size_(-1), nullstr_(), format_() {}
};

// options text format: key=value pairs separated by ';'
// keys: enabled, header, delim, quote, escape, nullstr, format, sample
int parse_file_sql_options(const common::ObString &text, ObFileSqlOptions &opts);

// Reads the file and produces a JSON array of objects (one object per row).
int read_file_sql_json(common::ObIAllocator &alloc,
                       const common::ObString &path,
                       const ObFileSqlOptions &opts,
                       common::ObString &json_out);

// Infers the column schema of the file. Column names are allocated in alloc.
int infer_file_sql_schema(common::ObIAllocator &alloc,
                          const common::ObString &path,
                          ObFileSqlOptions &opts,
                          common::ObIArray<ObFileSqlColumn> &columns,
                          common::ObString &format_out);

// Expands a file pattern, produces a JSON array of file names (with '/' separators).
int glob_file_sql(common::ObIAllocator &alloc,
                  const common::ObString &pattern,
                  common::ObString &json_out);

// Returns true when the file can be accessed under the secure_file_priv setting.
bool file_sql_path_allowed(const common::ObString &secure_file_priv,
                           const common::ObString &path);

} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_SQL_UTILS_H_
