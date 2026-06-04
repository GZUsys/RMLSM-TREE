#ifndef MAPTABMANAGER_H
#define MAPTABMANAGER_H

#include "pmblock.h"
#include "location.h"

#include <map>
#include <unordered_map>
#include <atomic>

class Group;

class MapTableManager {
  public:
    MapTableManager() = delete;
    MapTableManager(Group &ref, const maptableLocation &loc, size_t size);
    ~MapTableManager(); 

    size_t UsedBytes() const;
    uint64_t WriteBytes() const {
        return maptable_write_bytes_.load(std::memory_order_relaxed);
    }

  public:
    inline MapTable *get_table_addr() { 
        return maptable_write_loc.table;
    }

    inline MapEntry *get_cur_write_addr() { 
        return (MapEntry *)maptable_write_loc.get_addr();
    }

    inline maptableLocation& get_maptab_write_loc() {
        return maptable_write_loc;
    }

    maptableLocation appendWrite(MapEntry src_entry);

    void modifyEntryInvalid(PMEMoid LA);
    void tgc_modifyEntryLA(MapEntry *entry, PMEMoid new_oid);
    void fgc_modifyEntryLA(MapEntry *entry, PMEMoid new_oid);

    /* utils */
    size_t index(PMEMoid LA);
    size_t index(MapEntry *entry);
    bool entryValid(PMEMoid LA);
    double invalidEntryRate();

  private:
    Group &group_ref;

  public:
    maptableLocation maptable_write_loc;

    SpinLock *locks;

    size_t all_size;        
    size_t all_count;       
    std::atomic<size_t> current_count;   

    struct ComparePMEMoid {
        bool operator()(const PMEMoid& a, const PMEMoid& b) const {
            return a.off < b.off;  
        }
    };
    SpinLock _slock_empty_entries;
    std::map<PMEMoid, MapEntry *, ComparePMEMoid> empty_entries;
    SpinLock _slock_wait_gc_entries;
    std::map<PMEMoid, MapEntry *, ComparePMEMoid> wait_gc_entries;
    struct PMEMoidHash {
        std::size_t operator()(const PMEMoid& oid) const {
            std::size_t h1 = std::hash<uint64_t>{}(oid.pool_uuid_lo);
            std::size_t h2 = std::hash<uint64_t>{}(oid.off);
            return h1 ^ (h2 << 1);  
        }
    };
    struct PMEMoidEqual {
        bool operator()(const PMEMoid& a, const PMEMoid& b) const {
            return a.pool_uuid_lo == b.pool_uuid_lo && a.off == b.off;
        }
    };
    SpinLock _slock_hash;
    std::unordered_map<PMEMoid, PMEMoid, PMEMoidHash, PMEMoidEqual> valid_kv_map2_entry;

    private:
        std::atomic<uint64_t> maptable_write_bytes_{0};
};

#endif