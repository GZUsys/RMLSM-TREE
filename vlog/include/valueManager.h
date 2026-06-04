#ifndef VALUEMANAGER_H
#define VALUEMANAGER_H

#include "location.h"
#include <atomic>

class Group;

class valueManager {
  public:
    valueManager() = delete;
    valueManager(Group &ref, const valueLocation &loc)
        : group_ref(ref), vlog_write_loc(loc) {}
    ~valueManager() {} 

    size_t UsedBytes() const;
    uint64_t VlogWriteBytes() const {
      return pmblock_write_bytes_.load(std::memory_order_relaxed);
    }
    uint64_t GcMigrateBytes() const {
      return gc_migrate_bytes_.load(std::memory_order_relaxed);
    }

  public:
    inline Block *get_current_block() {
        return vlog_write_loc.block;
    }

    inline size_t get_current_offset() {
        return vlog_write_loc.block_off;
    }

    inline valueLocation& get_vlog_write_loc() {
        return vlog_write_loc;
    }

    valueLocation appendWrite(const char *key, size_t keySize,
                  const char *value, size_t valueSize,
                  bool is_gc = false);
    valueLocation appendWrite_fast(const char *key, size_t keySize,
                  const char *value, size_t valueSize,
                  bool is_gc = false);

  private:
    Group &group_ref;

  public:
    valueLocation vlog_write_loc;
    SpinLock lock;

  private:
    std::atomic<uint64_t> pmblock_write_bytes_{0};
    std::atomic<uint64_t> gc_migrate_bytes_{0};
};

#endif