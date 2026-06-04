#ifndef GROUPMANAGER_H
#define GROUPMANAGER_H

#include <string>
#include <vector>
#include <queue>
#include <unordered_map>
#include <list>
#include <mutex>
#include <exception>
#include <tuple>

#include "define.h"
#include "deviceManager.h"
#include "location.h"
#include "blockAllocator.h"
#include "maptableManager.h"
#include "valueManager.h"

class Group {
  public:
    Group(FsDaxDevice *fsdev, int gid);
    ~Group();
    void test();

    // space
    std::pair<size_t, size_t> UsedBytes() const;
    std::tuple<uint64_t, uint64_t, uint64_t> WriteBytesDetail() const {
      uint64_t maptable = _maptable ? _maptable->WriteBytes() : 0;
      uint64_t pmblock = _vlog ? _vlog->VlogWriteBytes() : 0;
      uint64_t gc = _vlog ? _vlog->GcMigrateBytes() : 0;
      return {maptable, pmblock, gc};
    }

  public:
    inline PMEMobjpool *get_pop() {
        return pool->pop;
    }
    metaroot *get_pm_root();                                    
    bool check_list_exist();                                    

  private: 
    BlockInfo obj_alloc_block();                                
    void obj_free_block(PMEMoid node);                          

  public:
    BlockInfo alloc_block();                                    // 从free_blocks池中分配
    void dealloc_block(BlockInfo &block);                       // 归还给free_blocks池
    inline bool is_allocated(BlockInfo &block) {
        assert(block.is_allocated == _block_allocator.is_allocated(block.id));
        return block.is_allocated;
    }
    inline size_t total_blocks() {
        return _block_allocator.total_blocks();
    }
    inline size_t free_blocks() {
        return _block_allocator.free_blocks();
    }

    SpinLock& block_lock(size_t id) { return _block_allocator.block_lock(id); }
    size_t block_id(PMEMoid LA) { return _block_allocator.id(LA); }
    BlockInfo& block_id(size_t id) { return _block_allocator.id(id); }

  public:
    inline valueLocation& get_vlog_write_loc() {
        return _vlog->get_vlog_write_loc();
    }
    inline maptableLocation& get_maptab_write_loc() {
        return _maptable->get_maptab_write_loc();
    }
    valueLocation appendWrite(const char *key, size_t keySize, const char *value, size_t valueSize);

    std::pair<std::string, std::string> appendRead(valueLocation loc, int index);
    std::pair<void *, void *> appendRead_(valueLocation loc, int index);

  private:
    FsDaxDevice *pool;

  public:
    MapTableManager *_maptable; 
    valueManager *_vlog;        

  private:
    uint64_t _magic_number;
    int _gid_dram;              

    BlockAllocator _block_allocator;

};

#include <unistd.h>

class GroupManager {
  public:
    GroupManager(const std::string &db_name = "default_", DevDaxDevice *devptr = nullptr);
    virtual ~GroupManager();

    std::pair<size_t, size_t> UsedBytes() const;
    std::tuple<uint64_t, uint64_t, uint64_t> WriteBytesDetail() const;

    Group &operator[](size_t i) {
        return *(partitions[i]);
    }

    size_t group_number(PMEMoid oid) {
        for (size_t i = 0; i < partitions.size(); i++) {
            PMEMoid table_start = POINTER2OID(partitions[i]->_maptable->get_table_addr());
            if (oid.pool_uuid_lo == table_start.pool_uuid_lo)
                return i;
        }
        // error 
        return -1;
    }

    void init();
    void destory();

  private:
    DevDaxDevice *_device_ptr;  //
    void *all_base_addr;

    bool is_open = false;
    std::string db_name;
    std::vector<std::string> partitions_path;
    std::vector<Group *> partitions;
};

#endif