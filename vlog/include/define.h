#ifndef DEFINE_H
#define DEFINE_H

/* Base Configuration */
#define CACHE_LINE_SIZE (64)

/* KV Configuration */
#define KEY_SIZE        (16)
/* Node Configuration */
#define LIST_NODE_SIZE_64M (64 * (1UL << 20))
#define LIST_NODE_SIZE_256M (64 * (1UL << 20))
#define LIST_NODE_SIZE_1GB (1UL << 30)

#define DEFAULT_BLOCK_SIZE LIST_NODE_SIZE_256M
#define DEFAULT_MAPTAB_SIZE (DEFAULT_POOL_SIZE / 40) 

#define DEFAULT_PMOBJ_ROOT_SIZE (8 * (1UL << 10))

/* 键值分离 */
#define DEFAULT_MIN_VALUE_SIZE  512 

/* PM Configuration */
#define DEFAULT_POOL_LAYOUT_NAME "emerald"
#define DEFAULT_POOL_MODE 0666
#define DEFAULT_FSDAX_DIR "/pmem0/emerald/"
#define DEFAULT_PARTITION_NAME "group_"
#define DEFAULT_POOL_SIZE   (45 * (1UL << 30))

#define DEFAULT_DAXDEV_PATH "/dev/dax1.0"
#define DEFAULT_DAXDEV_SIZE (400 * (1UL << 30))

/* misc */
#define DEFAULT_PARTITION_NUMS 1
#define DEFAULT_GROUP_BLOCK_NUMS ((DEFAULT_POOL_SIZE / DEFAULT_BLOCK_SIZE) * 0.9)

/* LevelDB */
#define DEFAULT_LEVELDB_COMPRESSION_TYPE 0x0    
#define DEFAULT_LEVELDB_MAX_OPEN_FILES  -1      
#define DEFAULT_LEVELDB_SYNC    0x0             
#define DEFAULT_LEVELDB_DIR     "db/"           

/* 指针快速转换 */
#define OID2POINTER(type, var) (type)(pmemobj_direct(var))
#define POINTER2OID(var) (pmemobj_oid((const void *)var))

/* 哈希函数 */
#include <string>
#include <random>
#include <chrono>
#include <assert.h>
#include <cstring>
#define RANGE (UINT64_MAX / DEFAULT_PARTITION_NUMS)
class HashFunc {
  public:
    static unsigned int hash1(const char *data, unsigned int n) {
        uint64_t *num;
        num = (uint64_t *)data;
        size_t gid = *num / RANGE;
        return gid;
        return (unsigned char)data[n - 1] % DEFAULT_PARTITION_NUMS;
    }
    static unsigned int hash1(const std::string &data) {
        return data[data.size() - 1] % DEFAULT_PARTITION_NUMS;
    }

    static unsigned int hash (const char* data, unsigned int n) {
        unsigned int hash = 388650013;
        unsigned int scale = 388650179;
        unsigned int hardener  = 1176845762;
        while (n) {
            hash *= scale;
            hash += *data++;
            n--;
        }
        return hash ^ hardener;
    }

    static unsigned int hash (const char* data1, unsigned int n1, const char* data2, unsigned int n2) {
        unsigned int hash = 388650013;
        unsigned int scale = 388650179;
        unsigned int hardener  = 1176845762;
        unsigned int n = n1 + n2;
        while (n) {
            hash *= scale;
            if (n > n2)
                hash += *data1++;
            else
                hash += *data2++;
            n--;
        }
        return hash ^ hardener;
    }

    static unsigned int hash (const char* data1, unsigned int n1, const char* data2, unsigned int n2, const char* data3, unsigned int n3) {
        unsigned int hash = 388650013;
        unsigned int scale = 388650179;
        unsigned int hardener  = 1176845762;
        unsigned int n = n1 + n2 + n3;
        while (n) {
            hash *= scale;
            if (n > n2 + n3)
                hash += *data1++;
            else if (n > n3)
                hash += *data2++;
            else
                hash += *data3++;
            n--;
        }
        return hash ^ hardener;
    }

    static const std::string ALPHA_CHARS;

    static const std::string ALPHA_NUMS;

    static std::string generateKey(size_t length, const std::string &char_set) {
        if (length == 0 || char_set.empty()) {
            return "";
        }
        static std::mt19937 rng(std::chrono::high_resolution_clock::now()
                                .time_since_epoch()
                                .count());
        std::uniform_int_distribution<size_t> dist(0, char_set.length() - 1);

        std::string random_key;
        random_key.reserve(length);

        for (size_t i=0; i<length; i++) {
            random_key += char_set[dist(rng)];
        }
        assert(random_key.size() == length);
        return random_key;
    }

    static std::string generateKey(size_t gid, size_t length,
                                   const std::string &char_set) {
        if (length == 0 || char_set.empty()) {
            return "";
        }
        static std::mt19937 rng(std::chrono::high_resolution_clock::now()
                                .time_since_epoch()
                                .count());
        std::uniform_int_distribution<size_t> dist(0, char_set.length() - 1);

        std::string random_key;
        random_key.reserve(length);

        for (size_t i=0; i<length; i++) {
            random_key += char_set[dist(rng)];
        }
        char assigned = '0' + gid % 10;
        random_key[length - 1] = assigned;
        assert(random_key.size() == length);
        return random_key;
    }
};

/* 自旋锁 */
#include <atomic>
class SpinLock {
  public:
    SpinLock():flag(ATOMIC_FLAG_INIT) {}
    void lock() {
        while (flag.test_and_set()){} 
    }
    void unlock() {
        flag.clear();
    }
  private:
    std::atomic_flag flag;
};

/* 随机数生成 */
#include <random>
class RandomNums {
  public:
    RandomNums(int left, int right):rd(), gen(rd()), dis(left, right) {}
    ~RandomNums() {}

    int getRandomNum() { return dis(gen); } // 生成均匀分布随机数

  private:
    // 随机数生成器，使用默认种子
    std::random_device rd;
    std::mt19937 gen;
    // 定义一个均匀分布，范围从 1 到 100
    std::uniform_int_distribution<> dis;
};

#endif