// hashmap.h

#pragma once

#include <stddef.h> // For size_t

// A single key-value entry stored in the map.
typedef struct map_entry {
    void* key;
    void* value;
} map_entry_t;

// Linked list node for handling hash collisions.
typedef struct map_node {
    map_entry_t entry;
    struct map_node* next;
} map_node_t;

// Define function pointers for custom map behavior.
typedef unsigned int (*hash_func_t)(const void* key, size_t map_capacity);
typedef int (*key_compare_func_t)(const void* key1, const void* key2);
typedef void (*free_func_t)(void* data);

// The main map structure.
typedef struct map {
    map_node_t** buckets;             // Array of pointers to map nodes (the hash table).
    size_t capacity;                  // The current capacity of the bucket array.
    size_t count;                     // The number of elements currently in the map.
    float load_factor_threshold;      // Threshold to trigger a resize.
    hash_func_t hash;                 // Hashing function for keys.
    key_compare_func_t key_compare;   // Comparison function for keys.
    free_func_t key_free;             // Optional function to free keys.
    free_func_t value_free;           // Optional function to free values.
} map_t;

/**
 * @brief Creates and initializes a new hash map.
 * @param initial_capacity The initial number of buckets in the hash map. If 0, a default is used.
 * @param load_factor The load factor threshold for resizing. If 0, a default is used.
 * @param hash The hashing function. If NULL, a default for string keys is used.
 * @param key_compare The key comparison function. If NULL, a default for string keys is used.
 * @param key_free The function to free keys. Can be NULL if keys don't need freeing.
 * @param value_free The function to free values. Can be NULL if values don't need freeing.
 * @return A pointer to the newly created map, or NULL on allocation failure.
 */
map_t* map_create(size_t initial_capacity, float load_factor,
    hash_func_t hash, key_compare_func_t key_compare,
    free_func_t key_free, free_func_t value_free);

/**
 * @brief Destroys the map, freeing all allocated memory for nodes, keys, and values.
 * @param map The map to destroy. Does nothing if map is NULL.
 */
void map_destroy(map_t* map);

/**
 * @brief Inserts a key-value pair into the map.
 * @details If the key already exists, the existing value is freed (if value_free is set)
 * and replaced with the new value. The map takes ownership of both the key and
 * value pointers.
 * @note If the key already exists, the *new* key pointer provided is freed (if key_free is set),
 * as the original key is kept.
 * @param map The map to insert into.
 * @param key The key pointer. The map takes ownership.
 * @param value The value pointer. The map takes ownership.
 */
void map_insert(map_t* map, void* key, void* value);

/**
 * @brief Finds an entry by its key and returns the associated value.
 * @param map The map to search in.
 * @param key The key to find.
 * @return A pointer to the value, or NULL if the key is not found. The pointer is
 * owned by the map and should not be freed by the caller.
 */
void* map_find(const map_t* map, const void* key);

/**
 * @brief Removes a key-value pair from the map.
 * @details Frees the key and value using the provided free functions if they are set.
 * @param map The map to remove from.
 * @param key The key of the element to remove.
 */
void map_erase(map_t* map, const void* key);

/**
 * @brief Returns the number of elements in the map.
 * @param map The map.
 * @return The current element count.
 */
size_t map_size(const map_t* map);

/**
 * @brief Checks if the map is empty.
 * @param map The map.
 * @return 1 (true) if the map has no elements, 0 (false) otherwise.
 */
int map_is_empty(const map_t* map);