#include "groupManager.h"
#include "define.h"
#include <cstddef>

std::pair<size_t, size_t> Group::UsedBytes() const {
    size_t maptable = _maptable ? _maptable->UsedBytes() : 0;
    size_t vlog = _vlog ? _vlog->UsedBytes() : 0;
    return {maptable, vlog};
}

std::pair<size_t, size_t> GroupManager::UsedBytes() const {
    size_t maptable = 0, vlog = 0;
    for (const auto& g : partitions) {
        if (g) {
            auto [mt, vg] = g->UsedBytes();
            maptable += mt;
            vlog += vg;
        }
    }
    return {maptable, vlog};
}

std::tuple<uint64_t, uint64_t, uint64_t> GroupManager::WriteBytesDetail() const {
    uint64_t maptable = 0, pmblock = 0, gc = 0;
    for (const auto& g : partitions) {
        if (g) {
            auto [mt, pm, gc_bytes] = g->WriteBytesDetail();
            maptable += mt;
            pmblock += pm;
            gc += gc_bytes;
        }
    }
    return {maptable, pmblock, gc};
}

#include <libpmemobj.h>
#include <spdlog/spdlog.h>

#include <exception>
#include <string>
#include <vector>
#include <iostream>
#include <filesystem>

Group::Group(FsDaxDevice *fsdev, int gid)
    : pool(fsdev), _gid_dram(gid), _block_allocator() {
    PMEMoid meta_oid = pmemobj_root(pool->pop, sizeof(Block));
    if (OID_IS_NULL(meta_oid)) {
        spdlog::error("meta_oid is null");
        return;
    }
    metaroot *meta_ptr = static_cast<metaroot *>(pmemobj_direct(meta_oid));
    if (!check_list_exist()) {
        spdlog::info("Group::Group() init_list");
        meta_ptr->magicNumber = (uint64_t)(meta_oid.pool_uuid_lo);
        _magic_number = (uint64_t)(meta_oid.pool_uuid_lo);

        std::vector<BlockInfo> tmp_block;
        for (size_t i = 0; i < DEFAULT_GROUP_BLOCK_NUMS ; i++) {
            BlockInfo new_node = obj_alloc_block(); 
            tmp_block.push_back(new_node);
            meta_ptr->all_block_oid[meta_ptr->all_block_count++] = new_node.block_oid;
        }
        _block_allocator = tmp_block;

        spdlog::info("magnicNumber = 0x{:x} total_blocks = {}", meta_ptr->magicNumber, _block_allocator.total_blocks());

        BlockInfo block = alloc_block();
        Block::change_allocated(get_pop(), block.block_oid, 2); 
        meta_ptr->write_block = block.block_oid;
        meta_ptr->block_off = 0;
        valueLocation vlog_write_loc =
            valueLocation(OID2POINTER(Block *, block.block_oid), 0);
        _vlog = new valueManager(*this, vlog_write_loc);

        if (pmemobj_zalloc(get_pop(), &(meta_ptr->write_maptab), DEFAULT_MAPTAB_SIZE,
                           1) == -1) {
            perror("Group::Group");
            throw std::runtime_error("error");
        }
        meta_ptr->maptab_off = 0;
        maptableLocation maptab_write_loc = maptableLocation(
                                                OID2POINTER(MapTable *, meta_ptr->write_maptab), 0);
        _maptable = new MapTableManager(*this, maptab_write_loc, DEFAULT_MAPTAB_SIZE);

        pmemobj_flush(pool->pop, meta_ptr, DEFAULT_PMOBJ_ROOT_SIZE);
        pmemobj_drain(pool->pop);

        _gid_dram = gid;
    } else { 
        spdlog::info("Group::Group() read_list");
        std::vector<BlockInfo> allblocks;
        for (size_t i=0; i<meta_ptr->all_block_count; i++) {
            Block *block = OID2POINTER(Block *, meta_ptr->all_block_oid[i]);
            allblocks.push_back(BlockInfo(block->id, POINTER2OID(block),
                                          block->is_allocated, block->valid_kv));
        }
        _block_allocator.recovery(allblocks);

        assert(!OID_IS_NULL(meta_ptr->write_block));
        _magic_number = meta_ptr->magicNumber;

        maptableLocation maptab_write_loc =
            maptableLocation(OID2POINTER(MapTable *, meta_ptr->write_maptab),
                             meta_ptr->maptab_off);
        _maptable = new MapTableManager(*this, maptab_write_loc, DEFAULT_MAPTAB_SIZE);

        valueLocation vlog_write_loc = valueLocation(
                                           OID2POINTER(Block *, meta_ptr->write_block), meta_ptr->block_off);
        _vlog = new valueManager(*this, vlog_write_loc);
    }
}

Group::~Group() {
    metaroot *obj_root = get_pm_root();
    obj_root->write_block = POINTER2OID(get_vlog_write_loc().block);
    obj_root->block_off = get_vlog_write_loc().block_off;

    obj_root->write_maptab = POINTER2OID(_maptable->get_maptab_write_loc().table);
    obj_root->maptab_off = _maptable->get_maptab_write_loc().table_off;

    pmemobj_persist(get_pop(), obj_root, CACHE_LINE_SIZE);

    delete pool;
    delete _maptable;
    delete _vlog;

    spdlog::info("Group::~Group()");
}

metaroot *Group::get_pm_root() {
    PMEMoid meta_root = pmemobj_root(pool->pop, sizeof(metaroot));
    if (OID_IS_NULL(meta_root)) {
        spdlog::error("oid_is_null ");
        return nullptr;
    }
    metaroot *meta_ptr = static_cast<metaroot *>(pmemobj_direct(meta_root));
    return meta_ptr;
}

bool Group::check_list_exist() {
    PMEMoid meta_root = pmemobj_root(pool->pop, sizeof(metaroot));
    if (OID_IS_NULL(meta_root)) {
        spdlog::error("oid_is_null ");
        return false;
    }
    metaroot *meta_ptr = static_cast<metaroot *>(pmemobj_direct(meta_root));
    if (meta_ptr->magicNumber == 0)
        return false;
    return true;
}

BlockInfo Group::obj_alloc_block() {
    PMEMobjpool *pop = pool->pop;
    PMEMoid new_node_oid;
    static size_t _counter = 0;
    uint64_t id = _magic_number + _counter++;
    // spdlog::info("allocated id = {}", id);
    int ret =
        pmemobj_alloc(pop, &new_node_oid, sizeof(Block), 0,
                      Block::block_construct, (void *)(&id));
    if (ret == -1) {
        perror("Group::alloc_node");
        throw std::runtime_error("error");
        return BlockInfo(0, OID_NULL);
    }
    assert(!OID_IS_NULL(new_node_oid));
    return BlockInfo(id, new_node_oid);
}

void Group::obj_free_block(PMEMoid Block) {
    pmemobj_free(&Block);
}

BlockInfo Group::alloc_block() {
    BlockInfo block = _block_allocator.allocate();
    Block::change_allocated(this->get_pop(), block.block_oid, 1);
    return block;
}
void Group::dealloc_block(BlockInfo &block) {
    _block_allocator.free(block.id);
    Block::change_allocated(this->get_pop(), block.block_oid, 0);
    return ;
}

void Group::test() {
    // spdlog::warn("_free_block_count = {}", _free_block_count);
}

valueLocation Group::appendWrite(const char *key, size_t keySize, const char *value,
                                 size_t valueSize) {
    bool ret;

    size_t all_write_size = keySize + sizeof(size_t) + valueSize;
    valueLocation cur = get_vlog_write_loc();

    if (!Block::canFit(cur.block, cur.block_off, all_write_size)) {
        BlockInfo newblock = alloc_block();
        cur.block = OID2POINTER(Block *, newblock.block_oid);
        cur.block_off = 0;
    }
    valueLocation tmp = cur;

    ret = Block::appendWrite(get_pop(), cur.block, cur.block_off, key, keySize);
    assert(ret);
    ret = Block::appendWrite(get_pop(), cur.block, cur.block_off,
                             &valueSize, sizeof(size_t));
    assert(ret);
    if (valueSize > 0) 
        ret = Block::appendWrite(get_pop(), cur.block,
                                 cur.block_off, value,
                                 valueSize);
    assert(ret);

    get_vlog_write_loc() = cur; 
    return tmp;
}

static std::string dirname(const std::string &path) {
    std::filesystem::path p(path);
    return p.parent_path().string();
}
static std::string basename(const std::string &path) {
    std::filesystem::path p(path);
    return p.stem().string();
}

GroupManager::GroupManager(const std::string& str, DevDaxDevice* devptr)
    : db_name(str), _device_ptr(devptr) {}

GroupManager::~GroupManager() {
    // destory(); 
    for (auto i : partitions)
        if (i) delete i;
}

void GroupManager::init() {
    if (!is_open) {
        int partition_num = DEFAULT_PARTITION_NUMS;
        size_t partition_size = DEFAULT_POOL_SIZE; // 50GB
        std::string fsdax_dir(DEFAULT_FSDAX_DIR);
        for (int i = 0; i < partition_num; i++) {
            std::string filepath =
                fsdax_dir + basename(db_name) + std::string(DEFAULT_PARTITION_NAME) + std::to_string(i);
            partitions_path.push_back(filepath);
            partitions.push_back(new Group(
                                    new FsDaxDevice(filepath, DEFAULT_POOL_LAYOUT_NAME, partition_size),
                                    i));
        }
        is_open = true;
    }
}

void GroupManager::destory() {
    if (is_open)
        for (size_t i = 0; i < partitions_path.size(); i++) {
            ::unlink(partitions_path[i].c_str());
        }
    is_open = false;
}
