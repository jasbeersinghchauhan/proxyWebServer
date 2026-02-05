#pragma once

#include <memory>
#include <string>
#include <chrono>
#include <mutex>
#include <cstddef>
#include <unordered_map>

namespace proxy_cache
{

    constexpr std::size_t MAX_CACHE_BYTES = 100 * 1024 * 1024;

    class Cache
    {
    private:
        struct cache_node
        {
           std::shared_ptr<std::vector<char>> data_ptr;
            std::string url;

            std::shared_ptr<cache_node> next;
            std::weak_ptr<cache_node> prev;

            cache_node(const std::string &url_c,std::shared_ptr<std::vector<char>> &data_c) : data_ptr(std::move(data_c)), url(url_c), next(nullptr), prev() {}
        };
        std::shared_ptr<cache_node> head;
        std::shared_ptr<cache_node> tail;

        std::size_t current_size;

        std::unordered_map<std::string, std::shared_ptr<cache_node>> cache_map;

        mutable std::mutex cache_mutex;

        void detach_node_unlocked(std::shared_ptr<cache_node> &node);
        void remove_lru_node_unlocked(const std::size_t &required_space);

    public:
        Cache();
        ~Cache() = default;

        Cache(const Cache &) = delete;
        Cache &operator=(const Cache &) = delete;

        void cache_add(const std::string &url, const std::vector<char> &data);

        std::vector<char> cache_find(const std::string &url);
    };
}