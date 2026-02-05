#include <gtest/gtest.h>
#include <string>
#include <thread>
#include <vector>
#include <memory>
#include "proxy_cache.hpp"

using namespace proxy_cache;

// --- Test Fixture ---
// This class runs before EVERY test to give us a fresh cache.
class CacheTest : public ::testing::Test {
protected:
    std::unique_ptr<Cache> cache;

    void SetUp() override {
        cache = std::make_unique<Cache>();
    }

    void TearDown() override {
        cache.reset();
    }
};

// --- TEST CASE 1: Basic Add & Find ---
TEST_F(CacheTest, HandlesBasicAddAndFind) {
    std::string url = "http://example.com";
    std::vector<char> data = {'H', 'e', 'l', 'l', 'o', ' ', 'W', 'o', 'r', 'l', 'd'};

    cache->cache_add(url, data);
    
    EXPECT_FALSE(cache->cache_find(url).empty());
    EXPECT_EQ(cache->cache_find(url), data);
}

// --- TEST CASE 2: LRU Eviction ---
TEST_F(CacheTest, EvictsLeastRecentlyUsedItem) {
    std::string url1 = "http://1.com"; std::vector<char> data1(26, 'A');
    std::string url2 = "http://2.com"; std::vector<char> data2(27, 'B');
    std::string url3 = "http://3.com"; std::vector<char> data3(26, 'C');
    std::string url4 = "http://4.com"; std::vector<char> data4(39, 'D');

    cache->cache_add(url1, data1);
    cache->cache_add(url2, data2);
    cache->cache_add(url3, data3);

    EXPECT_FALSE(cache->cache_find(url1).empty());
    cache->cache_add(url4, data4);

    EXPECT_FALSE(cache->cache_find(url4).empty());
    EXPECT_FALSE(cache->cache_find(url1).empty());
    EXPECT_FALSE(cache->cache_find(url3).empty());

    EXPECT_TRUE(cache->cache_find(url2).empty()) << "url2 was the LRU and should be evicted.";
}

// --- TEST CASE 3: Thread Safety ---
TEST_F(CacheTest, IsThreadSafe) {
    const int NUM_THREADS = 8;
    const int OPS_PER_THREAD = 100;
    std::vector<std::thread> threads;

    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back([this, i]() {
            for (int j = 0; j < OPS_PER_THREAD; ++j) {
                std::string url = "http://t" + std::to_string(i) + "-" + std::to_string(j);
                std::vector<char> data{'d', 'a', 't', 'a'};
                cache->cache_add(url, data);
                cache->cache_find(url);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    SUCCEED();
}

//--- TEST CASE 4: Missing URL ---
TEST_F(CacheTest, ReturnsEmptyOnMiss) {
    EXPECT_TRUE(cache->cache_find("http://missing.com").empty());
}

//--- TEST CASE 5: Reject Empty URL ---
TEST_F(CacheTest, RejectsEmptyUrl) {
    std::vector<char> data = {'x'};
    cache->cache_add("", data);

    EXPECT_TRUE(cache->cache_find("").empty());
}


//---TEST CASE 6: Reject empty data---
TEST_F(CacheTest, RejectsEmptyData) {
    std::string url = "http://emptydata.com";
    std::vector<char> data;

    cache->cache_add(url, data);

    EXPECT_TRUE(cache->cache_find(url).empty());
}

//---TEST CASE 7: Reject Oversized Data---
TEST_F(CacheTest, RejectsOversizedObject) {
    std::string url = "http://too-big.com";
    std::vector<char> hugeData(MAX_CACHE_BYTES + 1, 'H');

    cache->cache_add(url, hugeData);

    EXPECT_TRUE(cache->cache_find(url).empty());
}

//--- TEST CASE 8: Overwrite Existing URL ---
TEST_F(CacheTest, OverwritesExistingUrl) {
    std::string url = "http://overwrite.com";

    std::vector<char> oldData = {'o', 'l', 'd'};
    std::vector<char> newData = {'n', 'e', 'w'};

    cache->cache_add(url, oldData);
    EXPECT_EQ(cache->cache_find(url), oldData);

    cache->cache_add(url, newData);
    EXPECT_EQ(cache->cache_find(url), newData);
}

//--- TEST CASE 9: Eviction When Total Size Exceeded ---
TEST_F(CacheTest, EvictsWhenTotalSizeExceeded) {
    std::vector<char> data50(50, 'A');

    cache->cache_add("http://1.com", data50);
    cache->cache_add("http://2.com", data50);

    EXPECT_FALSE(cache->cache_find("http://1.com").empty());
    EXPECT_FALSE(cache->cache_find("http://2.com").empty());

    cache->cache_add("http://3.com", data50);

    EXPECT_TRUE(cache->cache_find("http://1.com").empty());
    EXPECT_FALSE(cache->cache_find("http://2.com").empty());
    EXPECT_FALSE(cache->cache_find("http://3.com").empty());
}

//TEST CASE 10: Access Updates Recency
TEST_F(CacheTest, FindPromotesToMostRecentlyUsed) {
    std::vector<char> data30(30, 'X');

    cache->cache_add("http://1.com", data30);
    cache->cache_add("http://2.com", data30);
    cache->cache_add("http://3.com", data30);

    EXPECT_FALSE(cache->cache_find("http://1.com").empty());

    cache->cache_add("http://4.com", data30);

    EXPECT_TRUE(cache->cache_find("http://2.com").empty());
    EXPECT_FALSE(cache->cache_find("http://1.com").empty());
}

//TEST CASE 11: Evict Multiple Items If Needed
TEST_F(CacheTest, EvictsMultipleItemsToMakeSpace) {
    std::vector<char> data40(40, 'Z');

    cache->cache_add("http://a.com", data40);
    cache->cache_add("http://b.com", data40);
    cache->cache_add("http://c.com", data40);

    std::vector<char> data80(80, 'Y');
    cache->cache_add("http://big.com", data80);

    EXPECT_TRUE(cache->cache_find("http://a.com").empty());
    EXPECT_TRUE(cache->cache_find("http://b.com").empty());
    EXPECT_FALSE(cache->cache_find("http://big.com").empty());
}