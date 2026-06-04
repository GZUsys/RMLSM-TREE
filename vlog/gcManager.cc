#include "gcManager.h"
#include <unordered_set>
#include <iostream>
#include <chrono>

gcManager::gcManager(GroupManager &ref):gm_ref(ref) {
    _stop.store(false);
    _gc_fast_group_number.store(-1);

    gc_fast_ptr = std::make_unique<std::thread>([this](){ fast_gc(); });
}

gcManager::~gcManager() {
    stop();
}

void gcManager::stop() {
    {
        std::unique_lock<std::mutex> lock(_mtx_gc_fast);
        while (_gc_state.load() != GCState::IDLE) {
            _cv_gc_fast.notify_one();
            _cv_gc_fast.wait_for(lock, std::chrono::milliseconds(10));
        }
    }
    _stop.store(true);
    {
        std::unique_lock<std::mutex> lock(_mtx_gc_fast);
    }
    _cv_gc_fast.notify_one();
    if (gc_fast_ptr && gc_fast_ptr->joinable()) {
        gc_fast_ptr->join();
    }
}

void gcManager::WaitIdle() {
    std::unique_lock<std::mutex> lock(_mtx_gc_fast);
    while (_gc_state.load() != GCState::IDLE) {
        _cv_gc_fast.wait_for(lock, std::chrono::milliseconds(10));
    }
}

void gcManager::isGC() {
    std::vector<std::pair<int, double>> partition_invalid_rates;
    
    int gc_group = -1;
    double invalidrate = 0.0;
    for (size_t i = 0; i < DEFAULT_PARTITION_NUMS; i++) {
        double partition_invalid_rate = gm_ref[i]._maptable->invalidEntryRate();
        double free_block_rate = 0.0;
        size_t total_blocks = gm_ref[i].total_blocks();
        if (total_blocks > 0) {
            free_block_rate = 1.0 * gm_ref[i].free_blocks() / total_blocks;
        }
        if (partition_invalid_rate >= 0.7 || free_block_rate < 0.2) {
            std::cout << "gcManager::isGC() TrueGC" << i
                      << " invalidrate=" << partition_invalid_rate
                      << " free_block_rate=" << free_block_rate << std::endl;
            double score = partition_invalid_rate;
            if (free_block_rate < 0.2) {
                score = std::max(score, 1.0 - free_block_rate);
            }
            partition_invalid_rates.push_back({static_cast<int>(i), score});
        }
    }
    std::sort(partition_invalid_rates.begin(), partition_invalid_rates.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    if (!partition_invalid_rates.empty()) {
        gc_group = partition_invalid_rates[0].first;
        invalidrate = partition_invalid_rates[0].second;
        {
            std::unique_lock<std::mutex> lock(_mtx_gc_fast);
            if (_gc_state.load() == GCState::IDLE) {
                _gc_state.store(GCState::SCHEDULED);
                _gc_fast_group_number.store(gc_group);
                lock.unlock();
                _cv_gc_fast.notify_one();
            }
        }
    }
}

size_t BLOCK_ALL_KV_NUMS(size_t blocksize, size_t kvsize) {
    return (blocksize - CACHE_LINE_SIZE) / (sizeof(size_t)*2 + kvsize);
}

void gcManager::fast_gc() {
    while (!_stop.load()) {
        {
            std::unique_lock<std::mutex> lock(_mtx_gc_fast);
            _cv_gc_fast.wait(lock, [this] {
                return _stop.load() || (_gc_state.load() == GCState::SCHEDULED);
            });
        }

        int gn = _gc_fast_group_number.load();
        if (gn != -1) {
            std::cout << "fast_gc runing in by gn = " << gn << std::endl;

            _gc_state.store(GCState::RUNNING);
            std::unordered_set<size_t> low_valid_blocks;
            size_t unify_block_all_kv_nums = 0;
            size_t keySize = 0, valueSize = 0, expect_low_kv_nums = 0;

            while (true) {
                std::pair<PMEMoid, MapEntry *> cur;
                bool entry_found = false;
                {
                    std::lock_guard<SpinLock> lock(gm_ref[gn]._maptable->_slock_wait_gc_entries);
                    if (gm_ref[gn]._maptable->wait_gc_entries.empty())
                        break;
                    auto it = gm_ref[gn]._maptable->wait_gc_entries.begin();
                    cur = *it;
                    gm_ref[gn]._maptable->wait_gc_entries.erase(it);

                    entry_found = true;
                    {
                        std::lock_guard<SpinLock> lock_maptable_hash(gm_ref[gn]._maptable->_slock_hash);
                        gm_ref[gn]._maptable->valid_kv_map2_entry.erase(cur.second->pa);
                    }
                }
                if (!entry_found)
                    break;
                {
                    gm_ref[gn]._maptable->locks[gm_ref[gn]._maptable->index(cur.second)].lock();

                    size_t cur_block_id = gm_ref[gn].block_id(cur.second->pa);
                    BlockInfo &cur_block_info = gm_ref[gn].block_id(cur_block_id);
                    Block *block = OID2POINTER(Block *, cur_block_info.block_oid);
                    Block::subValid(gm_ref[gn].block_lock(cur_block_id), gm_ref[gn].get_pop(), block);

                    if (block->valid_kv < expect_low_kv_nums && block->is_allocated == 1 &&
                        low_valid_blocks.find(cur_block_id) == low_valid_blocks.end()) {
                        low_valid_blocks.insert(cur_block_id);
                    }

                    gm_ref[gn]._maptable->fgc_modifyEntryLA(cur.second, OID_NULL);

                    if (keySize == 0 && valueSize == 0) {
                        keySize = *((size_t *)block);
                        valueSize = *((size_t *)((char *)block+sizeof(size_t)+keySize));
                        unify_block_all_kv_nums = BLOCK_ALL_KV_NUMS(sizeof(Block), keySize+valueSize);
                        expect_low_kv_nums = unify_block_all_kv_nums * 0.5;
                    }

                    gm_ref[gn]._maptable->locks[gm_ref[gn]._maptable->index(cur.second)].unlock();
                }
            }

            for (const auto& block_id : low_valid_blocks) {
                BlockInfo &block_info = gm_ref[gn].block_id(block_id);
                Block *block = OID2POINTER(Block *, block_info.block_oid);
                
                // std::cout << "\tentry gn=" << gn << " blockid=" << block->id << " block_status=" << block->is_allocated << " block_valid_rate=" << (block->valid_kv * 100.0 / unify_block_all_kv_nums) << " current_valid_kv=" << block->valid_kv << " expect_low_kv=" << expect_low_kv_nums << " unify_block_all=" << unify_block_all_kv_nums << " keySize=" << keySize << ", valueSize=" << valueSize << std::endl;
                std::lock_guard<SpinLock> lock(gm_ref[gn].block_lock(block_id));

                char *kv_pos = (char *)block;
                size_t count = 0;
                while (kv_pos < (char *)block+sizeof(Block)-CACHE_LINE_SIZE) {
                    size_t cur_keySize, cur_valueSize, allsize;
                    cur_keySize = *(size_t *)kv_pos;
                    cur_valueSize = *(size_t *)(kv_pos + sizeof(size_t) + cur_keySize);
                    allsize = sizeof(size_t)*2 + cur_keySize + cur_valueSize;
                    char *key = kv_pos + sizeof(size_t);
                    char *value = kv_pos + sizeof(size_t)*2 + cur_keySize;
                    if (cur_keySize == 0 || cur_valueSize == 0)
                        break;
                    std::unordered_map<PMEMoid, PMEMoid, MapTableManager::PMEMoidHash, 
                                       MapTableManager::PMEMoidEqual>::iterator hash_valid_find_res;
                    {
                        std::lock_guard<SpinLock> lock_maptable_hash(gm_ref[gn]._maptable->_slock_hash);
                        hash_valid_find_res = gm_ref[gn]._maptable->valid_kv_map2_entry.find(POINTER2OID(kv_pos));
                        if (hash_valid_find_res == gm_ref[gn]._maptable->valid_kv_map2_entry.end()) {
                            kv_pos += allsize;
                            continue;
                        }
                    }

                    count++;

                    valueLocation new_write_place =
                        gm_ref[gn]._vlog->appendWrite_fast(key, cur_keySize, value, cur_valueSize, true);
                    {
                        std::lock_guard<SpinLock> lock_maptable(gm_ref[gn]._maptable->_slock_hash);
                        MapEntry *entry = nullptr;
                        if (hash_valid_find_res != gm_ref[gn]._maptable->valid_kv_map2_entry.end()) {
                            entry = OID2POINTER(MapEntry *, hash_valid_find_res->second);
                            gm_ref[gn]._maptable->fgc_modifyEntryLA(entry, POINTER2OID(new_write_place.get_addr()));
                        }
                        if (entry != nullptr && hash_valid_find_res != gm_ref[gn]._maptable->valid_kv_map2_entry.end()) {
                            gm_ref[gn]._maptable->valid_kv_map2_entry.erase(hash_valid_find_res);
                            gm_ref[gn]._maptable->valid_kv_map2_entry.insert({POINTER2OID(new_write_place.get_addr()), POINTER2OID(entry)});
                        }
                    } 

                    kv_pos += allsize;
                }

                if (count == block->valid_kv) {
                    // std::cout << "\tblock->valid_kv==0, need free. block_allocator.free_blocks= " << gm_ref[gn].free_blocks() << std::endl;

                    block->valid_kv = 0;
                    pmemobj_persist(gm_ref[gn].get_pop(), block->padding, CACHE_LINE_SIZE);

                    BlockInfo tmpinfo(block->id, POINTER2OID(block));
                    gm_ref[gn].dealloc_block(tmpinfo);

                } else {
                    // std::cout << "3 block->valid_kv != 0, error for free" << std::endl;
                }
            }
            _gc_fast_group_number.store(-1);
            _gc_state.store(GCState::IDLE);
            {
                std::unique_lock<std::mutex> lock(_mtx_gc_fast);
            }
            _cv_gc_fast.notify_all();
        }
    }
}