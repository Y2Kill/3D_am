#pragma once
#include <sqlite3.h>
#include <atomic>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fault_test {
// Test-only VFS: all real reads/locks/WAL operations are delegated. Only selected
// database writes return SQLITE_FULL. No production injection or schema changes.
class FullVfs {
 public:
  explicit FullVfs(std::string database) : database_(std::move(database)), wal_(database_ + "-wal") {
    if (active_) throw std::runtime_error("Nested fault VFS is not supported");
    original_ = sqlite3_vfs_find(nullptr);
    if (!original_) throw std::runtime_error("SQLite VFS unavailable");
    vfs_ = *original_;
    vfs_.zName = "am-test-full";
    vfs_.pNext = nullptr;
    // Keep the original pAppData: unchanged VFS methods still receive its data.
    vfs_.szOsFile += static_cast<int>(sizeof(File));
    vfs_.xOpen = open;
    active_ = this;
    if (sqlite3_vfs_register(&vfs_, 1) != SQLITE_OK) {
      active_ = nullptr;
      throw std::runtime_error("Cannot register fault VFS");
    }
  }
  ~FullVfs() {
    sqlite3_vfs_register(original_, 1);
    sqlite3_vfs_unregister(&vfs_);
    active_ = nullptr;
  }
  FullVfs(const FullVfs&) = delete;
  FullVfs& operator=(const FullVfs&) = delete;
  void arm(int successful_writes = 0) { single_=false; remaining_ = successful_writes; armed_ = true; }
  void arm_once() { single_=true; remaining_=0; armed_=true; }
  void disarm() { armed_ = false; }
  int failures() const { return failures_.load(); }
 private:
  struct File {
    sqlite3_file base{};
    sqlite3_file* real = nullptr;
    FullVfs* owner = nullptr;
    bool selected = false;
  };
  inline static FullVfs* active_ = nullptr;
  sqlite3_vfs vfs_{};
  sqlite3_vfs* original_ = nullptr;
  std::string database_, wal_;
  std::atomic<bool> armed_ = false;
  std::atomic<bool> single_ = false;
  std::atomic<int> remaining_ = 0, failures_ = 0;
  static File* f(sqlite3_file* p) { return reinterpret_cast<File*>(p); }
  static int open(sqlite3_vfs*, const char* name, sqlite3_file* out, int flags, int* actual) {
    auto* owner = active_;
    auto* file = new (out) File{};
    file->owner = owner;
    file->real = reinterpret_cast<sqlite3_file*>(reinterpret_cast<char*>(out) + sizeof(File));
    if (name) {
      const std::string_view path(name);
      file->selected = path.ends_with(owner->database_) || path.ends_with(owner->wal_);
    }
    const auto rc = owner->original_->xOpen(owner->original_, name, file->real, flags, actual);
    if (file->real->pMethods) file->base.pMethods = &methods_;
    return rc;
  }
  static int close(sqlite3_file* p) { auto* v=f(p); const auto rc=v->real->pMethods->xClose(v->real); p->pMethods=nullptr; return rc; }
  static int read(sqlite3_file* p, void* b, int n, sqlite3_int64 o) { auto* v=f(p); return v->real->pMethods->xRead(v->real,b,n,o); }
  static int write(sqlite3_file* p, const void* b, int n, sqlite3_int64 o) {
    auto* v=f(p);
    if (v->selected && v->owner->armed_ && v->owner->remaining_.fetch_sub(1) <= 0) {
      ++v->owner->failures_;
      if (v->owner->single_) v->owner->armed_=false;
      return SQLITE_FULL;
    }
    return v->real->pMethods->xWrite(v->real,b,n,o);
  }
  static int truncate(sqlite3_file* p, sqlite3_int64 n) { auto* v=f(p); return v->real->pMethods->xTruncate(v->real,n); }
  static int sync(sqlite3_file* p, int n) { auto* v=f(p); return v->real->pMethods->xSync(v->real,n); }
  static int size(sqlite3_file* p, sqlite3_int64* n) { auto* v=f(p); return v->real->pMethods->xFileSize(v->real,n); }
  static int lock(sqlite3_file* p, int n) { auto* v=f(p); return v->real->pMethods->xLock(v->real,n); }
  static int unlock(sqlite3_file* p, int n) { auto* v=f(p); return v->real->pMethods->xUnlock(v->real,n); }
  static int reserved(sqlite3_file* p, int* n) { auto* v=f(p); return v->real->pMethods->xCheckReservedLock(v->real,n); }
  static int control(sqlite3_file* p, int op, void* n) { auto* v=f(p); return v->real->pMethods->xFileControl(v->real,op,n); }
  static int sector(sqlite3_file* p) { auto* v=f(p); return v->real->pMethods->xSectorSize(v->real); }
  static int characteristics(sqlite3_file* p) { auto* v=f(p); return v->real->pMethods->xDeviceCharacteristics(v->real); }
  static int shm_map(sqlite3_file* p, int a, int b, int c, void volatile** d) { auto* v=f(p); return v->real->pMethods->xShmMap(v->real,a,b,c,d); }
  static int shm_lock(sqlite3_file* p, int a, int b, int c) { auto* v=f(p); return v->real->pMethods->xShmLock(v->real,a,b,c); }
  static void shm_barrier(sqlite3_file* p) { auto* v=f(p); v->real->pMethods->xShmBarrier(v->real); }
  static int shm_unmap(sqlite3_file* p, int a) { auto* v=f(p); return v->real->pMethods->xShmUnmap(v->real,a); }
  static int fetch(sqlite3_file* p, sqlite3_int64 a, int b, void** c) { auto* v=f(p); if (!v->real->pMethods->xFetch) { *c=nullptr; return SQLITE_OK; } return v->real->pMethods->xFetch(v->real,a,b,c); }
  static int unfetch(sqlite3_file* p, sqlite3_int64 a, void* b) { auto* v=f(p); return v->real->pMethods->xUnfetch ? v->real->pMethods->xUnfetch(v->real,a,b) : SQLITE_OK; }
  inline static const sqlite3_io_methods methods_ = {3,close,read,write,truncate,sync,size,lock,unlock,reserved,control,sector,characteristics,
                                                   shm_map,shm_lock,shm_barrier,shm_unmap,fetch,unfetch};
};
}  // namespace fault_test
