#pragma once
#include "common.h"
#include "sql.h"
#include "store.h"
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <functional>

namespace mvdb {

constexpr ColType DBL = (ColType)3;

struct Column {
  std::string name;
  ColType type = ColType::BIGINT;
  int len = 0;
  bool pk = false;
};

struct Table {
  uint16_t id = 0;
  std::string name;
  std::vector<Column> cols;
  uint32_t pk_root = 0;
  int pk_col = -1;
  int vec_col = -1;
  bool has_index = false;
  std::string index_name;
  uint32_t heap_hint = 0;
};

struct RowRef {
  uint32_t page = 0;
  uint32_t slot = 0;
};

struct Row {
  int64_t pk = 0;
  std::vector<Value> vals;
};

// Which response fields the statement produces: SELECT gives columns/rows,
// DML gives affected_rows, DDL and transaction control give neither.
struct Result {
  enum Shape { NONE, QUERY, DML } shape = NONE;
  std::vector<std::string> col_names;
  std::vector<std::vector<Cell>> rows;
  int64_t affected = 0;
};

struct IVF {
  int dim = 0;
  std::vector<std::vector<float>> centroids;
  std::vector<std::vector<int64_t>> postings;
  int probes = 0;
};

struct VectorScanSnapshot {
  std::vector<Row> rows;
  std::vector<float> vector_data;
  std::vector<double> vector_norm_squared;
  std::unordered_map<int64_t, size_t> pk_to_index;
  std::vector<std::unordered_map<std::string, std::vector<size_t>>> varchar_eq_index;
  std::vector<bool> varchar_eq_index_valid;
  size_t bytes = 0;
};

class Engine {
 public:
  Engine(const std::string& dir, size_t cache_bytes, const std::string& mode);
  void open();
  void close();
  Result execute(const Stmt& st);
  void set_mode(const std::string& m) { mode_ = m; }

 private:
  Table& table(const std::string& name);
  int col_idx(const Table& t, const std::string& name);
  void persist_catalog();
  void load_catalog();

  std::vector<uint8_t> serialize_row(const Table& t, const Row& r);
  Row deserialize_row(const Table& t, const uint8_t* p);
  uint32_t rec_size(const Table& t, const uint8_t* p);

  RowRef heap_insert(const Table& t, const std::vector<uint8_t>& rec);
  RowRef heap_insert_into(const Table& t, uint32_t pid, const std::vector<uint8_t>& rec);
  bool heap_fits(const Table& t, uint32_t pid, size_t n);
  std::vector<uint8_t> read_heap_record(uint32_t first_page, uint32_t stored_len);
  void free_heap_record(uint32_t first_page, uint32_t stored_len);
  Row read_row(const Table& t, RowRef ref);
  RowRef update_row(const Table& t, RowRef ref, const std::vector<uint8_t>& rec);
  void delete_row(const Table& t, RowRef ref);
  void scan_table(const Table& t, const std::function<bool(const Row&, RowRef)>& fn);
  const VectorScanSnapshot* vector_scan_snapshot(Table& t);
  void invalidate_vector_scan_snapshot(uint16_t table_id);

  bool btree_lookup(const Table& t, int64_t pk, RowRef& out);
  void btree_insert(Table& t, int64_t pk, RowRef ref);
  void btree_remove(Table& t, int64_t pk);

  Value eval_expr(const Expr& e, const Table& t, const Row* row);
  bool eval_where(const Expr& e, const Table& t, const Row& row);
  std::string expr_text(const Expr& e);

  void build_index(Table& t);
  void index_add(Table& t, int64_t pk, const std::vector<float>& v);
  void rebuild_indexes();
  bool index_topk(Table& t, const std::vector<float>& q, int64_t k,
                  const std::function<bool(const Row&)>& filter,
                  std::vector<std::pair<double, Row>>& out);

  Result do_select(Stmt st);
  Result do_insert(const Stmt& st);
  Result do_update(const Stmt& st);
  Result do_delete(const Stmt& st);
  Result do_create_table(const Stmt& st);
  Result do_create_index(const Stmt& st);
  Value coerce(const Column& c, const Value& v);

  Store store_;
  std::string dir_;
  std::vector<Table> tables_;
  std::unordered_map<uint16_t, IVF> indexes_;
  std::unordered_map<uint16_t, VectorScanSnapshot> vector_scan_snapshots_;
  std::unordered_set<uint16_t> oversized_vector_scan_snapshots_;
  size_t vector_scan_snapshot_bytes_ = 0;
  std::string mode_;
  // The catalog lives in memory too, so page undo alone cannot restore
  // pk_root/heap_hint after a split; snapshot it around each atomic unit.
  std::vector<Table> stmt_saved_;
  std::vector<Table> txn_saved_;
  bool txn_saved_valid_ = false;
};

}  // namespace mvdb
