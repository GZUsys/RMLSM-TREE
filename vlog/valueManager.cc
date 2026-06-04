#include "valueManager.h"
#include "groupManager.h"
#include "define.h"
#include <cstddef>
#include <cstdio>

size_t valueManager::UsedBytes() const {
    size_t used = 0;
    size_t cnt0 = 0, cnt1 = 0, cnt2 = 0;
    metaroot* meta_ptr = group_ref.get_pm_root();
    if (!meta_ptr) return 0;
    for (size_t i = 0; i < meta_ptr->all_block_count; ++i) {
        Block* pmblock = OID2POINTER(Block*, meta_ptr->all_block_oid[i]);
        if (!pmblock) continue;
        if (pmblock->is_allocated == 2) {
            cnt2++;
            if (vlog_write_loc.block && pmblock->id == vlog_write_loc.block->id) {
                used += vlog_write_loc.block_off;
            }
        } else if (pmblock->is_allocated == 1) {
            cnt1++;
            used += DEFAULT_BLOCK_SIZE;
        } else {
            cnt0++;
        }
    }
    // printf("[LCKV DEBUG] block count: %zu, is_allocated=2: %zu, is_allocated=1: %zu, is_allocated=0: %zu\n",
    //        meta_ptr->all_block_count, cnt2, cnt1, cnt0);
    return used;
}

valueLocation valueManager::appendWrite(const char *key, size_t keySize,
                                        const char *value, size_t valueSize,
                                        bool is_gc) {
    size_t all_write_size = sizeof(size_t) + keySize + sizeof(size_t) + valueSize;

    lock.lock();

    valueLocation cur = vlog_write_loc;
    if (!Block::canFit(cur.block, cur.block_off, all_write_size)) {
        Block::change_allocated(group_ref.get_pop(), POINTER2OID(cur.block), 1);
        BlockInfo newblock = group_ref.alloc_block();
        Block::change_allocated(group_ref.get_pop(), newblock.block_oid, 2);
        cur.block = OID2POINTER(Block *, newblock.block_oid);
        cur.block_off = 0;
    }
    valueLocation ret = cur;
    Block::appendWrite(group_ref.get_pop(), cur.block, cur.block_off, &keySize,
                       sizeof(size_t));
    Block::appendWrite(group_ref.get_pop(), cur.block, cur.block_off, key,
                       keySize);
    Block::appendWrite(group_ref.get_pop(), cur.block, cur.block_off,
                       &valueSize, sizeof(size_t));
    if (valueSize > 0)
        Block::appendWrite(group_ref.get_pop(), cur.block, cur.block_off, value,
                           valueSize);
    Block::addValid(group_ref.block_lock(cur.block->id), group_ref.get_pop(), cur.block); 
    vlog_write_loc = cur;

    if (is_gc) {
        gc_migrate_bytes_.fetch_add(all_write_size, std::memory_order_relaxed);
    } else {
        pmblock_write_bytes_.fetch_add(all_write_size, std::memory_order_relaxed);
    }

    lock.unlock();

    return ret;
}

valueLocation valueManager::appendWrite_fast(const char *key, size_t keySize,
                                        const char *value, size_t valueSize,
                                        bool is_gc) {
    size_t all_write_size = sizeof(size_t) + keySize + sizeof(size_t) + valueSize;
    
    char *write_buffer = static_cast<char*>(alloca(all_write_size));
    char *cur_pos = write_buffer;
    
    memcpy(cur_pos, &keySize, sizeof(size_t));
    cur_pos += sizeof(size_t);
    memcpy(cur_pos, key, keySize);
    cur_pos += keySize;
    memcpy(cur_pos, &valueSize, sizeof(size_t));
    cur_pos += sizeof(size_t);
    memcpy(cur_pos, value, valueSize);

    lock.lock();
    valueLocation cur = vlog_write_loc;
    
    if (!Block::canFit(cur.block, cur.block_off, all_write_size)) {
        Block::change_allocated(group_ref.get_pop(), POINTER2OID(cur.block), 1);
        BlockInfo newblock = group_ref.alloc_block();
        Block::change_allocated(group_ref.get_pop(), newblock.block_oid, 2);
        cur.block = OID2POINTER(Block *, newblock.block_oid);
        cur.block_off = 0;
    }
    
    valueLocation ret = cur;
    
    pmemobj_memcpy(group_ref.get_pop(), 
                   (char*)(cur.block->data) + cur.block_off,
                   write_buffer,
                   all_write_size,
                   PMEMOBJ_F_MEM_NONTEMPORAL);
    
    cur.block_off += all_write_size;
    vlog_write_loc = cur;

    if (is_gc) {
        gc_migrate_bytes_.fetch_add(all_write_size, std::memory_order_relaxed);
    } else {
        pmblock_write_bytes_.fetch_add(all_write_size, std::memory_order_relaxed);
    }
    
    Block::addValid(group_ref.block_lock(cur.block->id), group_ref.get_pop(), cur.block);
    
    lock.unlock();
    return ret;
}