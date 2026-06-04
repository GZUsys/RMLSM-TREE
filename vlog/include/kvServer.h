#ifndef KVSERVER_H
#define KVSERVER_H

#include "groupManager.h"
#include "gcManager.h"

#include "leveldb/write_batch.h"

#include <memory>
#include <unordered_map>
#include <shared_mutex>
#include <tuple>

class vlogManager {
  public:
    vlogManager(const std::string &db_name);
    ~vlogManager(); // 不需要继承

    std::pair<size_t, size_t> UsedBytes() const {
      if (!_groupManager) return {0, 0};
      return _groupManager->UsedBytes();
    }

    std::tuple<uint64_t, uint64_t, uint64_t> WriteBytesDetail() const {
      if (!_groupManager) return {0, 0, 0};
      return _groupManager->WriteBytesDetail();
    }

    void WaitGcIdle() const {
      if (_gcManager) {
        _gcManager->WaitIdle();
      }
    }

    Location putValue_separation(const char *key, uint64_t keySize, const char *value,
                  uint64_t valueSize);
    bool getValue(size_t keySize, std::string *&value);
    leveldb::Slice iterValue(const leveldb::Slice &value);

    // bool delValue(const char *key, size_t keySize);
    // void manual_compaction(std::string &begin, std::string &end);

  public:
    std::unordered_map<std::string, std::string> _memory_hash;
    mutable std::shared_mutex _memory_hash_mutex;
    std::shared_ptr<GroupManager> _groupManager;
    std::shared_ptr<gcManager> _gcManager;
};

#endif