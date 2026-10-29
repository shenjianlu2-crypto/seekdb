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

#ifndef OCEANBASE_OBSERVER_MYSQL_OB_AI_COLUMN_REWRITE_H_
#define OCEANBASE_OBSERVER_MYSQL_OB_AI_COLUMN_REWRITE_H_

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

// Materializes the AI COLUMN extension on top of ordinary user tables:
//  * DDL: "col <type> AI COLUMN (AI_COMPLETE(...)) WITH (...)" is turned into a
//    plain result column carrying the metadata in its comment plus three hidden
//    helper columns (state / request / result spec).
//  * DML: statements touching a table with AI columns are extended with the
//    statements that invalidate stale results and enqueue worker tasks inside
//    the user transaction.
//  * RETRY ALTER: "alter table ... retry ai column c where ..." is turned into
//    an invalidation update.
//  * strict read: when the session switch is on, statements referencing an AI
//    column whose results are still missing are rejected with 11120.
//
// Rewritten statements are allocated in 'alloc'.  Auxiliary statements (task
// bookkeeping and result invalidation) are not appended to 'queries'; they are
// parked per statement index and must be executed by the caller through
// run_ai_column_post_statements() after the corresponding user statement
// succeeded, inside the very same session and transaction.
int rewrite_ai_column_statements(sql::ObSQLSessionInfo &session,
                                 common::ObIAllocator &alloc,
                                 common::ObIArray<common::ObString> &queries);

// True when at least one statement of the current request owns auxiliary
// statements.  Such requests must be answered synchronously so that the
// auxiliary statements cannot race with an asynchronous end-transaction
// callback.
bool ai_column_has_post_statements();

// Executes (and forgets) the auxiliary statements of one user statement.
int run_ai_column_post_statements(sql::ObSQLSessionInfo &session, int64_t stmt_index);

// Starts the background materialization thread on first use.
int start_ai_column_worker();

} // namespace observer
} // namespace oceanbase

#endif // OCEANBASE_OBSERVER_MYSQL_OB_AI_COLUMN_REWRITE_H_
