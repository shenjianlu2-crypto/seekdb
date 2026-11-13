#include "store.h"
#include "common.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstring>
#include <algorithm>

namespace mvdb {

namespace {
constexpr uint32_t MAGIC = 0x4244564D;
constexpr uint8_t WAL_PAGE_IMAGE = 1;
constexpr uint8_t WAL_COMMIT = 2;
constexpr uint8_t WAL_CHECKPOINT = 3;

void full_write(int fd, const void* buf, size_t n) {
  const uint8_t* p = (const uint8_t*)buf;
  while (n) {
    ssize_t w = ::write(fd, p, n);
    if (w <= 0) throw MVError(ER_INTERNAL, "write failed");
    p += w;
    n -= w;
  }
}
void full_pread(int fd, void* buf, size_t n, off_t off) {
  uint8_t* p = (uint8_t*)buf;
  while (n) {
    ssize_t r = ::pread(fd, p, n, off);
    if (r <= 0) throw MVError(ER_INTERNAL, "read failed");
    p += r;
    n -= r;
    off += r;
  }
}
}  // namespace

Store::Store(const std::string& dir, size_t cache_bytes) : dir_(dir), cache_cap_(cache_bytes) {
  ::mkdir(dir.c_str(), 0755);
  std::string dpath = dir + "/data.db";
  std::string wpath = dir + "/wal.log";
  data_fd_ = ::open(dpath.c_str(), O_RDWR | O_CREAT, 0644);
  wal_fd_ = ::open(wpath.c_str(), O_RDWR | O_CREAT, 0644);
  if (data_fd_ < 0 || wal_fd_ < 0) throw MVError(ER_INTERNAL, "cannot open data files");
  struct stat st;
  if (::fstat(data_fd_, &st) == 0) file_pages_ = (uint32_t)(st.st_size / PAGE_SIZE);
  if (::fstat(wal_fd_, &st) == 0) wal_size_ = st.st_size;
}

Store::~Store() {
  if (data_fd_ >= 0) ::close(data_fd_);
  if (wal_fd_ >= 0) ::close(wal_fd_);
}

void Store::recover() {
  if (wal_size_ > 0) {
    std::vector<uint8_t> wal(wal_size_);
    full_pread(wal_fd_, wal.data(), wal_size_, 0);
    size_t pos = 0;
    std::unordered_map<uint32_t, std::vector<uint8_t>> pending;
    while (pos + 5 <= wal.size()) {
      uint32_t len;
      memcpy(&len, wal.data() + pos, 4);
      uint8_t kind = wal[pos + 4];
      if (len < 1 || pos + 5 + len > wal.size()) break;
      const uint8_t* payload = wal.data() + pos + 5;
      if (kind == WAL_PAGE_IMAGE && len >= 4 + PAGE_SIZE) {
        uint32_t pid;
        memcpy(&pid, payload, 4);
        pending[pid].assign(payload + 4, payload + 4 + PAGE_SIZE);
      } else if (kind == WAL_COMMIT) {
        uint32_t committed_max = 0;
        for (auto& kv : pending) {
          ssize_t w = ::pwrite(data_fd_, kv.second.data(), PAGE_SIZE, (off_t)kv.first * PAGE_SIZE);
          if (w != PAGE_SIZE) throw MVError(ER_INTERNAL, "replay write failed");
          committed_max = std::max(committed_max, kv.first);
        }
        if (!pending.empty()) file_pages_ = std::max(file_pages_, committed_max + 1);
        pending.clear();
      } else if (kind == WAL_CHECKPOINT) {
        pending.clear();
      }
      pos += 5 + len;
    }
    ::fsync(data_fd_);
    if (::ftruncate(wal_fd_, 0) != 0) throw MVError(ER_INTERNAL, "wal truncate failed");
    wal_size_ = 0;
  }
  struct stat st;
  if (::fstat(data_fd_, &st) == 0) file_pages_ = std::max(file_pages_, (uint32_t)(st.st_size / PAGE_SIZE));
  if (file_pages_ == 0) {
    std::vector<uint8_t> meta(PAGE_SIZE, 0);
    uint32_t magic = MAGIC;
    memcpy(meta.data(), &magic, 4);
    next_page_ = 1;
    memcpy(meta.data() + 4, &next_page_, 4);
    full_write(data_fd_, meta.data(), PAGE_SIZE);
    ::fsync(data_fd_);
    file_pages_ = 1;
  } else {
    uint8_t meta[PAGE_SIZE];
    full_pread(data_fd_, meta, PAGE_SIZE, 0);
    uint32_t magic;
    memcpy(&magic, meta, 4);
    if (magic != MAGIC) throw MVError(ER_INTERNAL, "bad data file magic");
    memcpy(&next_page_, meta + 4, 4);
  }
}

void Store::close() {
  if (in_txn_) rollback();
  checkpoint();
  ::fsync(data_fd_);
}

Frame& Store::frame(uint32_t pid) {
  auto it = frames_.find(pid);
  if (it != frames_.end()) return it->second;
  evict_if_needed();
  Frame f;
  f.pid = pid;
  f.data.resize(PAGE_SIZE, 0);
  if (pid < file_pages_) full_pread(data_fd_, f.data.data(), PAGE_SIZE, (off_t)pid * PAGE_SIZE);
  f.pin = 0;
  cache_used_ += PAGE_SIZE;
  auto res = frames_.emplace(pid, std::move(f));
  return res.first->second;
}

uint8_t* Store::read(uint32_t pid) {
  Frame& f = frame(pid);
  f.pin++;
  if (stmt_active_) {
    bool seen = false;
    for (auto& u : stmt_undo_) if (u.pid == pid) { seen = true; break; }
    if (!seen) stmt_undo_.push_back({pid, f.data, f.dirty, f.needs_writeback});
  }
  if (in_txn_ && !txn_undo_.count(pid)) txn_undo_[pid] = {pid, f.data, f.dirty, f.needs_writeback};
  return f.data.data();
}

void Store::unpin(uint32_t pid) {
  auto it = frames_.find(pid);
  if (it == frames_.end()) return;
  Frame& f = it->second;
  if (f.pin > 0) f.pin--;
  if (f.pin == 0 && !f.dirty) {
    clean_lru_.erase(f.lru);
    f.lru = ++lru_clock_;
    clean_lru_[f.lru] = pid;
  }
}

void Store::evict_if_needed() {
  while (cache_used_ + PAGE_SIZE > cache_cap_ && !clean_lru_.empty()) {
    auto it = clean_lru_.begin();
    uint32_t pid = it->second;
    clean_lru_.erase(it);
    auto fit = frames_.find(pid);
    if (fit == frames_.end()) continue;
    Frame& f = fit->second;
    if (f.needs_writeback) writeback(f);
    cache_used_ -= PAGE_SIZE;
    frames_.erase(fit);
  }
}

void Store::writeback(Frame& f) {
  ssize_t w = ::pwrite(data_fd_, f.data.data(), PAGE_SIZE, (off_t)f.pid * PAGE_SIZE);
  if (w != PAGE_SIZE) throw MVError(ER_INTERNAL, "data writeback failed");
  if (f.pid >= file_pages_) file_pages_ = f.pid + 1;
  f.needs_writeback = false;
}

uint32_t Store::alloc() {
  uint8_t* meta = read(META_PAGE);
  uint32_t free_head = 0;
  memcpy(&free_head, meta + 12, 4);
  if (free_head) {
    uint8_t* p = read(free_head);
    uint32_t next = 0;
    memcpy(&next, p + 4, 4);
    memset(p, 0, PAGE_SIZE);
    mark_dirty(free_head);
    unpin(free_head);
    memcpy(meta + 12, &next, 4);
    mark_dirty(META_PAGE);
    unpin(META_PAGE);
    return free_head;
  }
  uint32_t pid = next_page_++;
  memcpy(meta + 4, &next_page_, 4);
  mark_dirty(META_PAGE);
  unpin(META_PAGE);
  return pid;
}

void Store::free_page(uint32_t pid) {
  if (pid == 0 || pid >= next_page_) throw MVError(ER_INTERNAL, "invalid page free");
  uint8_t* meta = read(META_PAGE);
  uint32_t free_head = 0;
  memcpy(&free_head, meta + 12, 4);
  uint8_t* p = read(pid);
  memset(p, 0, PAGE_SIZE);
  memcpy(p + 4, &free_head, 4);
  mark_dirty(pid);
  unpin(pid);
  memcpy(meta + 12, &pid, 4);
  mark_dirty(META_PAGE);
  unpin(META_PAGE);
}

void Store::set_next_page(uint32_t n) {
  next_page_ = n;
  uint8_t* meta = read(META_PAGE);
  memcpy(meta + 4, &next_page_, 4);
  mark_dirty(META_PAGE);
  unpin(META_PAGE);
}

void Store::mark_dirty(uint32_t pid) {
  Frame& f = frames_.at(pid);
  if (f.dirty) return;
  f.dirty = true;
  clean_lru_.erase(f.lru);
}

void Store::begin_txn() {
  in_txn_ = true;
  txn_undo_.clear();
  stmt_undo_.clear();
}

void Store::begin_stmt() {
  stmt_undo_.clear();
  stmt_active_ = true;
}

void Store::stmt_fail() {
  for (auto it = stmt_undo_.rbegin(); it != stmt_undo_.rend(); ++it) {
    auto fit = frames_.find(it->pid);
    if (fit == frames_.end()) continue;
    Frame& f = fit->second;
    f.data = it->image;
    f.dirty = it->dirty;
    f.needs_writeback = it->needs_writeback;
    if (!f.dirty && f.pin == 0) {
      clean_lru_.erase(f.lru);
      f.lru = ++lru_clock_;
      clean_lru_[f.lru] = f.pid;
    }
  }
  stmt_undo_.clear();
  stmt_active_ = false;
}

void Store::commit() {
  std::vector<uint32_t> dirty;
  for (auto& kv : frames_) if (kv.second.dirty) dirty.push_back(kv.first);
  std::vector<uint8_t> rec;
  for (uint32_t pid : dirty) {
    Frame& f = frames_[pid];
    uint32_t len = 4 + PAGE_SIZE;
    rec.resize(5 + len);
    memcpy(rec.data(), &len, 4);
    rec[4] = WAL_PAGE_IMAGE;
    memcpy(rec.data() + 5, &pid, 4);
    memcpy(rec.data() + 9, f.data.data(), PAGE_SIZE);
    full_write(wal_fd_, rec.data(), rec.size());
    wal_size_ += rec.size();
  }
  uint32_t len = 8;
  uint8_t crec[13];
  memcpy(crec, &len, 4);
  crec[4] = WAL_COMMIT;
  memcpy(crec + 5, &txn_id_, 8);
  full_write(wal_fd_, crec, 13);
  wal_size_ += 13;
  wal_fsync();
  for (uint32_t pid : dirty) {
    Frame& f = frames_[pid];
    f.dirty = false;
    f.needs_writeback = true;
    if (f.pin == 0) {
      f.lru = ++lru_clock_;
      clean_lru_[f.lru] = pid;
    }
  }
  txn_undo_.clear();
  stmt_undo_.clear();
  stmt_active_ = false;
  in_txn_ = false;
  txn_id_++;
}

void Store::rollback() {
  for (auto& kv : txn_undo_) {
    auto it = frames_.find(kv.first);
    if (it == frames_.end()) continue;
    Frame& f = it->second;
    f.data = kv.second.image;
    f.dirty = kv.second.dirty;
    f.needs_writeback = kv.second.needs_writeback;
    if (!f.dirty && f.pin == 0) {
      clean_lru_.erase(f.lru);
      f.lru = ++lru_clock_;
      clean_lru_[f.lru] = f.pid;
    }
  }
  txn_undo_.clear();
  stmt_undo_.clear();
  stmt_active_ = false;
  in_txn_ = false;
}

void Store::checkpoint() {
  for (auto& kv : frames_) {
    if (kv.second.needs_writeback) writeback(kv.second);
  }
  ::fsync(data_fd_);
  uint32_t len = 8;
  uint8_t rec[13];
  memcpy(rec, &len, 4);
  rec[4] = WAL_CHECKPOINT;
  uint64_t zero = 0;
  memcpy(rec + 5, &zero, 8);
  full_write(wal_fd_, rec, 13);
  wal_fsync();
  if (::ftruncate(wal_fd_, 0) != 0) throw MVError(ER_INTERNAL, "wal truncate failed");
  wal_size_ = 0;
}

void Store::wal_fsync() { ::fsync(wal_fd_); }

}  // namespace mvdb