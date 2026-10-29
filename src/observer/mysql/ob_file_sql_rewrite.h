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

#ifndef OCEANBASE_OBSERVER_MYSQL_OB_FILE_SQL_REWRITE_H_
#define OCEANBASE_OBSERVER_MYSQL_OB_FILE_SQL_REWRITE_H_

#include "lib/string/ob_string.h"
#include "lib/container/ob_iarray.h"
#include "lib/allocator/ob_allocator.h"

namespace oceanbase
{
namespace sql
{
class ObSQLSessionInfo;
}
namespace observer
{

// Rewrites statements that reference the file table functions (read_csv, read_json,
// read_ndjson, read_parquet, glob) into native json_table scans over the parsed file
// content.  Also emulates the enable_file_sql session switch and DESCRIBE syntax.
//
// Rewritten statements are allocated in 'alloc'; entries replaced in 'queries'.
// On failure the connection must stay alive and the error must be reported.
int rewrite_file_sql_statements(sql::ObSQLSessionInfo &session,
                                common::ObIAllocator &alloc,
                                common::ObIArray<common::ObString> &queries);

} // namespace observer
} // namespace oceanbase

#endif // OCEANBASE_OBSERVER_MYSQL_OB_FILE_SQL_REWRITE_H_
