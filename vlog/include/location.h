#ifndef LOCATION_H
#define LOCATION_H

#include "pmblock.h"

class Location {
  public:
    Location() {}
    Location(const Location &loc) // 拷贝构造函数
        : in_lsm(loc.in_lsm), LA(loc.LA), value(loc.value) {}
    Location & operator=(const Location &loc) { // 拷贝赋值运算
        if (this == &loc) {
            return *this;
        }
        in_lsm = loc.in_lsm;
        LA = loc.LA;
        value = loc.value;
        return *this;
    }

  public:
    Location(PMEMoid la, bool inlsm = false) : in_lsm(inlsm), LA(la) {}
    Location(const std::string &v, bool inlsm = true) : in_lsm(inlsm), value(v) {}
    Location(std::string &&v, bool inlsm = true) : in_lsm(inlsm), value(v) {}
    virtual ~Location() {}

    inline virtual void *get_addr() {
        return OID2POINTER(void *, LA);
    }
  
  public:
    std::string serialize() {
        std::string ans;
        if (in_lsm) {
            ans.append("1");
            ans.append(value);
        } else {
            ans.append("0");
            char buf[16];
            memcpy(buf, &LA.pool_uuid_lo, 8);
            memcpy(buf + 8, &LA.off, 8);
            ans.append(buf, 16);
        }
        return ans;
    }

    void deserialize(const std::string &str) {
        if (str[0] == '1') {
            in_lsm = true;
            value = std::move(std::string(str, 1, str.size() - 1));
        } else {
            in_lsm = false;
            memcpy(&LA.pool_uuid_lo, str.c_str() + 1, 8);
            memcpy(&LA.off, str.c_str() + 1 + 8, 8);
        }
    }

  public:
    bool in_lsm;
    /* */
    PMEMoid LA;
    std::string value;
};

class valueLocation : public Location {
  public:
    valueLocation() {}
    valueLocation(Block *b, size_t off) : block(b), block_off(off) {}
    valueLocation(const valueLocation &other)
        : block(other.block), block_off(other.block_off) {}

    void *get_addr() {
        return (void *)((size_t)block + block_off);
    }

    Block * block;
    size_t block_off;
};

class maptableLocation : public Location {
  public:
    maptableLocation() {}
    maptableLocation(MapTable *m, size_t off) : table(m), table_off(off) {}
    maptableLocation(const maptableLocation &other)
        : table(other.table), table_off(other.table_off) {}

    void *get_addr() const {
        return (void *)((size_t)table + table_off);
    }

    MapTable *table;
    size_t table_off;
};

#endif