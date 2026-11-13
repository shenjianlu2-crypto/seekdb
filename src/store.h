#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <map>
#include <vector>

namespace mvdb {

constexpr uint32_t PAGE_SIZE = 8192;
constexpr uint32_t META_PAGE = 0;

struct Frame {
  uint32_t pid = 0;
  std::vector<uint8_t> data;
  bool dirty = false;          // owned by active txn, never evicted
  bool needs_writeback = false; // committed, must reach data file before reuse
  int pin = 0;
  uint64_t lru = 0;
};

class Store {
 public:
  explicit Store(const std::string& dir, size_t cache_bytes);
  ~Store();

  void recover();
  void close();

  uint8_t* read(uint32_t pid);
  void unpin(uint32_t pid);
  uint32_t alloc();
  void free_page(uint32_t pid);
  void mark_dirty(uint32_t pid);

  void begin_txn();
  void begin_stmt();
  void stmt_fail();
  void commit();
  void rollback();
  bool in_txn() const { return in_txn_; }

  void checkpoint();
  uint32_t next_page() const { return next_page_; }
  void set_next_page(uint32_t n);

  uint64_t wal_bytes() const { return wal_size_; }

 private:
  Frame& frame(uint32_t pid);
  void evict_if_needed();
  void writeback(Frame& f);
  void wal_append(uint8_t kind, const uint8_t* payload, size_t len);
  void wal_fsync();

  std::string dir_;
  int data_fd_ = -1;
  int wal_fd_ = -1;
  uint32_t file_pages_ = 0;
  uint32_t next_page_ = 1;
  size_t cache_cap_;
  size_t cache_used_ = 0;
  uint64_t lru_clock_ = 0;
  std::unordered_map<uint32_t, Frame> frames_;
  std::map<uint64_t, uint32_t> clean_lru_;
  uint64_t wal_size_ = 0;
  uint64_t txn_id_ = 0;

  bool in_txn_ = false;
  bool stmt_active_ = false;
  struct UndoRec { uint32_t pid; std::vector<uint8_t> image; bool dirty; bool needs_writeback; };
  std::vector<UndoRec> stmt_undo_;
  std::unordered_map<uint32_t, UndoRec> txn_undo_;
};

}  // namespace mvdb