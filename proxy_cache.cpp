#include <iostream>
#include <string>

#include "proxy_cache.hpp"

using namespace proxy_cache;

Cache::Cache() : head(nullptr), tail(nullptr), current_size(0) {}

void Cache::detach_node_unlocked(std::shared_ptr<Cache::cache_node> &node)
{
    if (!node)
        return;

    std::shared_ptr<Cache::cache_node> prev_node = node->prev.lock();
    std::shared_ptr<Cache::cache_node> next_node = node->next;

    if (prev_node)
        prev_node->next = next_node;
    else
        head = next_node;

    if (next_node)
        next_node->prev = prev_node;
    else
        tail = prev_node;

    node->prev.reset();
    node->next.reset();
}

void Cache::remove_lru_node_unlocked(const std::size_t &required_space)
{
    while (tail && (current_size + required_space > MAX_CACHE_BYTES))
    {

        std::shared_ptr<Cache::cache_node> old_node = tail;

        detach_node_unlocked(old_node);

        current_size -= old_node->data_ptr->size();
        cache_map.erase(old_node->url);
    }
}

void Cache::cache_add(const std::string &url, const std::vector<char> &data)
{

    if (url.empty() || data.empty() || data.size() > MAX_CACHE_BYTES)
    {
        std::cerr << "Invalid URL or Data for caching.\n";
        return;
    }

    std::size_t data_size = data.size();

    std::shared_ptr<std::vector<char>> data_ptr = std::make_shared<std::vector<char>>(data);

    std::lock_guard<std::mutex> lock(cache_mutex);

    auto it = cache_map.find(url);

    if (it != cache_map.end())
    {
        std::shared_ptr<Cache::cache_node> existing_node = it->second;

        detach_node_unlocked(existing_node);

        if (existing_node->data_ptr)
            current_size -= existing_node->data_ptr->size();

        remove_lru_node_unlocked(data_size);

        existing_node->data_ptr = data_ptr;
        existing_node->next = head;
        existing_node->prev.reset();

        if (head)
        {
            head->prev = existing_node;
        }
        else
        {
            tail = existing_node;
        }
        head = existing_node;
        current_size += data_size;
    }
    else
    {
        std::shared_ptr<Cache::cache_node> new_node = std::make_shared<Cache::cache_node>(url, data_ptr);

        remove_lru_node_unlocked(data_size);

        new_node->next = head;
        new_node->prev.reset();

        if (head)
        {
            head->prev = new_node;
        }
        else
        {
            tail = new_node;
        }

        head = new_node;
        cache_map[url] = new_node;
        current_size += data_size;
    }
}

std::vector<char> Cache::cache_find(const std::string &url)
{
    if (url.empty())
        return {};

    std::shared_ptr<std::vector<char>> data_read_ptr;

    {
        std::lock_guard<std::mutex> lock(cache_mutex);

        auto it = cache_map.find(url);
        if (it == cache_map.end())
            return {};

        std::shared_ptr<Cache::cache_node> node = it->second;

        if (node != head)
        {

            detach_node_unlocked(node);

            if (!head)
                tail = node;
            else
                head->prev = node;

            node->next = head;
            node->prev.reset();
            head = node;
        }
        data_read_ptr = node->data_ptr;
    }

    if (data_read_ptr)
        return *data_read_ptr;
    return {};
}