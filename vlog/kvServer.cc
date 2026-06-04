#include "kvServer.h"
#include "define.h"

#include <sys/time.h>
#include <spdlog/spdlog.h>

const std::string HashFunc::ALPHA_CHARS =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
const std::string HashFunc::ALPHA_NUMS = "0123456789";

vlogManager::vlogManager(const std::string &db_name) {
    spdlog::set_level(spdlog::level::warn);
    spdlog::info("vlogManager::vlogManager()");

    _groupManager = std::make_shared<GroupManager>(db_name);
    _gcManager = std::make_shared<gcManager>(*_groupManager.get());
}

vlogManager::~vlogManager() {
    spdlog::info("vlogManager::~vlogManager()");
}


Location vlogManager::putValue_separation(const char *key, uint64_t keySize, const char *value,
                        uint64_t valueSize) {
    uint64_t gid = HashFunc::hash1(key, keySize);

    valueLocation write_place = (*_groupManager)[gid]._vlog->appendWrite_fast(
                                    key, keySize, value, valueSize);

    maptableLocation LA = (*_groupManager)[gid]._maptable->appendWrite(
                                MapEntry(1, POINTER2OID(write_place.get_addr())));
    assert(((MapEntry *)(LA.get_addr()))->valid == true);

    return Location(POINTER2OID(LA.get_addr()));
}

bool vlogManager::getValue(size_t keySize, std::string *&value) {
    Location ret;
    ret.deserialize(*value);
    
    if (ret.in_lsm) { 

        *value = std::move(ret.value);

    } else {
        size_t valueSize = 0;

        MapEntry *entry = (MapEntry *)ret.get_addr();
        char *vstore = OID2POINTER(char *, entry->pa);
        Block::readData(vstore + sizeof(size_t) + keySize, &valueSize, sizeof(size_t));
        *value = std::move(std::string(vstore + sizeof(size_t) + keySize + sizeof(size_t), valueSize));
    }
    return true;
}

leveldb::Slice vlogManager::iterValue(const leveldb::Slice &value) {
    Location ret;
    ret.deserialize(value.ToString());

    if (ret.in_lsm)
        return leveldb::Slice(value.data()+1, value.size()-1);
    size_t valueSize = 0, keySize = 0;
    MapEntry *entry = (MapEntry *)ret.get_addr();
    if (entry) {
        char *vstore = OID2POINTER(char *, entry->pa);
        Block::readData(vstore, &keySize, sizeof(size_t));
        Block::readData(vstore + sizeof(size_t) + keySize, &valueSize, sizeof(size_t));
        leveldb::Slice readval(vstore + sizeof(size_t) + keySize + sizeof(size_t), valueSize);
        return readval;
    } 
    return leveldb::Slice(nullptr, 0);
}

// bool vlogManager::delValue(const char *key, size_t keySize) {
//     return _keyManager->deleteKey(key, keySize);
// }
// 
// void vlogManager::manual_compaction(std::string &begin, std::string &end) {
//     return _keyManager->manual_compaction(begin, end);
// }