#ifndef PMBLOCK_H
#define PMBLOCK_H

#include <libpmemobj.h>

#include "define.h"

struct metaroot {
    /* 0 - 64 */
    PMEMoid write_block;            
    uint64_t block_off;             

    uint64_t maptab_off;            
    PMEMoid write_maptab;           

    uint64_t magicNumber;           
    uint64_t all_block_count;       

    /* 64 - 8192 */
    PMEMoid all_block_oid[(DEFAULT_PMOBJ_ROOT_SIZE - 64)/16];
};

struct Block {
    char data[DEFAULT_BLOCK_SIZE - CACHE_LINE_SIZE];
    char padding[CACHE_LINE_SIZE - 16];  //
    uint64_t id;
    uint32_t is_allocated;              /* 0:未分配 1: 已分配 2:正在写 */
    uint32_t valid_kv;

    static inline int block_construct(PMEMobjpool *pop, void *ptr, void *arg) {
        Block *block = (Block *)ptr;
        block->id = *(uint64_t *)arg;
        block->is_allocated = 0;
        block->valid_kv = 0;
        pmemobj_persist(pop, block->padding, CACHE_LINE_SIZE);
        return 0;
    }

    static inline void change_allocated(PMEMobjpool *pop, PMEMoid oid,
                                        int new_status) {
        Block *block = OID2POINTER(Block *, oid);
        block->is_allocated = new_status;
        pmemobj_persist(pop, block->padding, CACHE_LINE_SIZE);
        return ;
    }

    static inline bool canFit(Block *a, size_t off, size_t size) {
        return (off + size) <= (DEFAULT_BLOCK_SIZE - CACHE_LINE_SIZE);
    }

    template <class T>
    static inline bool appendWrite(PMEMobjpool *pop, struct Block *a,
                                   size_t &off, const T *buf, size_t size) {
        if (!Block::canFit(a, off, size))
            return false;
        if (sizeof(T) > 1) {  //
            *(T *)((char *)(a->data) + off) = *buf;
            pmemobj_persist(pop, (char *)(a->data) + off, size);
        } else { // char
            pmemobj_memcpy(pop, ((char *)(a->data) + off), buf, size, PMEMOBJ_F_MEM_NONTEMPORAL);
        }
        off += size;
        return true;
    }

    static inline void addValid(SpinLock &lock, PMEMobjpool *pop, struct Block *a) {
        lock.lock();
        a->valid_kv++;
        pmemobj_persist(pop, a->padding, CACHE_LINE_SIZE);
        lock.unlock();
    }
    static inline void subValid(SpinLock &lock, PMEMobjpool *pop, struct Block *a) {
        lock.lock();
        a->valid_kv--;
        pmemobj_persist(pop, a->padding, CACHE_LINE_SIZE);
        lock.unlock();
    }

    template<class T>
    static inline bool readData(Block *block, size_t off, T *buf, size_t size) {
        if (sizeof(T) > 1) {
            assert(sizeof(T) == size);
            *buf = *(T *)((char *)(block->data) + off);
        } else {
            memcpy(buf, (char *)(block->data) + off, size);
        }
        return true;
    }
    template<class T>
    static inline bool readData(void *addr, T *buf, size_t size) {
        if (sizeof(T) > 1) {
            *buf = *(T *)((char *)addr);
        } else {
            memcpy(buf, (char *)addr, size);
        }
        return true;
    }
};

struct BlockInfo {
  public:
    BlockInfo() : block_oid(OID_NULL) {}
    BlockInfo(uint64_t id, PMEMoid oid, int al = 0, uint32_t validkv = 0)
        : id(id), block_oid(oid), is_allocated(al), valid_kv(validkv) {}
    ~BlockInfo() {}
    BlockInfo(const BlockInfo &other):_slock() { // 拷贝构造函数
        id = other.id;
        block_oid = other.block_oid;
        is_allocated = other.is_allocated;
        valid_kv = other.valid_kv;
    }
    BlockInfo &operator=(const BlockInfo &other) {
        if (this != &other) {
            id = other.id;
            block_oid = other.block_oid;
            is_allocated = other.is_allocated;
            valid_kv = other.valid_kv;
        }
        return *this;
    }
    BlockInfo(BlockInfo &&other):_slock() {
        id = other.id;
        block_oid = other.block_oid;
        is_allocated = other.is_allocated;
        valid_kv = other.valid_kv;
    }

    uint64_t id;
    PMEMoid block_oid;
    int is_allocated;
    uint32_t valid_kv;

    SpinLock _slock;
};

struct MapEntry {
    MapEntry(bool v = false, PMEMoid oid = OID_NULL): valid(v), pa(oid) {}

    uint64_t valid;
    PMEMoid pa;
};

struct MapTable {
    union {
        char data[DEFAULT_MAPTAB_SIZE];
        MapEntry entries[DEFAULT_MAPTAB_SIZE / sizeof(MapEntry)];
    };

    static inline bool CanFit(size_t off) {
        return (off + sizeof(MapEntry)) <= DEFAULT_MAPTAB_SIZE;
    }

    static inline void appendWrite(PMEMobjpool *pop, MapTable *table,
                                   size_t &off, const MapEntry &src_entry) {
        uint64_t index = (off / sizeof(MapEntry));
        table->entries[index].valid = src_entry.valid;
        table->entries[index].pa = src_entry.pa;
        pmemobj_persist(pop, &(table->entries[index]), sizeof(MapEntry));
        off += sizeof(MapEntry);
        return;
    }

    static inline void overWrite(PMEMobjpool *pop, MapEntry *dst_entry,
                                 MapEntry &src_entry) {

        dst_entry->valid = src_entry.valid;
        dst_entry->pa = src_entry.pa;
        pmemobj_persist(pop, (void *)dst_entry, CACHE_LINE_SIZE);
        return ;
    }
};

#endif