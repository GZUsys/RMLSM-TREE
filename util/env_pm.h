#ifndef EMERALD_ENV_PM_H
#define EMERALD_ENV_PM_H

#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#ifndef __Fuchsia__
#include <sys/resource.h>
#endif
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <queue>
#include <set>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

#include "leveldb/env.h"
#include "leveldb/slice.h"
#include "leveldb/status.h"
#include "port/port.h"
#include "port/thread_annotations.h"
#include "util/env_posix_test_helper.h"
#include "util/posix_logger.h"

#include <libpmem2.h>
#include <immintrin.h>
#include <map>

#define CACHE_LINE_SIZE (64)
#define PAGE_SIZE (4096)

namespace leveldb {

/* 工具函数 */
bool is_dax_filesystem(const std::string &filepath);
bool ends_with_ldb(const std::string &str);
Status PMError(const std::string& context, int error_number);

/* libpmem2 工具类 */
class Utils {
public:
    Utils() {}
    ~Utils() {}
 
    static int open(const std::string &path, int mode, struct pmem2_config *&cfg, struct pmem2_source *&src, struct pmem2_map *&map) {
        int fd = ::open(path.c_str(), mode, 0666);
        if (fd < 0) {
            pmem2_perror("error open");
            return fd;
        }

        if (pmem2_config_new(&cfg)) {
            pmem2_perror("error config new");
        }
        if (pmem2_source_from_fd(&src, fd)) {
            pmem2_perror("error from fd");
        }
        // 粒度
        if (pmem2_config_set_required_store_granularity(cfg, 
                    PMEM2_GRANULARITY_CACHE_LINE)) {
            pmem2_perror("error set store granulatiry");
        }
        if (pmem2_map_new(&map, cfg, src)) {
            pmem2_perror("error map new");
        }

        return fd;
    }

    static inline char *getaddr(struct pmem2_map *&map) {
        return (char*)pmem2_map_get_address(map);
    }

    static inline size_t getsize(struct pmem2_map *&map) {
        return pmem2_map_get_size(map);
    }

    static bool close(int fd, struct pmem2_config *&cfg, struct pmem2_source *&src, struct pmem2_map *&map) {
        pmem2_map_delete(&map);
        pmem2_source_delete(&src);
        pmem2_config_delete(&cfg);
        fsync(fd);
        ::close(fd);
        return true;
    }
};

/* 顺序读文件 */
class PMSequentialFile : public SequentialFile {
private:
    std::string _filename;

    struct pmem2_config *cfg;
    struct pmem2_source *src;
    struct pmem2_map *map;
    int fd;
    char *_addr;
    size_t _size;

    size_t pos;

public:
    PMSequentialFile(const std::string &path) {
        _filename = path;

        fd = Utils::open(path, O_RDWR, cfg, src, map);
        _addr = Utils::getaddr(map);
        _size = Utils::getsize(map);

        pos = 0;
    }
    virtual ~PMSequentialFile() {
        Utils::close(fd, cfg, src, map);
    }

    Status Read(size_t n, Slice *result, char *scratch) override {
        Status status;
        if (pos > _size) {
            *result = Slice();
            return PMError(_filename, ENODATA);
        }
        size_t remain = _size - pos;
        size_t to_read = (n <= remain) ? n : remain;
        if (to_read > 0) {
            memcpy(scratch, _addr + pos, to_read);
            pos += to_read;
        } 
        *result = Slice(scratch, to_read);

        return status;
    }
    Status Skip(uint64_t n) override {
        if (pos + n > _size)
            return PMError(_filename, ENODATA);
        pos += n;
        return Status::OK();
    }
};

/* 随机读文件 */
class PMRandomAccessFile : public RandomAccessFile {
private:
    std::string _filename;

    struct pmem2_config *cfg;
    struct pmem2_source *src;
    struct pmem2_map *map;
    int fd;
    char *_addr;
    size_t _size;

    // size_t pos;
public:
    PMRandomAccessFile(const std::string &path) {
        _filename = path;

        fd = Utils::open(path, O_RDWR, cfg, src, map);
        _addr = Utils::getaddr(map);
        _size = Utils::getsize(map);
    }
    virtual ~PMRandomAccessFile() {
        Utils::close(fd, cfg, src, map);
    }

    Status Read(uint64_t offset, size_t n, Slice* result,
                char* scratch) const override {
        if (offset + n > _size) {
            *result = Slice();
            return PMError(_filename, ENODATA);
        }
        // 这里可能有点问题? 忽略了scratch的语义?

        *result = Slice(_addr + offset, n);
        return Status::OK();
    }
};

/* clwb(刷新缓存) 只写文件 小于32MB的小文件 */
class PMclwbWriteFile : public WritableFile {
    // 预留空间32MB, 实际在leveldb中WAL最大4MB, 后期调优调小为16/8/4MB
    constexpr static size_t FILE_SIZE = 32 * 1024 * 1024;
private:
    std::string filename;

    struct pmem2_config *cfg;
    struct pmem2_source *src;
    struct pmem2_map *map;

    int fd;
    char *addr = nullptr;

    size_t mapped_size = 0;
    size_t pos = 0;
    bool is_closed = false;
public:
    pmem2_drain_fn drain_fn = nullptr;
    pmem2_flush_fn flush_fn = nullptr;

public:
    PMclwbWriteFile(const std::string& path) {
      filename = std::move(path);
      // 1. Create/Open the file
      fd = open(path.c_str(), O_RDWR | O_TRUNC | O_CREAT, 0644);
      if (fd == -1) {
        perror("Failed to open clwb file: \n");
        return;  // Error handled by LevelDB's caller checking s.ok()
      }

      // 2. Truncate to max estimated size
      if (ftruncate(fd, FILE_SIZE) == -1) {
        perror("Failed to truncate SSTable file: ");
        close(fd);
        fd = -1;
        return;
      }
      // 3. src + cfg
      if (pmem2_config_new(&cfg)||pmem2_source_from_fd(&src, fd)) {
        pmem2_perror("config new || from fd");
        cleanup();
        return ;
      }
      // 粒度
      if (pmem2_config_set_required_store_granularity(
              cfg, PMEM2_GRANULARITY_CACHE_LINE)) {
        pmem2_perror("set store granulatiry");
        cleanup();
        return ;
      }
      // 4.
      if (pmem2_map_new(&map, cfg, src)) {
        pmem2_perror("pmem2 map new\n");
        cleanup();
        return ;
      }

      addr = (char *)pmem2_map_get_address(map);
      mapped_size = pmem2_map_get_size(map);
      pos = 0;

      flush_fn = pmem2_get_flush_fn(map);
      drain_fn = pmem2_get_drain_fn(map);
    }
    ~PMclwbWriteFile() override {
        if (!is_closed)
            Close();
    }

    // 64B = 512bit 进行拷贝
    inline void avx_mov(char *dst, const char *src) {
        __m512i data = _mm512_loadu_si512(src);
        _mm512_storeu_si512(dst, data);
    }

    Status Append(const Slice &data) override {
        if (is_closed)
            return Status::IOError("File is close");
        if (!map || !addr)
            return Status::IOError("File is not map, addr is null");

        if (pos + data.size() > mapped_size)
            return Status::IOError("MAX SIZE EXCEEDED FOR " + filename);
        
        // 1: memcpy + pmem2_flush + pmem2_drain (2也可以)
        ::memcpy(addr+pos, data.data(), data.size());
        flush_fn(addr+pos, data.size());
        drain_fn();
        pos += data.size();

        // 2. memcpy + clwb + sfence
        // 3: avx + clwb + sfence 未对齐

        return Status::OK();
    }
    Status Close() override {
        if (is_closed)
            return Status::OK();
        is_closed = true;
        // if (drain_fn && pos > 0)
        //     drain_fn();
        if (map) {
            if (pmem2_map_delete(&map)) {
                pmem2_perror("pmem2 map delete");
                return Status::IOError("pmem2 map delete");
            }
            map = nullptr;
        }
        
        // 如果文件为空，直接删除
        if (fd != -1) {
            if (pos == 0) {
                cleanup();
                ::unlink(filename.c_str());  // 删除空文件
                return Status::OK();
            }
            
            // 截断到实际大小
            if (pos < FILE_SIZE) {
                if (ftruncate(fd, pos) != 0) {
                    pmem2_perror("ftruncate error");
                    cleanup();
                    return Status::IOError("ftruncate error");
                }
            }
        }

        cleanup();
        return Status::OK();
    }
    Status Flush() override {
        return Status::OK();
    }
    Status Sync() override {
        if (drain_fn)
            drain_fn();
        return Status::OK();
    }

    void cleanup() {
        if (src)
            pmem2_source_delete(&src);
        if (cfg)
            pmem2_config_delete(&cfg);
        if (fd != -1) {
            ::close(fd);
            fd = -1;
        }
        addr = nullptr;
        mapped_size = 0;
    }
};

/* ntstore(绕开缓存)+DRAM缓冲 只写文件 针对大文件 */
class PMntstoreWriteFile : public WritableFile {
    /* 最优值在 4~32 MB之间 */
    constexpr static size_t BUFFER_SIZE =  8 * 1024 * 1024;
    constexpr static size_t SEGMENT_SIZE = 32 * 1024 * 1024;
    constexpr static size_t VM_RESERVATION_MAXSIZE = 128 * 1024 * 1024; // 128MB
private:
    std::string filename;

    char buffer[BUFFER_SIZE]; // dram缓冲区
    size_t pos_buffer = 0;

    size_t last_mapped_end_offset = 0; // 当前已经映射的范围
    size_t current_file_offset = 0; // 当前文件大小

    int fd = -1;
    bool is_closed = false;

    pmem2_vm_reservation *vm_reservation = nullptr;
    std::map<size_t, pmem2_map *> segment_maps;

    std::vector<pmem2_source *> src_vec;
    std::vector<pmem2_config *> cfg_vec;
    std::vector<int> dupfds;
public:
    /* 将文件[offset, offset+size)映射到vm中  */
    Status MapNewSegment(size_t segment_start_offset, 
                         size_t segment_size) {
        pmem2_source *src = nullptr;
        pmem2_config *cfg = nullptr;
        pmem2_map *new_map = nullptr;
        
        int cloned_fd = ::dup(fd);
        if (cloned_fd == -1) {
            return Status::IOError("MapNewSegment dup error\n");
        }

        if (pmem2_source_from_fd(&src, fd)) {
            ::close(cloned_fd);
            return Status::IOError("MapNewSegment cfg||src error");
        }

        if (pmem2_config_new(&cfg)) {
            ::close(cloned_fd);
            pmem2_source_delete(&src);
            return Status::IOError("Failed to create pmem2_config");
        }

        /*  将文件[size, size+segment_size)映射到
            vm地址空间 [start_offset, start_offset+segment_size) */
        pmem2_config_set_offset(cfg, segment_start_offset);
        pmem2_config_set_length(cfg, segment_size);
        pmem2_config_set_required_store_granularity(cfg, PMEM2_GRANULARITY_CACHE_LINE);
        if (pmem2_config_set_vm_reservation(cfg, vm_reservation,
                                            segment_start_offset)) {
            pmem2_config_delete(&cfg);
            pmem2_source_delete(&src);
            perror("set vm reservation");
            return Status::IOError("set vm reservation");
        }
        if (pmem2_map_new(&new_map, cfg, src)) {
            pmem2_config_delete(&cfg);
            pmem2_source_delete(&src);
            perror("map new error");
            return Status::IOError("map new error");
        }

        segment_maps[segment_start_offset] = new_map;
        last_mapped_end_offset = segment_start_offset + segment_size;

        cfg_vec.push_back(cfg);
        src_vec.push_back(src);
        dupfds.push_back(cloned_fd);
        return Status::OK();
    }

    Status EnsurePMMappedSpace(size_t target_size) {
        Status s;
        while (target_size > last_mapped_end_offset) {
            size_t new_segment_start_offset = last_mapped_end_offset;
            size_t new_segment_size = SEGMENT_SIZE;

            // 扩容
            if (ftruncate(fd, new_segment_start_offset + new_segment_size))
                return Status::IOError("EnsurePMMappedSpace ftruncate error");

            // 创建新扩容区间的映射到vm中
            s = MapNewSegment(new_segment_start_offset, new_segment_size);
            if (!s.ok()) return s;
        }
        return Status::OK();
    }

    pmem2_map *GetMapForOffset(size_t offset) {
        auto it = segment_maps.upper_bound(offset);
        if (it == segment_maps.begin())
            return nullptr;
        --it;

        pmem2_map *candidate_map = it->second;
        size_t map_start_offset_in_file = it->first;
        size_t map_size = pmem2_map_get_size(candidate_map);

        if (offset >= map_start_offset_in_file &&
            offset < map_start_offset_in_file + map_size)
                return candidate_map;

        return nullptr;
    }

    size_t GetMapStartOffset(pmem2_map *map) {
        for (auto const& [offset, m_ptr] : segment_maps)
            if (m_ptr == map)
                return offset;
        return 0;
    }

    inline void persist(char *dst, char *src, size_t len, pmem2_map *map) {
        // 0. ntstore 绕开缓存 memmove
        pmem2_memmove_fn move_fn = pmem2_get_memmove_fn(map);
        move_fn(dst, src, len, PMEM2_F_MEM_NONTEMPORAL);
    }

    Status FlushDramBuffer2PM() {
        if (pos_buffer == 0)
            return Status::OK();
        
        Status s = EnsurePMMappedSpace(current_file_offset + pos_buffer);
        if (!s.ok())
            return s;
        
        size_t bytes_to_flush = pos_buffer;
        size_t current_flushed_bytes = 0;

        while (current_flushed_bytes < bytes_to_flush) {
            pmem2_map* target_map =
                GetMapForOffset(current_file_offset + current_flushed_bytes);
            if (!target_map)
                return Status::IOError(
                    "Failed to find PMEM map for buffer flush at offset: " +
                    std::to_string(current_file_offset + current_flushed_bytes));
            void *base_addr = pmem2_map_get_address(target_map);
            size_t map_start_offset = GetMapStartOffset(target_map);
            size_t map_size = pmem2_map_get_size(target_map);

            size_t dst_offset_in_map = (current_file_offset + current_flushed_bytes) - map_start_offset;
            size_t copy_len = std::min(bytes_to_flush - current_flushed_bytes, map_size - dst_offset_in_map);

            char *dst_addr = (char *)base_addr + dst_offset_in_map;
            char *src_addr = buffer + current_flushed_bytes;

            /* 持久化 */
            persist(dst_addr, src_addr, copy_len, target_map);

            current_flushed_bytes += copy_len;
        }

        current_file_offset += bytes_to_flush;
        pos_buffer = 0;
        return Status::OK();
    }

    PMntstoreWriteFile(const std::string &path) {
        Status s;
        filename = std::move(path);

        // 1. 创建本地文件
        fd = ::open(path.c_str(), O_RDWR | O_TRUNC | O_CREAT, 0644);
        if (fd == -1) {
            perror("Failed to open ntstore SSTable File\n");
            return ;
        }

        // 2. 创建vm
        if (pmem2_vm_reservation_new(&vm_reservation, nullptr, VM_RESERVATION_MAXSIZE)) {
            perror("Failed to new vm_reservation\n");
            ::close(fd);
            return ;
        }

        // 3. 创建第一个分段映射
        s = EnsurePMMappedSpace(SEGMENT_SIZE);
        if (!s.ok()) {
            ::close(fd);
            fd = -1;
            pmem2_vm_reservation_delete(&vm_reservation);
            return ;
        }
    }
    ~PMntstoreWriteFile() override {
        if (!is_closed)
            Close();
    }
    Status Append(const Slice &data) override {
        if (is_closed)
            return Status::IOError("File is close!!!");
        Status s;
        
        size_t data_size = data.size();
        const char *data_ptr = data.data();

        // 缓冲区总大小不够
        if (data_size > BUFFER_SIZE) {
            s = FlushDramBuffer2PM();
            if (!s.ok())
                return s;
            
            size_t written_bytes = 0;
            while (written_bytes < data_size) {
                size_t chunk_size = std::min(data_size - written_bytes, BUFFER_SIZE);

                s = EnsurePMMappedSpace(current_file_offset + chunk_size);
                if (!s.ok())
                    return s;
                
                pmem2_map *target_map = GetMapForOffset(current_file_offset);
                if (!target_map)
                    return Status::IOError(
                        "Failed to find PMEM map for direct write at offset" +
                        std::to_string(current_file_offset));
                void *base_addr = pmem2_map_get_address(target_map);
                size_t map_start_offset = GetMapStartOffset(target_map);
                char *dst_addr = (char *)base_addr + (current_file_offset - map_start_offset);

                /* 持久化 */
                persist(dst_addr, buffer, chunk_size, target_map);

                current_file_offset += chunk_size;
                written_bytes += chunk_size;
            }
            return Status::OK();
        }

        // 缓冲区当前大小不够, 但缓冲区总大小够
        if (pos_buffer + data_size > BUFFER_SIZE) {
            s = FlushDramBuffer2PM();
            if (!s.ok())
                return s;
        }

        // 写入缓冲区
        ::memcpy(buffer + pos_buffer, data_ptr, data_size);
        pos_buffer += data_size;

        return Status::OK();
    }
    Status Close() override {
        if (is_closed)
            return Status::OK();
        Status s;
        is_closed = true;
        // 1. 刷新mem buffer
        s = FlushDramBuffer2PM();
        if (!s.ok()) 
            return s;
        
        // 2. 取消所有映射map
        for (auto& [offset, map_obj] : segment_maps) {
            pmem2_map_delete(&map_obj);
        }
        segment_maps.clear();

        // 2.1 删除所有 cfg
        for (int i=0; i<cfg_vec.size(); i++)
            pmem2_config_delete(&cfg_vec[i]);
        // 2.2 删除所有 src
        for (int i=0; i<src_vec.size(); i++)
            pmem2_source_delete(&src_vec[i]);
        // 2.3 释放所有clone fd
        for (int i=0; i<dupfds.size(); i++)
            ::close(dupfds[i]);

        // 3. 设置文件大小
        if (fd != -1) {
            if (ftruncate(fd, current_file_offset)) {
                s = Status::IOError("Failed to truncate PMem File " + filename);
            }
            ::close(fd);
            fd = -1;
        }

        // 4. 删除整个vm
        if (vm_reservation) {
            pmem2_vm_reservation_delete(&vm_reservation);
        }
        return s;
    }
    Status Flush() override {
        return FlushDramBuffer2PM();
    }
    Status Sync() override {
        Status s = FlushDramBuffer2PM();
        if (!s.ok())
            return s;
        //  Drain all active PM segments
        // for (auto const& [offset, map_obj] : segment_maps) {
        //     pmem2_map_drain(map_obj);  // Direct libpmem2 call
        // }
        return Status::OK();
    }
};

/* ntstore 不带Dram缓冲 效率不如带DRAM缓冲 */
class PMntstoreDirectWriteFile : public WritableFile {
    /* 最优值在 4~32 MB之间 */
    constexpr static size_t SEGMENT_SIZE = 4 * 1024 * 1024;
    constexpr static size_t VM_RESERVATION_MAXSIZE = 128 * 1024 * 1024; // 16G
private:
    std::string filename;

    size_t last_mapped_end_offset = 0; // 当前已经映射的范围
    size_t current_file_offset = 0; // 当前文件大小

    int fd = -1;
    bool is_closed = false;

    pmem2_vm_reservation *vm_reservation = nullptr;
    std::map<size_t, pmem2_map *> segment_maps;

    std::vector<pmem2_source *> src_vec;
    std::vector<pmem2_config *> cfg_vec;
    std::vector<int> dupfds;
public:
    /* 将文件[offset, offset+size)映射到vm中  */
    Status MapNewSegment(size_t segment_start_offset, 
                         size_t segment_size) {
        pmem2_source *src = nullptr;
        pmem2_config *cfg = nullptr;
        pmem2_map *new_map = nullptr;
        
        int cloned_fd = ::dup(fd);
        if (cloned_fd == -1) {
            return Status::IOError("MapNewSegment dup error\n");
        }

        if (pmem2_source_from_fd(&src, fd)) {
            ::close(cloned_fd);
            return Status::IOError("MapNewSegment cfg||src error");
        }

        if (pmem2_config_new(&cfg)) {
            ::close(cloned_fd);
            pmem2_source_delete(&src);
            return Status::IOError("Failed to create pmem2_config");
        }

        /*  将文件[size, size+segment_size)映射到
            vm地址空间 [start_offset, start_offset+segment_size) */
        pmem2_config_set_offset(cfg, segment_start_offset);
        pmem2_config_set_length(cfg, segment_size);
        pmem2_config_set_required_store_granularity(cfg, PMEM2_GRANULARITY_CACHE_LINE);
        if (pmem2_config_set_vm_reservation(cfg, vm_reservation,
                                            segment_start_offset)) {
            pmem2_config_delete(&cfg);
            pmem2_source_delete(&src);
            perror("set vm reservation");
            return Status::IOError("set vm reservation");
        }
        if (pmem2_map_new(&new_map, cfg, src)) {
            pmem2_config_delete(&cfg);
            pmem2_source_delete(&src);
            perror("map new error");
            return Status::IOError("map new error");
        }

        segment_maps[segment_start_offset] = new_map;
        last_mapped_end_offset = segment_start_offset + segment_size;

        cfg_vec.push_back(cfg);
        src_vec.push_back(src);
        dupfds.push_back(cloned_fd);
        return Status::OK();
    }

    Status EnsurePMMappedSpace(size_t target_size) {
        Status s;
        while (target_size > last_mapped_end_offset) {
            size_t new_segment_start_offset = last_mapped_end_offset;
            size_t new_segment_size = SEGMENT_SIZE;

            // 扩容
            if (ftruncate(fd, new_segment_start_offset + new_segment_size))
                return Status::IOError("EnsurePMMappedSpace ftruncate error");

            // 创建新扩容区间的映射到vm中
            s = MapNewSegment(new_segment_start_offset, new_segment_size);
            if (!s.ok()) return s;
        }
        return Status::OK();
    }

    pmem2_map *GetMapForOffset(size_t offset) {
        auto it = segment_maps.upper_bound(offset);
        if (it == segment_maps.begin())
            return nullptr;
        --it;

        pmem2_map *candidate_map = it->second;
        size_t map_start_offset_in_file = it->first;
        size_t map_size = pmem2_map_get_size(candidate_map);

        if (offset >= map_start_offset_in_file &&
            offset < map_start_offset_in_file + map_size)
                return candidate_map;

        return nullptr;
    }

    size_t GetMapStartOffset(pmem2_map *map) {
        for (auto const& [offset, m_ptr] : segment_maps)
            if (m_ptr == map)
                return offset;
        return 0;
    }

    inline void persist(char *dst, char *src, size_t len, pmem2_map *map) {
        // 0. ntstore 绕开缓存
        pmem2_memmove_fn move_fn = pmem2_get_memmove_fn(map);
        move_fn(dst, src, len, PMEM2_F_MEM_NONTEMPORAL);
    }

    PMntstoreDirectWriteFile(const std::string &path) {
        Status s;
        filename = std::move(path);

        // 1. 创建本地文件
        fd = ::open(path.c_str(), O_RDWR | O_TRUNC | O_CREAT, 0644);
        if (fd == -1) {
            perror("Failed to open ntstore SSTable File\n");
            return ;
        }

        // 2. 创建vm
        if (pmem2_vm_reservation_new(&vm_reservation, nullptr, VM_RESERVATION_MAXSIZE)) {
            perror("Failed to new vm_reservation\n");
            ::close(fd);
            return ;
        }

        // 3. 创建第一个分段映射
        s = EnsurePMMappedSpace(SEGMENT_SIZE);
        if (!s.ok()) {
            ::close(fd);
            fd = -1;
            pmem2_vm_reservation_delete(&vm_reservation);
            return ;
        }
    }
    ~PMntstoreDirectWriteFile() override {
        if (!is_closed)
            Close();
    }
    Status Append(const Slice &data) override {
        if (is_closed)
            return Status::IOError("File is close!!!");
        Status s;
        
        size_t data_size = data.size();
        char *data_ptr = (char *)data.data();

        // 写入量比Segment_Size大 或者 跨越两个Segment_Size
        if (data_size > SEGMENT_SIZE || 
            current_file_offset % SEGMENT_SIZE + data_size > SEGMENT_SIZE) {
            size_t written_bytes = 0;
            while (written_bytes < data_size) {
                size_t chunk_size = std::min(data_size - written_bytes, SEGMENT_SIZE);

                s = EnsurePMMappedSpace(current_file_offset + chunk_size);
                if (!s.ok())
                    return s;
                
                pmem2_map *target_map = GetMapForOffset(current_file_offset);
                if (!target_map)
                    return Status::IOError(
                        "Failed to find PMEM map for direct write at offset" +
                        std::to_string(current_file_offset));
                void *base_addr = pmem2_map_get_address(target_map);
                size_t map_start_offset = GetMapStartOffset(target_map);
                char *dst_addr = (char *)base_addr + (current_file_offset - map_start_offset);

                /* 持久化 */
                persist(dst_addr, data_ptr + written_bytes, chunk_size, target_map);

                current_file_offset += chunk_size;
                written_bytes += chunk_size;
            }
            return Status::OK();
        }

        // 在segment_size内
        pmem2_map *target_map = GetMapForOffset(current_file_offset);
        if (!target_map)
            return Status::IOError(
                "Failed to find PMEM map for direct write at offset" +
                std::to_string(current_file_offset));
        void *base_addr = pmem2_map_get_address(target_map);
        size_t map_start_offset = GetMapStartOffset(target_map);
        char *dst_addr = (char *)base_addr + (current_file_offset - map_start_offset);
        persist(dst_addr, data_ptr, data_size, target_map);
        current_file_offset += data_size;

        return Status::OK();
    }
    Status Close() override {
        if (is_closed)
            return Status::OK();
        Status s;
        is_closed = true;
        // 1. 刷新mem buffer
        
        // 2. 取消所有映射map
        for (auto& [offset, map_obj] : segment_maps) {
            pmem2_map_delete(&map_obj);
        }
        segment_maps.clear();

        // 2.1 删除所有 cfg
        for (int i=0; i<cfg_vec.size(); i++)
            pmem2_config_delete(&cfg_vec[i]);
        // 2.2 删除所有 src
        for (int i=0; i<src_vec.size(); i++)
            pmem2_source_delete(&src_vec[i]);
        // 2.3 释放所有clone fd
        for (int i=0; i<dupfds.size(); i++)
            ::close(dupfds[i]);

        // 3. 设置文件大小
        if (fd != -1) {
            if (ftruncate(fd, current_file_offset)) {
                s = Status::IOError("Failed to truncate PMem File " + filename);
            }
            ::close(fd);
            fd = -1;
        }

        // 4. 删除整个vm
        if (vm_reservation) {
            pmem2_vm_reservation_delete(&vm_reservation);
        }
        return s;
    }
    Status Flush() override {
        return Status::OK();
    }
    Status Sync() override {
        return Status::OK();
    }
};

};

#endif