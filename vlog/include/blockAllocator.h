#ifndef BLOCKALLOCATOR_H
#define BLOCKALLOCATOR_H


#include <mutex>
#include <vector>
#include <map>
#include <unordered_map>

#include "pmblock.h"

class BlockAllocator {
  private:
    std::vector<BlockInfo> managed_oids;
    std::vector<bool> allocated_status;
    std::vector<size_t> free_indices_stack;
    std::unordered_map<size_t, size_t> id_to_index_map; /* 块id -> 数组下标 */
    mutable std::mutex mtx;

  public:
    BlockAllocator() {}
    BlockAllocator(const std::vector<BlockInfo> &init) {
        std::lock_guard<std::mutex> lock(mtx);

        managed_oids = init;
        allocated_status.resize(init.size(), false);

        for (size_t i = 0; i < managed_oids.size(); i++) {
            free_indices_stack.push_back(i);
            id_to_index_map[managed_oids[i].id] = i;
        }
    }
    void operator=(const std::vector<BlockInfo> &init) {
        std::lock_guard<std::mutex> lock(mtx);

        managed_oids = init;
        allocated_status.resize(init.size(), false);

        for (size_t i = 0; i < managed_oids.size(); i++) {
            free_indices_stack.push_back(i);
            id_to_index_map[managed_oids[i].id] = i;
        }
    }
    void recovery(const std::vector<BlockInfo> &rec) {
        std::lock_guard<std::mutex> lock(mtx);

        managed_oids = rec;
        allocated_status.resize(rec.size(), false);
        for (size_t i = 0; i < rec.size(); i++) {
            if (rec[i].is_allocated)
                allocated_status[i] = true;
            else {
                free_indices_stack.push_back(i);
            }
            id_to_index_map[managed_oids[i].id] = i;
        }
    }
    BlockInfo allocate() {
        std::lock_guard<std::mutex> lcok(mtx);

        if (free_indices_stack.empty()) {
            perror("free_indices_stack.empty() error");
        }

        size_t index = free_indices_stack.back();
        free_indices_stack.pop_back();

        allocated_status[index] = true;

        managed_oids[index].is_allocated = true;
        return managed_oids[index];
    }
    void free(size_t id) {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = id_to_index_map.find(id);
        if (it == id_to_index_map.end())
            perror("未找到映射");
        
        size_t index = it->second;

        if (!allocated_status[index])
            perror("已分配");
        
        allocated_status[index] = false; // 修改分配状态为未分配
        managed_oids[index].is_allocated = false;
        free_indices_stack.push_back(index);
    }
    SpinLock &block_lock(size_t id) { // 得到DRAM上的block自旋锁
        return managed_oids[id_to_index_map[id]]._slock;
    }
    size_t id(PMEMoid LA) {
        for (size_t i = 0; i < managed_oids.size(); i++) {
            assert(LA.pool_uuid_lo == managed_oids[i].block_oid.pool_uuid_lo);
            if (managed_oids[i].block_oid.off <= LA.off &&
                LA.off < (managed_oids[i].block_oid.off + DEFAULT_BLOCK_SIZE)) {
                return managed_oids[i].id;
            }
        }
        return -1;
    }
    BlockInfo &id(size_t id) {
        return managed_oids[id_to_index_map[id]];
    }
    bool is_allocated(size_t id) {
        std::lock_guard<std::mutex> lock(mtx);

        auto it = id_to_index_map.find(id);
        if (it == id_to_index_map.end()) {
            return false;
        }
        return allocated_status[it->second];
    }
    inline size_t total_blocks() {
        return managed_oids.size();
    }
    inline size_t free_blocks() {
        std::lock_guard<std::mutex> lock(mtx);
        return free_indices_stack.size();
    }
};

#endif