#include "leveldb/db.h"
#include "gtest/gtest.h"
#include <random>
#include <vector>
#include <map>

// 生成随机字符串
static std::string generateRandomString(size_t length) {
    const std::string characters = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::string result;
    result.reserve(length);
    std::mt19937_64 rng(std::random_device{}());
    std::uniform_int_distribution<> dist(0, characters.size() - 1);
    for (size_t i = 0; i < length; ++i) {
        result += characters[dist(rng)];
    }
    return result;
}

// 生成随机数字
static size_t generateRandomNum(size_t left, size_t right) {
    std::mt19937_64 rng(std::random_device{}());
    std::uniform_int_distribution<> dist(left, right);
    return dist(rng);
}

// small value point read
TEST(GET, small_value_get) {
    leveldb::DB* db;
    leveldb::Options options;
    options.create_if_missing = true;

    // 打开或创建数据库
    leveldb::Status status = leveldb::DB::Open(options, "/tmp/testdb", &db);
    EXPECT_TRUE(status.ok());
    if (!status.ok()) {
        std::cerr << "无法打开或创建数据库: " << status.ToString() << std::endl;
    }
    std::cout << "数据库已成功打开。" << std::endl;

    std::vector<std::string> ans_value;
    size_t count = 500000;
    // 写入前 count 条键值对，value 小于 128 字节
    for (int i = 0; i < count; ++i) {
        std::string key = "key_small_" + std::to_string(i);
        std::string value = generateRandomString(100); // 100 字节
        ans_value.push_back(value);

        status = db->Put(leveldb::WriteOptions(), key, value);
        EXPECT_TRUE(status.ok());
        if (!status.ok()) {
            std::cerr << "写入失败: " << status.ToString() << std::endl;
        }
    }
    std::cout << "已写入 "<< count <<" 条 value 小于 128 字节的数据。" << std::endl;

    // 验证部分数据
    std::cout << "\n开始验证部分数据..." << std::endl;
    std::string retrieved_value;
    for (int i=0; i<count; i++) {
        std::string key = "key_small_" + std::to_string(i);
        status = db->Get(leveldb::ReadOptions(), key, &retrieved_value);
        EXPECT_TRUE(status.ok());
        EXPECT_EQ(retrieved_value, ans_value[i]);
    }

    // 关闭数据库并清理资源
    delete db;
    std::cout << "\n测试完成，数据库已关闭。" << std::endl;
}

// large value point read
TEST(GET, larget_value_get) {
    leveldb::DB* db;
    leveldb::Options options;
    options.create_if_missing = true;

    // 打开或创建数据库
    leveldb::Status status = leveldb::DB::Open(options, "/tmp/testdb", &db);
    EXPECT_TRUE(status.ok());
    if (!status.ok()) {
        std::cerr << "无法打开或创建数据库: " << status.ToString() << std::endl;
    }
    std::cout << "数据库已成功打开。" << std::endl;

    std::vector<std::string> ans_value;
    size_t count = 100000;
    // 写入后 count 条键值对，value 大于 128 字节
    for (int i = 0; i < count; ++i) {
        std::string key = "key_large_" + std::to_string(i);
        std::string value = generateRandomString(4096); // 200 字节
        ans_value.push_back(value);

        status = db->Put(leveldb::WriteOptions(), key, value);
        EXPECT_TRUE(status.ok());
        if (!status.ok()) {
            std::cerr << "写入失败: " << status.ToString() << std::endl;
        }
    }
    std::cout << "已写入 " << count << " 条 value 大于 128 字节的数据。" << std::endl;

    // 验证部分数据
    std::cout << "\n开始验证部分数据..." << std::endl;
    std::string retrieved_value;
    for (int i=0; i<count; i++) {
        std::string key = "key_large_" + std::to_string(i);
        status = db->Get(leveldb::ReadOptions(), key, &retrieved_value);
        EXPECT_TRUE(status.ok());
        EXPECT_EQ(retrieved_value, ans_value[i]);
    }

    // 关闭数据库并清理资源
    delete db;
    std::cout << "\n测试完成，数据库已关闭。" << std::endl;
}

// fix value point read
TEST(GET, fix_value_get) {
    leveldb::DB* db;
    leveldb::Options options;
    options.create_if_missing = true;

    // 打开或创建数据库
    leveldb::Status status = leveldb::DB::Open(options, "/tmp/testdb", &db);
    EXPECT_TRUE(status.ok());
    if (!status.ok()) {
        std::cerr << "无法打开或创建数据库: " << status.ToString() << std::endl;
    }
    std::cout << "数据库已成功打开。" << std::endl;

    std::vector<std::string> ans_value;
    size_t count = 85000;
    // 写入后 count 条键值对，value 大于 128 字节
    for (int i = 0; i < count; ++i) {
        std::string key = "key_large_" + std::to_string(i);
        std::string value = generateRandomString(
            generateRandomNum(64, 1024 * 16));  // 200 字节
        ans_value.push_back(value);

        status = db->Put(leveldb::WriteOptions(), key, value);
        EXPECT_TRUE(status.ok());
        if (!status.ok()) {
            std::cerr << "写入失败: " << status.ToString() << std::endl;
        }
    }
    std::cout << "已写入 " << count << " 条 value 大于 128 字节的数据。" << std::endl;

    // 验证部分数据
    std::cout << "\n开始验证部分数据..." << std::endl;
    std::string retrieved_value;
    for (int i=0; i<count; i++) {
        std::string key = "key_large_" + std::to_string(i);
        status = db->Get(leveldb::ReadOptions(), key, &retrieved_value);
        EXPECT_TRUE(status.ok());
        EXPECT_EQ(retrieved_value, ans_value[i]);
    }

    // 关闭数据库并清理资源
    delete db;
    std::cout << "\n测试完成，数据库已关闭。" << std::endl;

}

TEST(Scan, small_value_scan) {
    leveldb::DB *db;
    leveldb::Options options;
    options.create_if_missing = true;
    const std::string db_path = "/tmp/testdb_scan";

    leveldb::DestroyDB(db_path, options);
    leveldb::Status status = leveldb::DB::Open(options, db_path, &db);
    EXPECT_TRUE(status.ok());
    if (!status.ok()) {
        std::cerr << "无法打开或创建数据库: " << status.ToString() << std::endl;
        return ;
    }
    std::cout << "数据库已成功打开，开始写入数据..." << std::endl;

    size_t count = 80000;
    std::unordered_map<std::string, std::string> verified_map;
    for (size_t i = 0; i < count; i++) {
        std::string key = "key_small_" + std::to_string(i);
        std::string value = generateRandomString(100);
        verified_map.insert({key, value});

        status = db->Put(leveldb::WriteOptions(), key, value);
        EXPECT_TRUE(status.ok());
    }

    std::cout << "\n扫描并验证所有数据" << std::endl;
    leveldb::Iterator *it = db->NewIterator(leveldb::ReadOptions());
    size_t scan_count = 0;
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        std::string readkey = it->key().ToString();
        auto findit = verified_map.find(readkey);
        EXPECT_NE(findit, verified_map.end());
        
        std::string readvalue = it->value().ToString();
        EXPECT_EQ(readvalue, findit->second);

        scan_count++;
    }
    EXPECT_EQ(scan_count, count);

    delete it;
    delete db;
}

TEST(Scan, large_value_scan) {
    leveldb::DB *db;
    leveldb::Options options;
    options.create_if_missing = true;
    const std::string db_path = "/tmp/testdb_scan";

    leveldb::DestroyDB(db_path, options);
    leveldb::Status status = leveldb::DB::Open(options, db_path, &db);
    EXPECT_TRUE(status.ok());
    if (!status.ok()) {
        std::cerr << "无法打开或创建数据库: " << status.ToString() << std::endl;
        return ;
    }
    std::cout << "数据库已成功打开，开始写入数据..." << std::endl;

    size_t count = 80000;
    std::unordered_map<std::string, std::string> verified_map;
    for (size_t i = 0; i < count; i++) {
        std::string key = "key_small_" + std::to_string(i);
        std::string value = generateRandomString(8192);
        verified_map.insert({key, value});

        status = db->Put(leveldb::WriteOptions(), key, value);
        EXPECT_TRUE(status.ok());
    }

    std::cout << "\n扫描并验证所有数据" << std::endl;
    leveldb::Iterator *it = db->NewIterator(leveldb::ReadOptions());
    size_t scan_count = 0;
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        std::string readkey = it->key().ToString();
        auto findit = verified_map.find(readkey);
        EXPECT_NE(findit, verified_map.end());
        
        std::string readvalue = it->value().ToString();
        EXPECT_EQ(readvalue, findit->second);

        scan_count++;
    }
    EXPECT_EQ(scan_count, count);

    delete it;
    delete db;
}

TEST(Scan, fix_value_scan) {
    leveldb::DB *db;
    leveldb::Options options;
    options.create_if_missing = true;
    const std::string db_path = "/tmp/testdb_scan";

    leveldb::DestroyDB(db_path, options);
    leveldb::Status status = leveldb::DB::Open(options, db_path, &db);
    EXPECT_TRUE(status.ok());
    if (!status.ok()) {
        std::cerr << "无法打开或创建数据库: " << status.ToString() << std::endl;
        return ;
    }
    std::cout << "数据库已成功打开，开始写入数据..." << std::endl;

    size_t count = 85000;
    std::unordered_map<std::string, std::string> verified_map;
    for (size_t i = 0; i < count; i++) {
        std::string key = "key_small_" + std::to_string(i);
        std::string value = generateRandomString(
            generateRandomNum(64, 1024 * 16)
        );
        verified_map.insert({key, value});

        status = db->Put(leveldb::WriteOptions(), key, value);
        EXPECT_TRUE(status.ok());
    }

    std::cout << "\n扫描并验证所有数据" << std::endl;
    leveldb::Iterator *it = db->NewIterator(leveldb::ReadOptions());
    size_t scan_count = 0;
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        std::string readkey = it->key().ToString();
        auto findit = verified_map.find(readkey);
        EXPECT_NE(findit, verified_map.end());
        
        std::string readvalue = it->value().ToString();
        EXPECT_EQ(readvalue, findit->second);

        scan_count++;
    }
    EXPECT_EQ(scan_count, count);

    delete it;
    delete db;
}

int main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}