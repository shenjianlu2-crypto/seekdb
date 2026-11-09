#include "engine.h"
#include <algorithm>
#include <cstring>
#include <queue>
#include <fstream>
#include <random>
#include <unordered_set>

namespace mvdb {

namespace {
constexpr uint8_t PT_HEAP = 1;
constexpr uint8_t PT_BTREE = 2;
constexpr uint8_t PT_CATALOG = 3;
constexpr int MAX_KEYS = 500;
constexpr size_t HEAP_HDR = 16;
constexpr size_t CAT_HDR = 16;

uint16_t rd16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }
uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
uint64_t rd64(const uint8_t* p) { uint64_t v; memcpy(&v, p, 8); return v; }
void wr16(uint8_t* p, uint16_t v) { memcpy(p, &v, 2); }
void wr32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }
void wr64(uint8_t* p, uint64_t v) { memcpy(p, &v, 8); }

struct Key {
  int kind = 0;  // 0 int, 1 dbl, 2 str
  int64_t i = 0;
  double d = 0;
  std::string s;
};

int cmp_key(const Key& a, const Key& b) {
  if (a.kind == 2 || b.kind == 2) return a.s.compare(b.s);
  double x = a.kind == 0 ? (double)a.i : a.d;
  double y = b.kind == 0 ? (double)b.i : b.d;
  if (x < y) return -1;
  if (x > y) return 1;
  return 0;
}

Key value_key(const Value& v) {
  Key k;
  if (v.type == ColType::BIGINT) { k.kind = 0; k.i = v.i; }
  else if (v.type == ColType::VARCHAR) { k.kind = 2; k.s = v.s; }
  else if (v.type == (ColType)3) { k.kind = 1; k.d = v.d; }
  else { k.kind = 2; k.s = value_to_string(v); }
  return k;
}

struct Entry {
  std::vector<Key> keys;
  int64_t pk = 0;
  std::vector<std::string> out;
  size_t bytes() const {
    size_t n = pk + 8;
    for (auto& k : keys) n += 16 + k.s.size();
    for (auto& s : out) n += 8 + s.size();
    return n;
  }
};

struct Better {
  const std::vector<bool>* desc;
  bool operator()(const Entry& a, const Entry& b) const {
    for (size_t i = 0; i < a.keys.size() && i < b.keys.size(); i++) {
      int c = cmp_key(a.keys[i], b.keys[i]);
      if (c != 0) return (*desc)[i] ? c > 0 : c < 0;
    }
    return a.pk < b.pk;
  }
};

void write_entry(std::ostream& os, const Entry& e) {
  uint32_t n = e.keys.size();
  os.write((char*)&n, 4);
  for (auto& k : e.keys) {
    uint8_t kind = k.kind;
    os.write((char*)&kind, 1);
    os.write((char*)&k.i, 8);
    os.write((char*)&k.d, 8);
    uint32_t l = k.s.size();
    os.write((char*)&l, 4);
    os.write(k.s.data(), l);
  }
  os.write((char*)&e.pk, 8);
  uint32_t nc = e.out.size();
  os.write((char*)&nc, 4);
  for (auto& s : e.out) {
    uint32_t l = s.size();
    os.write((char*)&l, 4);
    os.write(s.data(), l);
  }
}

bool read_entry(std::istream& is, Entry& e) {
  uint32_t n = 0;
  if (!is.read((char*)&n, 4)) return false;
  e.keys.resize(n);
  for (auto& k : e.keys) {
    uint8_t kind = 0;
    is.read((char*)&kind, 1);
    k.kind = kind;
    is.read((char*)&k.i, 8);
    is.read((char*)&k.d, 8);
    uint32_t l = 0;
    is.read((char*)&l, 4);
    k.s.resize(l);
    is.read(&k.s[0], l);
  }
  is.read((char*)&e.pk, 8);
  uint32_t nc = 0;
  is.read((char*)&nc, 4);
  e.out.resize(nc);
  for (auto& s : e.out) {
    uint32_t l = 0;
    is.read((char*)&l, 4);
    s.resize(l);
    is.read(&s[0], l);
  }
  return (bool)is;
}
}  // namespace

Engine::Engine(const std::string& dir, size_t cache_bytes, const std::string& mode)
    : store_(dir, cache_bytes), dir_(dir), mode_(mode) {}

void Engine::open() {
  store_.recover();
  load_catalog();
  for (auto& t : tables_) if (t.has_index) build_index(t);
}

void Engine::close() { store_.close(); }

Table& Engine::table(const std::string& name) {
  for (auto& t : tables_) if (t.name == name) return t;
  throw MVError(ER_NO_TABLE, "table not found: " + name);
}

int Engine::col_idx(const Table& t, const std::string& name) {
  for (size_t i = 0; i < t.cols.size(); i++) if (t.cols[i].name == name) return (int)i;
  throw MVError(ER_NO_COLUMN, "column not found: " + name);
}

void Engine::load_catalog() {
  uint8_t* meta = store_.read(META_PAGE);
  uint32_t head = rd32(meta + 8);
  store_.unpin(META_PAGE);
  tables_.clear();
  uint32_t pid = head;
  std::vector<uint8_t> blob;
  while (pid) {
    uint8_t* p = store_.read(pid);
    if (p[0] != PT_CATALOG) { store_.unpin(pid); break; }
    uint32_t used = rd32(p + 8);
    blob.insert(blob.end(), p + CAT_HDR, p + CAT_HDR + used);
    uint32_t next = rd32(p + 4);
    store_.unpin(pid);
    pid = next;
  }
  size_t pos = 0;
  auto take = [&](void* dst, size_t n) {
    if (pos + n > blob.size()) throw MVError(ER_INTERNAL, "catalog corrupt");
    memcpy(dst, blob.data() + pos, n);
    pos += n;
  };
  auto take_str = [&]() {
    uint16_t l;
    take(&l, 2);
    std::string s(l, '\0');
    if (l) take(&s[0], l);
    return s;
  };
  while (pos < blob.size()) {
    Table t;
    take(&t.id, 2);
    t.name = take_str();
    take(&t.pk_root, 4);
    uint16_t vc;
    take(&vc, 2);
    t.vec_col = vc == 0xFFFF ? -1 : (int)vc;
    uint8_t hi;
    take(&hi, 1);
    t.has_index = hi;
    t.index_name = take_str();
    uint16_t nc;
    take(&nc, 2);
    for (int i = 0; i < nc; i++) {
      Column c;
      c.name = take_str();
      uint8_t ty;
      take(&ty, 1);
      c.type = (ColType)ty;
      take(&c.len, 4);
      uint8_t pk;
      take(&pk, 1);
      c.pk = pk;
      if (c.pk) t.pk_col = i;
      t.cols.push_back(c);
    }
    tables_.push_back(t);
  }
}

void Engine::persist_catalog() {
  std::vector<uint8_t> blob;
  auto put = [&](const void* src, size_t n) {
    const uint8_t* p = (const uint8_t*)src;
    blob.insert(blob.end(), p, p + n);
  };
  auto put_str = [&](const std::string& s) {
    uint16_t l = (uint16_t)s.size();
    put(&l, 2);
    put(s.data(), s.size());
  };
  for (auto& t : tables_) {
    put(&t.id, 2);
    put_str(t.name);
    put(&t.pk_root, 4);
    uint16_t vc = t.vec_col < 0 ? 0xFFFF : (uint16_t)t.vec_col;
    put(&vc, 2);
    uint8_t hi = t.has_index;
    put(&hi, 1);
    put_str(t.index_name);
    uint16_t nc = (uint16_t)t.cols.size();
    put(&nc, 2);
    for (auto& c : t.cols) {
      put_str(c.name);
      uint8_t ty = (uint8_t)c.type;
      put(&ty, 1);
      put(&c.len, 4);
      uint8_t pk = c.pk;
      put(&pk, 1);
    }
  }
  uint32_t head = 0, prev = 0;
  size_t pos = 0;
  if (blob.empty()) {
    head = 0;
  } else {
    while (pos < blob.size()) {
      uint32_t pid = store_.alloc();
      uint8_t* p = store_.read(pid);
      p[0] = PT_CATALOG;
      size_t n = std::min(blob.size() - pos, PAGE_SIZE - CAT_HDR);
      wr32(p + 8, (uint32_t)n);
      memcpy(p + CAT_HDR, blob.data() + pos, n);
      store_.mark_dirty(pid);
      store_.unpin(pid);
      if (prev) {
        uint8_t* pp = store_.read(prev);
        wr32(pp + 4, pid);
        store_.mark_dirty(prev);
        store_.unpin(prev);
      } else {
        head = pid;
      }
      prev = pid;
      pos += n;
    }
  }
  uint8_t* meta = store_.read(META_PAGE);
  wr32(meta + 8, head);
  store_.mark_dirty(META_PAGE);
  store_.unpin(META_PAGE);
}

std::vector<uint8_t> Engine::serialize_row(const Table& t, const Row& r) {
  std::vector<uint8_t> out;
  auto put = [&](const void* src, size_t n) {
    const uint8_t* p = (const uint8_t*)src;
    out.insert(out.end(), p, p + n);
  };
  put(&r.pk, 8);
  for (size_t i = 0; i < t.cols.size(); i++) {
    const Value& v = r.vals[i];
    switch (t.cols[i].type) {
      case ColType::BIGINT: put(&v.i, 8); break;
      case ColType::VARCHAR: {
        uint32_t l = (uint32_t)v.s.size();
        put(&l, 4);
        put(v.s.data(), v.s.size());
        break;
      }
      case ColType::VECTOR: put(v.v.data(), v.v.size() * 4); break;
      default: throw MVError(ER_INTERNAL, "bad column type");
    }
  }
  return out;
}

Row Engine::deserialize_row(const Table& t, const uint8_t* p) {
  Row r;
  r.pk = (int64_t)rd64(p);
  p += 8;
  for (auto& c : t.cols) {
    Value v;
    switch (c.type) {
      case ColType::BIGINT:
        v = Value::bigint((int64_t)rd64(p));
        p += 8;
        break;
      case ColType::VARCHAR: {
        uint32_t l = rd32(p);
        p += 4;
        v = Value::varchar(std::string((const char*)p, l));
        p += l;
        break;
      }
      case ColType::VECTOR: {
        v.type = ColType::VECTOR;
        v.v.resize(c.len);
        memcpy(v.v.data(), p, c.len * 4);
        p += c.len * 4;
        break;
      }
      default: throw MVError(ER_INTERNAL, "bad column type");
    }
    r.vals.push_back(v);
  }
  return r;
}

uint32_t Engine::rec_size(const Table& t, const uint8_t* p) {
  const uint8_t* start = p;
  p += 8;
  for (auto& c : t.cols) {
    switch (c.type) {
      case ColType::BIGINT: p += 8; break;
      case ColType::VARCHAR: p += 4 + rd32(p); break;
      case ColType::VECTOR: p += c.len * 4; break;
      default: throw MVError(ER_INTERNAL, "bad column type");
    }
  }
  return (uint32_t)(p - start);
}

bool Engine::heap_fits(const Table& t, uint32_t pid, size_t n) {
  if (pid == 0 || pid >= store_.next_page()) return false;
  uint8_t* p = store_.read(pid);
  bool ok = false;
  if (p[0] == PT_HEAP && rd16(p + 2) == t.id) {
    uint16_t sc = rd16(p + 4);
    uint32_t free_end = rd32(p + 8);
    ok = free_end >= HEAP_HDR + 8 * (sc + 1) + n;
  }
  store_.unpin(pid);
  return ok;
}

RowRef Engine::heap_insert_into(const Table& t, uint32_t pid, const std::vector<uint8_t>& rec) {
  uint8_t* p = store_.read(pid);
  uint16_t sc = rd16(p + 4);
  uint32_t free_end = rd32(p + 8);
  uint32_t off = free_end - rec.size();
  memcpy(p + off, rec.data(), rec.size());
  wr32(p + HEAP_HDR + 8 * sc, off);
  wr32(p + HEAP_HDR + 8 * sc + 4, (uint32_t)rec.size());
  wr16(p + 4, sc + 1);
  wr32(p + 8, off);
  store_.mark_dirty(pid);
  store_.unpin(pid);
  return {pid, sc};
}

RowRef Engine::heap_insert(const Table& t, const std::vector<uint8_t>& rec) {
  Table& mt = const_cast<Table&>(t);
  if (mt.heap_hint && heap_fits(t, mt.heap_hint, rec.size()))
    return heap_insert_into(t, mt.heap_hint, rec);
  if (mt.heap_hint == 0) {
    for (uint32_t pid = 1; pid < store_.next_page(); pid++) {
      if (heap_fits(t, pid, rec.size())) mt.heap_hint = pid;
    }
    if (mt.heap_hint) return heap_insert_into(t, mt.heap_hint, rec);
  }
  uint32_t pid = store_.alloc();
  uint8_t* p = store_.read(pid);
  p[0] = PT_HEAP;
  wr16(p + 2, t.id);
  wr16(p + 4, 0);
  wr32(p + 8, PAGE_SIZE);
  store_.mark_dirty(pid);
  store_.unpin(pid);
  mt.heap_hint = pid;
  return heap_insert_into(t, pid, rec);
}

Row Engine::read_row(const Table& t, RowRef ref) {
  uint8_t* p = store_.read(ref.page);
  uint32_t off = rd32(p + HEAP_HDR + 8 * ref.slot);
  Row r = deserialize_row(t, p + off);
  store_.unpin(ref.page);
  return r;
}

RowRef Engine::update_row(const Table& t, RowRef ref, const std::vector<uint8_t>& rec) {
  uint8_t* p = store_.read(ref.page);
  uint32_t off = rd32(p + HEAP_HDR + 8 * ref.slot);
  uint32_t old_size = rd32(p + HEAP_HDR + 8 * ref.slot + 4);
  if (rec.size() <= old_size) {
    memcpy(p + off, rec.data(), rec.size());
    wr32(p + HEAP_HDR + 8 * ref.slot + 4, (uint32_t)rec.size());
    store_.mark_dirty(ref.page);
    store_.unpin(ref.page);
    return ref;
  }
  wr32(p + HEAP_HDR + 8 * ref.slot, 0);
  store_.mark_dirty(ref.page);
  store_.unpin(ref.page);
  return heap_insert(t, rec);
}

void Engine::delete_row(const Table& t, RowRef ref) {
  uint8_t* p = store_.read(ref.page);
  wr32(p + HEAP_HDR + 8 * ref.slot, 0);
  store_.mark_dirty(ref.page);
  store_.unpin(ref.page);
}

void Engine::scan_table(const Table& t, const std::function<bool(const Row&, RowRef)>& fn) {
  for (uint32_t pid = 1; pid < store_.next_page(); pid++) {
    uint8_t* p = store_.read(pid);
    if (p[0] != PT_HEAP || rd16(p + 2) != t.id) {
      store_.unpin(pid);
      continue;
    }
    uint16_t sc = rd16(p + 4);
    for (uint32_t s = 0; s < sc; s++) {
      uint32_t off = rd32(p + HEAP_HDR + 8 * s);
      if (!off) continue;
      Row r = deserialize_row(t, p + off);
      store_.unpin(pid);
      bool cont = fn(r, {pid, s});
      if (!cont) return;
      p = store_.read(pid);
    }
    store_.unpin(pid);
  }
}

bool Engine::btree_lookup(const Table& t, int64_t pk, RowRef& out) {
  uint32_t node = t.pk_root;
  while (node) {
    uint8_t* p = store_.read(node);
    uint8_t leaf = p[1];
    uint16_t count = rd16(p + 2);
    const uint8_t* keys = p + 8;
    if (leaf) {
      for (uint16_t i = 0; i < count; i++) {
        int64_t k = (int64_t)rd64(keys + 8 * i);
        if (k == pk) {
          const uint8_t* vals = keys + 8 * count;
          out.page = rd32(vals + 8 * i);
          out.slot = rd32(vals + 8 * i + 4);
          store_.unpin(node);
          return true;
        }
        if (k > pk) break;
      }
      store_.unpin(node);
      return false;
    }
    const uint8_t* children = keys + 8 * count;
    uint16_t i = 0;
    while (i < count && (int64_t)rd64(keys + 8 * i) <= pk) i++;
    uint32_t child = rd32(children + 4 * i);
    store_.unpin(node);
    node = child;
  }
  return false;
}

struct BTSplit {
  bool has = false;
  int64_t key = 0;
  uint32_t pid = 0;
};

static BTSplit bt_insert_rec(Store& store, uint32_t node, int64_t pk, RowRef ref) {
  uint8_t* p = store.read(node);
  uint8_t leaf = p[1];
  uint16_t count = rd16(p + 2);
  uint8_t* keys = p + 8;
  BTSplit split;
  if (leaf) {
    uint8_t* vals = keys + 8 * count;
    uint16_t pos = 0;
    while (pos < count && (int64_t)rd64(keys + 8 * pos) < pk) pos++;
    if (count < MAX_KEYS) {
      uint8_t* new_vals = keys + 8 * (count + 1);
      memmove(new_vals, vals, 8 * count);
      memmove(keys + 8 * (pos + 1), keys + 8 * pos, 8 * (count - pos));
      memmove(new_vals + 8 * (pos + 1), new_vals + 8 * pos, 8 * (count - pos));
      wr64(keys + 8 * pos, (uint64_t)pk);
      wr32(new_vals + 8 * pos, ref.page);
      wr32(new_vals + 8 * pos + 4, ref.slot);
      wr16(p + 2, count + 1);
      store.mark_dirty(node);
      store.unpin(node);
      return split;
    }
    uint32_t np = store.alloc();
    uint8_t* npage = store.read(np);
    npage[0] = PT_BTREE;
    npage[1] = 1;
    wr32(npage + 4, rd32(p + 4));
    wr32(p + 4, np);
    uint16_t left = (count + 1) / 2;
    std::vector<int64_t> allk(count + 1);
    std::vector<RowRef> allv(count + 1);
    for (uint16_t i = 0; i < count; i++) {
      allk[i] = (int64_t)rd64(keys + 8 * i);
      allv[i] = {rd32(vals + 8 * i), rd32(vals + 8 * i + 4)};
    }
    allk.insert(allk.begin() + pos, pk);
    allv.insert(allv.begin() + pos, ref);
    wr16(p + 2, left);
    wr16(npage + 2, (uint16_t)(allk.size() - left));
    for (uint16_t i = 0; i < left; i++) {
      wr64(keys + 8 * i, (uint64_t)allk[i]);
      wr32(keys + 8 * left + 8 * i, allv[i].page);
      wr32(keys + 8 * left + 8 * i + 4, allv[i].slot);
    }
    uint8_t* nkeys = npage + 8;
    for (size_t i = left; i < allk.size(); i++) {
      wr64(nkeys + 8 * (i - left), (uint64_t)allk[i]);
      wr32(nkeys + 8 * (allk.size() - left) + 8 * (i - left), allv[i].page);
      wr32(nkeys + 8 * (allk.size() - left) + 8 * (i - left) + 4, allv[i].slot);
    }
    store.mark_dirty(node);
    store.mark_dirty(np);
    store.unpin(np);
    store.unpin(node);
    split.has = true;
    split.key = allk[left];
    split.pid = np;
    return split;
  }
  uint8_t* children = keys + 8 * count;
  uint16_t i = 0;
  while (i < count && (int64_t)rd64(keys + 8 * i) <= pk) i++;
  uint32_t child = rd32(children + 4 * i);
  store.unpin(node);
  BTSplit cs = bt_insert_rec(store, child, pk, ref);
  if (!cs.has) return split;
  p = store.read(node);
  count = rd16(p + 2);
  keys = p + 8;
  children = keys + 8 * count;
  i = 0;
  while (i < count && (int64_t)rd64(keys + 8 * i) <= pk) i++;
  if (count < MAX_KEYS) {
    uint8_t* new_children = keys + 8 * (count + 1);
    memmove(new_children, children, 4 * (count + 1));
    memmove(keys + 8 * (i + 1), keys + 8 * i, 8 * (count - i));
    memmove(new_children + 4 * (i + 2), new_children + 4 * (i + 1), 4 * (count - i));
    wr64(keys + 8 * i, (uint64_t)cs.key);
    wr32(new_children + 4 * (i + 1), cs.pid);
    wr16(p + 2, count + 1);
    store.mark_dirty(node);
    store.unpin(node);
    return split;
  }
  uint32_t np = store.alloc();
  uint8_t* npage = store.read(np);
  npage[0] = PT_BTREE;
  npage[1] = 0;
  std::vector<int64_t> allk(count + 1);
  std::vector<uint32_t> allc(count + 2);
  for (uint16_t j = 0; j < count; j++) allk[j] = (int64_t)rd64(keys + 8 * j);
  for (uint16_t j = 0; j <= count; j++) allc[j] = rd32(children + 4 * j);
  allk.insert(allk.begin() + i, cs.key);
  allc.insert(allc.begin() + i + 1, cs.pid);
  uint16_t left = count / 2;
  int64_t up = allk[left];
  wr16(p + 2, left);
  for (uint16_t j = 0; j < left; j++) wr64(keys + 8 * j, (uint64_t)allk[j]);
  for (uint16_t j = 0; j <= left; j++) wr32(keys + 8 * left + 4 * j, allc[j]);
  uint16_t rc = (uint16_t)(allk.size() - left - 1);
  wr16(npage + 2, rc);
  uint8_t* nkeys = npage + 8;
  for (uint16_t j = 0; j < rc; j++) wr64(nkeys + 8 * j, (uint64_t)allk[left + 1 + j]);
  for (uint16_t j = 0; j <= rc; j++) wr32(nkeys + 8 * rc + 4 * j, allc[left + 1 + j]);
  store.mark_dirty(node);
  store.mark_dirty(np);
  store.unpin(np);
  store.unpin(node);
  split.has = true;
  split.key = up;
  split.pid = np;
  return split;
}

void Engine::btree_insert(Table& t, int64_t pk, RowRef ref) {
  BTSplit s = bt_insert_rec(store_, t.pk_root, pk, ref);
  if (s.has) {
    uint32_t nr = store_.alloc();
    uint8_t* p = store_.read(nr);
    p[0] = PT_BTREE;
    p[1] = 0;
    wr16(p + 2, 1);
    wr64(p + 8, (uint64_t)s.key);
    wr32(p + 16, t.pk_root);
    wr32(p + 20, s.pid);
    store_.mark_dirty(nr);
    store_.unpin(nr);
    t.pk_root = nr;
    persist_catalog();
  }
}

static bool bt_remove_rec(Store& store, uint32_t node, int64_t pk) {
  uint8_t* p = store.read(node);
  uint8_t leaf = p[1];
  uint16_t count = rd16(p + 2);
  uint8_t* keys = p + 8;
  if (leaf) {
    uint8_t* vals = keys + 8 * count;
    for (uint16_t i = 0; i < count; i++) {
      if ((int64_t)rd64(keys + 8 * i) == pk) {
        memmove(keys + 8 * i, keys + 8 * (i + 1), 8 * (count - i - 1));
        memmove(vals + 8 * i, vals + 8 * (i + 1), 8 * (count - i - 1));
        wr16(p + 2, count - 1);
        store.mark_dirty(node);
        store.unpin(node);
        return true;
      }
    }
    store.unpin(node);
    return false;
  }
  const uint8_t* children = keys + 8 * count;
  uint16_t i = 0;
  while (i < count && (int64_t)rd64(keys + 8 * i) <= pk) i++;
  uint32_t child = rd32(children + 4 * i);
  store.unpin(node);
  bool removed = bt_remove_rec(store, child, pk);
  if (removed) {
    uint8_t* cp = store.read(child);
    uint16_t cc = rd16(cp + 2);
    store.unpin(child);
    if (cc == 0) {
      p = store.read(node);
      count = rd16(p + 2);
      keys = p + 8;
      children = keys + 8 * count;
      if (count == 0) {
        store.unpin(node);
        return true;  // caller collapses root
      }
    }
  }
  return removed;
}

void Engine::btree_remove(Table& t, int64_t pk) {
  bt_remove_rec(store_, t.pk_root, pk);
  uint8_t* p = store_.read(t.pk_root);
  bool leaf = p[1];
  uint16_t count = rd16(p + 2);
  if (!leaf && count == 0) {
    uint32_t child = rd32(p + 8);
    store_.unpin(t.pk_root);
    t.pk_root = child;
    persist_catalog();
  } else {
    store_.unpin(t.pk_root);
  }
}

Value Engine::coerce(const Column& c, const Value& v) {
  switch (c.type) {
    case ColType::BIGINT:
      if (v.type != ColType::BIGINT) throw MVError(ER_BAD_VALUE, "expected integer for " + c.name);
      return v;
    case ColType::VARCHAR: {
      std::string s = v.s;
      if (v.type != ColType::VARCHAR && v.type != ColType::VECTOR)
        throw MVError(ER_BAD_VALUE, "expected string for " + c.name);
      if (utf8_len(s) > (size_t)c.len) throw MVError(ER_TOO_LONG, "value too long for " + c.name);
      return Value::varchar(s);
    }
    case ColType::VECTOR: {
      std::vector<float> vec;
      if (v.type == ColType::VECTOR) vec = v.v;
      else if (v.type == ColType::VARCHAR) vec = parse_vector_literal(v.s);
      else throw MVError(ER_BAD_VALUE, "expected vector for " + c.name);
      if ((int)vec.size() != c.len) throw MVError(ER_BAD_VALUE, "vector dimension mismatch for " + c.name);
      return Value::vector(vec);
    }
    default: throw MVError(ER_INTERNAL, "bad type");
  }
}

Value Engine::eval_expr(const Expr& e, const Table& t, const Row* row) {
  if (e.kind == Expr::COL) {
    if (!row) throw MVError(ER_INTERNAL, "column without row");
    return row->vals[col_idx(t, e.name)];
  }
  if (e.kind == Expr::LIT) return e.lit;
  if (e.kind == Expr::L2) {
    Value a = eval_expr(*e.a, t, row);
    Value b = eval_expr(*e.b, t, row);
    auto getv = [](const Value& v) {
      if (v.type == ColType::VECTOR) return v.v;
      if (v.type == ColType::VARCHAR) return parse_vector_literal(v.s);
      throw MVError(ER_BAD_VALUE, "L2_DISTANCE expects vectors");
    };
    std::vector<float> va = getv(a), vb = getv(b);
    if (va.size() != vb.size()) throw MVError(ER_BAD_VALUE, "vector dimension mismatch");
    Value r;
    r.type = (ColType)3;
    r.d = l2_distance(va, vb);
    return r;
  }
  throw MVError(ER_INTERNAL, "bad expr");
}

bool Engine::eval_where(const Expr& e, const Table& t, const Row& row) {
  if (e.kind == Expr::LIT && e.a) {
    if (e.name == "AND") return eval_where(*e.a, t, row) && eval_where(*e.b, t, row);
    Value a = eval_expr(*e.a, t, &row);
    Value b = eval_expr(*e.b, t, &row);
    int c;
    if (a.type == ColType::VARCHAR && b.type == ColType::VARCHAR) c = a.s.compare(b.s);
    else if (a.type == ColType::BIGINT && b.type == ColType::BIGINT) c = a.i < b.i ? -1 : a.i > b.i ? 1 : 0;
    else if (a.type == (ColType)3 || b.type == (ColType)3) {
      double x = a.type == ColType::BIGINT ? (double)a.i : a.d;
      double y = b.type == ColType::BIGINT ? (double)b.i : b.d;
      c = x < y ? -1 : x > y ? 1 : 0;
    } else throw MVError(ER_BAD_VALUE, "type mismatch in comparison");
    if (e.name == "=") return c == 0;
    if (e.name == "<") return c < 0;
    if (e.name == "<=") return c <= 0;
    if (e.name == ">") return c > 0;
    if (e.name == ">=") return c >= 0;
    throw MVError(ER_INTERNAL, "bad op");
  }
  throw MVError(ER_SYNTAX, "bad where expression");
}

std::string Engine::expr_text(const Expr& e) {
  if (e.kind == Expr::COL) return e.name;
  if (e.kind == Expr::LIT) return value_to_string(e.lit);
  return "l2_distance(" + expr_text(*e.a) + ", " + expr_text(*e.b) + ")";
}

void Engine::build_index(Table& t) {
  IVF ivf;
  ivf.dim = t.cols[t.vec_col].len;
  std::vector<std::pair<int64_t, std::vector<float>>> sample;
  int64_t n = 0;
  std::mt19937 rng(42);
  scan_table(t, [&](const Row& r, RowRef) {
    n++;
    if ((int64_t)sample.size() < 4096) sample.push_back({r.pk, r.vals[t.vec_col].v});
    else {
      uint64_t j = rng() % n;
      if (j < 4096) sample[j] = {r.pk, r.vals[t.vec_col].v};
    }
    return true;
  });
  indexes_[t.id] = ivf;
  if (n == 0 || sample.empty()) return;
  int k = (int)std::min<int64_t>(std::max<int64_t>((int64_t)std::sqrt((double)n), 16), 256);
  k = std::min<int>(k, (int)sample.size());
  ivf.centroids.resize(k);
  for (int i = 0; i < k; i++) ivf.centroids[i] = sample[(size_t)i * sample.size() / k].second;
  std::vector<int> assign(sample.size(), 0);
  for (int iter = 0; iter < 12; iter++) {
    for (size_t si = 0; si < sample.size(); si++) {
      int best = 0;
      double bd = 1e300;
      for (int c = 0; c < k; c++) {
        double d = l2_distance(sample[si].second, ivf.centroids[c]);
        if (d < bd) { bd = d; best = c; }
      }
      assign[si] = best;
    }
    std::vector<std::vector<double>> sum(k, std::vector<double>(ivf.dim, 0));
    std::vector<int> cnt(k, 0);
    for (size_t si = 0; si < sample.size(); si++) {
      cnt[assign[si]]++;
      for (int d = 0; d < ivf.dim; d++) sum[assign[si]][d] += sample[si].second[d];
    }
    for (int c = 0; c < k; c++) {
      if (cnt[c]) for (int d = 0; d < ivf.dim; d++) ivf.centroids[c][d] = (float)(sum[c][d] / cnt[c]);
    }
  }
  ivf.postings.resize(k);
  scan_table(t, [&](const Row& r, RowRef) {
    int best = 0;
    double bd = 1e300;
    for (int c = 0; c < k; c++) {
      double d = l2_distance(r.vals[t.vec_col].v, ivf.centroids[c]);
      if (d < bd) { bd = d; best = c; }
    }
    ivf.postings[best].push_back(r.pk);
    return true;
  });
  ivf.probes = std::max(16, (k * 9) / 10);
  indexes_[t.id] = std::move(ivf);
}

void Engine::index_add(Table& t, int64_t pk, const std::vector<float>& v) {
  auto it = indexes_.find(t.id);
  if (it == indexes_.end()) return;
  IVF& ivf = it->second;
  if (ivf.centroids.empty()) {
    ivf.centroids.push_back(v);
    ivf.postings.resize(1);
    ivf.probes = 1;
  }
  int best = 0;
  double bd = 1e300;
  for (size_t c = 0; c < ivf.centroids.size(); c++) {
    double d = l2_distance(v, ivf.centroids[c]);
    if (d < bd) { bd = d; best = (int)c; }
  }
  ivf.postings[best].push_back(pk);
}

bool Engine::index_topk(Table& t, const std::vector<float>& q, int64_t k,
                        const std::function<bool(const Row&)>& filter,
                        std::vector<std::pair<double, Row>>& out) {
  auto it = indexes_.find(t.id);
  if (it == indexes_.end()) return false;
  IVF& ivf = it->second;
  if (ivf.centroids.empty()) return true;
  int probes = std::min<int>(ivf.probes, (int)ivf.centroids.size());
  std::vector<std::pair<double, int>> cd;
  for (size_t c = 0; c < ivf.centroids.size(); c++)
    cd.push_back({l2_distance(q, ivf.centroids[c]), (int)c});
  std::partial_sort(cd.begin(), cd.begin() + probes, cd.end(),
                    [](const auto& a, const auto& b) { return a.first < b.first; });
  std::unordered_set<int64_t> seen;
  struct Cmp {
    bool operator()(const std::pair<double, Row>& a, const std::pair<double, Row>& b) const {
      if (a.first != b.first) return a.first < b.first;
      return a.second.pk < b.second.pk;
    }
  };
  std::priority_queue<std::pair<double, Row>, std::vector<std::pair<double, Row>>, Cmp> heap;
  for (int pi = 0; pi < probes; pi++) {
    for (int64_t pk : ivf.postings[cd[pi].second]) {
      if (!seen.insert(pk).second) continue;
      RowRef ref;
      if (!btree_lookup(t, pk, ref)) continue;
      Row r = read_row(t, ref);
      if (filter && !filter(r)) continue;
      double d = l2_distance(r.vals[t.vec_col].v, q);
      if ((int64_t)heap.size() < k) heap.push({d, r});
      else {
        const auto& top = heap.top();
        if (d < top.first || (d == top.first && r.pk < top.second.pk)) {
          heap.pop();
          heap.push({d, r});
        }
      }
    }
  }
  while (!heap.empty()) {
    out.push_back(heap.top());
    heap.pop();
  }
  std::reverse(out.begin(), out.end());
  return true;
}

Result Engine::do_create_table(const Stmt& st) {
  for (auto& t : tables_) if (t.name == st.table) throw MVError(ER_BAD_VALUE, "table exists");
  Table t;
  t.id = (uint16_t)(tables_.size() + 1);
  t.name = st.table;
  for (auto& cd : st.cols) t.cols.push_back(Column{cd.name, cd.type, cd.len, cd.pk});
  int pk_count = 0, vec_count = 0;
  for (size_t i = 0; i < t.cols.size(); i++) {
    if (t.cols[i].pk) {
      pk_count++;
      t.pk_col = (int)i;
      if (t.cols[i].type != ColType::BIGINT) throw MVError(ER_UNSUPPORTED, "primary key must be BIGINT");
    }
    if (t.cols[i].type == ColType::VECTOR) { vec_count++; t.vec_col = (int)i; }
  }
  if (pk_count != 1) throw MVError(ER_UNSUPPORTED, "exactly one BIGINT primary key required");
  if (vec_count > 1) throw MVError(ER_UNSUPPORTED, "at most one vector column");
  uint32_t root = store_.alloc();
  uint8_t* p = store_.read(root);
  p[0] = PT_BTREE;
  p[1] = 1;
  wr16(p + 2, 0);
  store_.mark_dirty(root);
  store_.unpin(root);
  t.pk_root = root;
  tables_.push_back(t);
  persist_catalog();
  return {true, 0, "", {}, {}};
}

Result Engine::do_create_index(const Stmt& st) {
  Table& t = table(st.table);
  int ci = col_idx(t, st.cols[0].name);
  if (t.cols[ci].type != ColType::VECTOR) throw MVError(ER_BAD_VALUE, "vector index requires vector column");
  if (t.vec_col != ci) throw MVError(ER_BAD_VALUE, "vector index column mismatch");
  t.has_index = true;
  t.index_name = st.index;
  build_index(t);
  persist_catalog();
  return {true, 0, "", {}, {}};
}

Result Engine::do_insert(const Stmt& st) {
  Table& t = table(st.table);
  std::vector<int> map(t.cols.size(), -1);
  if (!st.cols.empty()) {
    if (st.cols.size() != t.cols.size()) throw MVError(ER_BAD_VALUE, "column count mismatch");
    for (size_t i = 0; i < st.cols.size(); i++) map[col_idx(t, st.cols[i].name)] = (int)i;
  } else {
    for (size_t i = 0; i < t.cols.size(); i++) map[i] = (int)i;
  }
  for (auto& rv : st.rows) {
    if (rv.size() != t.cols.size()) throw MVError(ER_BAD_VALUE, "value count mismatch");
    Row r;
    r.vals.resize(t.cols.size());
    for (size_t i = 0; i < t.cols.size(); i++) r.vals[i] = coerce(t.cols[i], rv[map[i]]);
    r.pk = r.vals[t.pk_col].i;
    RowRef dummy;
    if (btree_lookup(t, r.pk, dummy)) throw MVError(ER_DUP_PK, "duplicate primary key");
    auto rec = serialize_row(t, r);
    RowRef ref = heap_insert(t, rec);
    btree_insert(t, r.pk, ref);
    if (t.has_index) index_add(t, r.pk, r.vals[t.vec_col].v);
  }
  return {true, 0, "", {}, {}};
}

Result Engine::do_update(const Stmt& st) {
  Table& t = table(st.table);
  for (auto& a : st.assigns) {
    int ci = col_idx(t, a.first);
    if (ci == t.pk_col) throw MVError(ER_UNSUPPORTED, "primary key update not supported");
  }
  std::vector<std::pair<RowRef, Row>> matches;
  scan_table(t, [&](const Row& r, RowRef ref) {
    if (!st.has_where || eval_where(*st.where, t, r)) matches.push_back({ref, r});
    return true;
  });
  for (auto& m : matches) {
    Row r = m.second;
    bool vec_changed = false;
    for (auto& a : st.assigns) {
      int ci = col_idx(t, a.first);
      Value nv = coerce(t.cols[ci], a.second);
      if (ci == t.vec_col) vec_changed = true;
      r.vals[ci] = nv;
    }
    auto rec = serialize_row(t, r);
    RowRef nref = update_row(t, m.first, rec);
    if (nref.page != m.first.page || nref.slot != m.first.slot) {
      btree_remove(t, r.pk);
      btree_insert(t, r.pk, nref);
    }
    if (vec_changed && t.has_index) index_add(t, r.pk, r.vals[t.vec_col].v);
  }
  return {true, 0, "", {}, {}};
}

Result Engine::do_delete(const Stmt& st) {
  Table& t = table(st.table);
  std::vector<std::pair<RowRef, Row>> matches;
  scan_table(t, [&](const Row& r, RowRef ref) {
    if (!st.has_where || eval_where(*st.where, t, r)) matches.push_back({ref, r});
    return true;
  });
  for (auto& m : matches) {
    delete_row(t, m.first);
    btree_remove(t, m.second.pk);
  }
  return {true, 0, "", {}, {}};
}

Result Engine::do_select(Stmt st) {
  Table& t = table(st.table);
  std::unordered_map<std::string, ExprPtr> aliases;
  for (auto& it : st.items)
    if (!it.star && !it.alias.empty()) aliases[it.alias] = it.expr;
  for (auto& o : st.order)
    if (o.expr->kind == Expr::COL && aliases.count(o.expr->name)) o.expr = aliases[o.expr->name];
  Result res;
  res.ok = true;
  std::vector<std::string> names;
  std::vector<const SelectItem*> items;
  for (auto& it : st.items) {
    if (it.star) {
      for (auto& c : t.cols) {
        names.push_back(c.name);
        SelectItem* si = new SelectItem();
        si->expr = std::make_shared<Expr>();
        si->expr->kind = Expr::COL;
        si->expr->name = c.name;
        items.push_back(si);
      }
    } else {
      names.push_back(it.alias.empty() ? (it.expr->kind == Expr::COL ? it.expr->name : expr_text(*it.expr)) : it.alias);
      items.push_back(&it);
    }
  }
  res.col_names = names;
  auto project = [&](const Row& r) {
    std::vector<std::string> out;
    for (auto* it : items) out.push_back(value_to_string(eval_expr(*it->expr, t, &r)));
    return out;
  };
  auto filter = [&](const Row& r) { return !st.has_where || eval_where(*st.where, t, r); };

  bool vec_topk = false;
  const Expr* vexpr = nullptr;
  if (st.order.size() == 1 && st.order[0].expr->kind == Expr::L2 && st.limit >= 0) {
    vexpr = st.order[0].expr.get();
    vec_topk = true;
  }
  if (vec_topk) {
    const Expr* a = vexpr->a.get();
    const Expr* b = vexpr->b.get();
    const Expr* colside = nullptr;
    const Expr* litside = nullptr;
    if (a->kind == Expr::COL && b->kind == Expr::LIT) { colside = a; litside = b; }
    else if (b->kind == Expr::COL && a->kind == Expr::LIT) { colside = b; litside = a; }
    if (colside && litside && col_idx(t, colside->name) == t.vec_col) {
      std::vector<float> q = parse_vector_literal(litside->lit.s);
      if ((int)q.size() != t.cols[t.vec_col].len) throw MVError(ER_BAD_VALUE, "query vector dimension mismatch");
      std::vector<std::pair<double, Row>> top;
      bool used_index = false;
      if (mode_ == "ann" && t.has_index)
        used_index = index_topk(t, q, st.limit, filter, top);
      if (!used_index) {
        struct Cmp {
          bool operator()(const std::pair<double, Row>& x, const std::pair<double, Row>& y) const {
            if (x.first != y.first) return x.first < y.first;
            return x.second.pk < y.second.pk;
          }
        };
        std::priority_queue<std::pair<double, Row>, std::vector<std::pair<double, Row>>, Cmp> heap;
        scan_table(t, [&](const Row& r, RowRef) {
          if (!filter(r)) return true;
          double d = l2_distance(r.vals[t.vec_col].v, q);
          if ((int64_t)heap.size() < st.limit) heap.push({d, r});
          else {
            const auto& top0 = heap.top();
            if (d < top0.first || (d == top0.first && r.pk < top0.second.pk)) {
              heap.pop();
              heap.push({d, r});
            }
          }
          return true;
        });
        while (!heap.empty()) { top.push_back(heap.top()); heap.pop(); }
        std::reverse(top.begin(), top.end());
      }
      for (auto& pr : top) {
        res.rows.push_back(project(pr.second));
      }
      for (auto* it : items) if (it->star) delete it;
      return res;
    }
  }

  std::vector<bool> desc;
  for (auto& o : st.order) desc.push_back(o.desc);
  Better better{&desc};
  auto make_entry = [&](const Row& r) {
    Entry e;
    e.pk = r.pk;
    for (auto& o : st.order) e.keys.push_back(value_key(eval_expr(*o.expr, t, &r)));
    e.out = project(r);
    return e;
  };

  if (st.order.empty()) {
    int64_t emitted = 0;
    scan_table(t, [&](const Row& r, RowRef) {
      if (!filter(r)) return true;
      if (st.limit >= 0 && emitted >= st.limit) return false;
      res.rows.push_back(project(r));
      emitted++;
      return true;
    });
  } else if (st.limit >= 0) {
    std::priority_queue<Entry, std::vector<Entry>, Better> heap(better);
    scan_table(t, [&](const Row& r, RowRef) {
      if (!filter(r)) return true;
      Entry e = make_entry(r);
      if ((int64_t)heap.size() < st.limit) heap.push(std::move(e));
      else if (better(e, heap.top())) { heap.pop(); heap.push(std::move(e)); }
      return true;
    });
    std::vector<Entry> all;
    while (!heap.empty()) { all.push_back(heap.top()); heap.pop(); }
    std::reverse(all.begin(), all.end());
    for (auto& e : all) res.rows.push_back(e.out);
  } else {
    std::vector<Entry> mem;
    size_t mem_bytes = 0;
    std::vector<std::string> runs;
    auto flush_run = [&]() {
      if (mem.empty()) return;
      std::sort(mem.begin(), mem.end(), better);
      std::string path = dir_ + "/sort_run_" + std::to_string(runs.size()) + ".tmp";
      std::ofstream os(path, std::ios::binary);
      for (auto& e : mem) write_entry(os, e);
      os.close();
      runs.push_back(path);
      mem.clear();
      mem_bytes = 0;
    };
    scan_table(t, [&](const Row& r, RowRef) {
      if (!filter(r)) return true;
      Entry e = make_entry(r);
      mem_bytes += e.bytes();
      mem.push_back(std::move(e));
      if (mem_bytes > 32 * 1024 * 1024) flush_run();
      return true;
    });
    if (runs.empty()) {
      std::sort(mem.begin(), mem.end(), better);
      for (auto& e : mem) res.rows.push_back(e.out);
    } else {
      flush_run();
      std::vector<std::unique_ptr<std::ifstream>> ins;
      std::vector<Entry> heads;
      for (auto& path : runs) {
        ins.push_back(std::make_unique<std::ifstream>(path, std::ios::binary));
        Entry e;
        if (read_entry(*ins.back(), e)) heads.push_back(std::move(e));
        else heads.push_back(Entry());
      }
      std::vector<bool> valid(heads.size(), false);
      for (size_t i = 0; i < heads.size(); i++) valid[i] = !ins[i]->fail() && (heads[i].keys.size() == st.order.size());
      for (;;) {
        int best = -1;
        for (size_t i = 0; i < heads.size(); i++) {
          if (!valid[i]) continue;
          if (best < 0 || better(heads[i], heads[best])) best = (int)i;
        }
        if (best < 0) break;
        res.rows.push_back(heads[best].out);
        Entry e;
        if (read_entry(*ins[best], e)) heads[best] = std::move(e);
        else valid[best] = false;
      }
      for (auto& path : runs) ::remove(path.c_str());
    }
  }
  for (auto* it : items) if (it->star) delete it;
  return res;
}

Result Engine::execute(const Stmt& st) {
  try {
    bool mutating = st.kind == Stmt::CREATE_TABLE || st.kind == Stmt::INSERT ||
                    st.kind == Stmt::UPDATE || st.kind == Stmt::DELETE || st.kind == Stmt::CREATE_INDEX;
    if (mutating) {
      bool autocommit = !store_.in_txn();
      if (autocommit) store_.begin_txn();
      store_.begin_stmt();
      try {
        Result r;
        switch (st.kind) {
          case Stmt::CREATE_TABLE: r = do_create_table(st); break;
          case Stmt::INSERT: r = do_insert(st); break;
          case Stmt::UPDATE: r = do_update(st); break;
          case Stmt::DELETE: r = do_delete(st); break;
          case Stmt::CREATE_INDEX: r = do_create_index(st); break;
          default: break;
        }
        if (autocommit) store_.commit();
        return r;
      } catch (...) {
        store_.stmt_fail();
        if (autocommit) store_.rollback();
        throw;
      }
    }
    switch (st.kind) {
      case Stmt::SELECT: return do_select(st);
      case Stmt::BEGIN_STMT:
        if (store_.in_txn()) throw MVError(ER_TXN, "transaction already active");
        store_.begin_txn();
        return {true, 0, "", {}, {}};
      case Stmt::COMMIT_STMT:
        if (!store_.in_txn()) throw MVError(ER_TXN, "no active transaction");
        store_.commit();
        return {true, 0, "", {}, {}};
      case Stmt::ROLLBACK_STMT:
        if (!store_.in_txn()) throw MVError(ER_TXN, "no active transaction");
        store_.rollback();
        return {true, 0, "", {}, {}};
      case Stmt::SET_MODE:
        if (st.mode != "ann" && st.mode != "exact") throw MVError(ER_BAD_VALUE, "mode must be ann or exact");
        mode_ = st.mode;
        return {true, 0, "", {}, {}};
      case Stmt::EXIT_STMT:
        return {true, 0, "EXIT", {}, {}};
      default:
        throw MVError(ER_UNSUPPORTED, "statement not supported");
    }
  } catch (const MVError& e) {
    return {false, e.code, e.what(), {}, {}};
  }
}

}  // namespace mvdb