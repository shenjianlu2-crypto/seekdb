#pragma once
#include "common.h"
#include <memory>

namespace mvdb {

struct Expr {
  enum Kind { COL, LIT, L2 } kind = COL;
  std::string name;
  Value lit;
  std::shared_ptr<Expr> a, b;
};
using ExprPtr = std::shared_ptr<Expr>;

struct SelectItem {
  bool star = false;
  ExprPtr expr;
  std::string alias;
};

struct OrderItem {
  ExprPtr expr;
  bool desc = false;
};

struct ColDef {
  std::string name;
  ColType type = ColType::BIGINT;
  int len = 0;
  bool pk = false;
};

struct Stmt {
  enum Kind {
    CREATE_TABLE, INSERT, SELECT, UPDATE, DELETE,
    BEGIN_STMT, COMMIT_STMT, ROLLBACK_STMT, CREATE_INDEX, SET_MODE, EXIT_STMT, UNSUPPORTED
  } kind = UNSUPPORTED;

  std::string table;
  std::string index;
  std::vector<ColDef> cols;
  std::vector<std::vector<Value>> rows;
  std::vector<SelectItem> items;
  bool has_where = false;
  ExprPtr where;
  std::vector<OrderItem> order;
  int64_t limit = -1;
  std::vector<std::pair<std::string, Value>> assigns;
  std::string mode;
  std::string unsupported_text;
};

Stmt parse_sql(const std::string& sql);

}  // namespace mvdb