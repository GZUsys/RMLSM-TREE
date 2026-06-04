#ifndef DEVICEMANAGER_H
#define DEVICEMANAGER_H

#include <libpmem2.h>
#include <libpmemobj.h>
#include <string.h>

#include <string>

#include "define.h"

class Device {
  public:
    Device() {}
    virtual ~Device() {};
};

class DevDaxDevice : public Device {
  public:
    DevDaxDevice(const std::string &devpath = DEFAULT_DAXDEV_PATH,
                 size_t devsize = DEFAULT_DAXDEV_SIZE);
    ~DevDaxDevice();

    pmem2_granularity get_granularity(pmem2_map *map);  // 拿到map的粒度
    void *get_global_address();  // 返回整个连续映射的PM空间

    void test();

  private:
    void *_addr;

    int fd;
    pmem2_persist_fn persist_fn;
    struct pmem2_config *cfg;
    struct pmem2_source *src;
    struct pmem2_map *map;
    struct pmem2_vm_reservation *vm_rsv;
};

class FsDaxDevice : public Device {
  public:
    FsDaxDevice(const std::string &path,
                const std::string &layout,
                size_t size);
    ~FsDaxDevice();

    bool open(const std::string &path, const std::string &layout);
    bool create(const std::string &path, const std::string &layout);
    void write();
    void read();
    void test();

    PMEMobjpool *pop;

  private:
    std::string _pool_path;
    std::string _pool_layout_name;
    size_t _size;
};

#endif