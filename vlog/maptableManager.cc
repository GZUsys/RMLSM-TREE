#include "maptableManager.h"
#include "groupManager.h"
#include "define.h"
#include <cstddef>

size_t MapTableManager::UsedBytes() const {
    size_t written = ((size_t)maptable_write_loc.get_addr() - (size_t)maptable_write_loc.table);
    size_t empty = empty_entries.size();
    size_t used = written - empty * sizeof(MapEntry);
    return used;
}

maptableLocation MapTableManager::appendWrite(MapEntry src_entry) {
    {
        bool is_empty = false;
        _slock_empty_entries.lock();
        is_empty = empty_entries.empty();
        if (!is_empty) {
            auto it = empty_entries.begin();
            MapEntry *dst_entry = it->second;
            empty_entries.erase(it);
            _slock_empty_entries.unlock();

            locks[index(dst_entry)].lock();
            _slock_hash.lock();
            valid_kv_map2_entry.insert({src_entry.pa, it->first});
            _slock_hash.unlock();
            MapTable::overWrite(group_ref.get_pop(), dst_entry, src_entry);
            maptable_write_bytes_.fetch_add(sizeof(MapEntry), std::memory_order_relaxed);
            maptableLocation ret(maptable_write_loc.table, (size_t)dst_entry -
                                    (size_t)maptable_write_loc.table);
            locks[index(dst_entry)].unlock();

            return ret;
        }
        _slock_empty_entries.unlock();
    }
    if (!MapTable::CanFit(maptable_write_loc.table_off)) {
        throw std::runtime_error("MapTable full: no space for new MapEntry");
    }
    maptableLocation ret = maptable_write_loc;

    maptableLocation cur = maptable_write_loc;
    _slock_hash.lock();
    valid_kv_map2_entry.insert({src_entry.pa, POINTER2OID(cur.get_addr())});
    _slock_hash.unlock();
    MapTable::appendWrite(group_ref.get_pop(), cur.table, cur.table_off,
                          src_entry);
    maptable_write_bytes_.fetch_add(sizeof(MapEntry), std::memory_order_relaxed);

    maptable_write_loc = cur;
    current_count.fetch_add(1);
    return ret;
}

MapTableManager::MapTableManager(Group &ref, const maptableLocation &loc,
                                 size_t size)
    : group_ref(ref), maptable_write_loc(loc) {
    all_size = size;
    all_count = all_size / sizeof(MapEntry);
    current_count.store(index((MapEntry *)(maptable_write_loc.get_addr())));

    MapEntry *start = (MapEntry *)maptable_write_loc.table;
    MapEntry *end = (MapEntry *)maptable_write_loc.get_addr();
    for (MapEntry *entry = start; entry != end; entry++) {
        if (entry->valid == false && OID_IS_NULL(entry->pa)) {
            empty_entries.insert({POINTER2OID(entry), entry});
        } else if (entry->valid == false && !OID_IS_NULL(entry->pa)) {
            wait_gc_entries.insert({POINTER2OID(entry), entry});
        } else {
            valid_kv_map2_entry.insert({entry->pa, POINTER2OID(entry)});
        }
    }
    locks = new SpinLock[all_count];

    return;
}

MapTableManager::~MapTableManager() {
    delete[] locks;
}

size_t MapTableManager::index(PMEMoid LA) {
    MapTable *table = maptable_write_loc.table;
    MapEntry *entry = OID2POINTER(MapEntry *, LA);
    return ((size_t)entry - (size_t)table) / sizeof(MapEntry);
}

size_t MapTableManager::index(MapEntry *entry) {
    MapTable *table = maptable_write_loc.table;
    return ((size_t)entry - (size_t)table) / sizeof(MapEntry);
}

bool MapTableManager::entryValid(PMEMoid LA) {
    MapEntry *entry = OID2POINTER(MapEntry *, LA);
    return entry->valid;
}

double MapTableManager::invalidEntryRate() {
    return (1.0 * wait_gc_entries.size()) / (current_count.load() - empty_entries.size());
}

void MapTableManager::modifyEntryInvalid(PMEMoid LA) {
    size_t i = index(LA);
    MapEntry *entry = OID2POINTER(MapEntry *, LA);
    locks[i].lock();
    
    entry->valid = false;
    pmemobj_persist(group_ref.get_pop(), entry, sizeof(MapEntry));
    maptable_write_bytes_.fetch_add(1, std::memory_order_relaxed);
    { 
        _slock_wait_gc_entries.lock();
        wait_gc_entries.insert({LA, entry});
        _slock_wait_gc_entries.unlock();
    }

    locks[i].unlock();
    return ;
}

void MapTableManager::tgc_modifyEntryLA(MapEntry *entry, PMEMoid newoid) {
    entry->pa = newoid;
    pmemobj_persist(group_ref.get_pop(), entry, sizeof(MapEntry));
    maptable_write_bytes_.fetch_add(sizeof(MapEntry), std::memory_order_relaxed);
    if (OID_IS_NULL(newoid)) {
        {
            _slock_wait_gc_entries.lock();
            auto it = wait_gc_entries.find(POINTER2OID(entry));
            if (it == wait_gc_entries.end()) {
                throw std::runtime_error("NORMAL GC: entry not exist");
            }
            wait_gc_entries.erase(it);
            _slock_wait_gc_entries.unlock();

            _slock_empty_entries.lock();
            empty_entries.insert({POINTER2OID(entry), entry});
            _slock_empty_entries.unlock();
        }
    }
    return ;
}

void MapTableManager::fgc_modifyEntryLA(MapEntry *entry, PMEMoid newoid) {
    entry->pa = newoid;
    pmemobj_persist(group_ref.get_pop(), entry, sizeof(MapEntry));
    maptable_write_bytes_.fetch_add(sizeof(MapEntry), std::memory_order_relaxed);
    if (OID_IS_NULL(newoid)) {
        _slock_empty_entries.lock();

        empty_entries.insert({POINTER2OID(entry), entry});

        _slock_empty_entries.unlock();
    }
}