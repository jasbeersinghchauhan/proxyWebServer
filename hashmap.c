// hashmap.c
#define WIN32_LEAN_AND_MEAN
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS

#include "hashmap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//=============================================================================
// 1. Static Helper Functions (Internal Logic)
//=============================================================================

/**
 * @brief Creates a new map node.
 * @return A pointer to the new node, or NULL on failure.
 */
static map_node_t* create_node(void* key, void* value) {
    map_node_t* new_node = (map_node_t*)malloc(sizeof(map_node_t));
    if (new_node == NULL) {
        return NULL;
    }
    new_node->entry.key = key;
    new_node->entry.value = value;
    new_node->next = NULL;
    return new_node;
}

/**
 * @brief Grows the map's capacity and rehashes all existing elements.
 * @details This optimized version re-links existing nodes into new buckets
 * instead of re-allocating them, improving performance.
 */
static void resize_and_rehash(map_t* map) {
    size_t old_capacity = map->capacity;
    map_node_t** old_buckets = map->buckets;

    map->capacity *= 2;
    map->buckets = (map_node_t**)calloc(map->capacity, sizeof(map_node_t*));
    if (map->buckets == NULL) {
        // Allocation failed, restore old state and abort resize.
        map->capacity = old_capacity;
        map->buckets = old_buckets;
        return;
    }

    // Relink every node from the old buckets into the new, larger ones.
    for (size_t i = 0; i < old_capacity; i++) {
        map_node_t* current = old_buckets[i];
        while (current) {
            map_node_t* next = current->next; // Save next node
            unsigned int new_index = map->hash(current->entry.key, map->capacity);

            // Insert the node at the head of the new bucket's list
            current->next = map->buckets[new_index];
            map->buckets[new_index] = current;

            current = next; // Move to the next node in the old chain
        }
    }
    free(old_buckets);
}

//=============================================================================
// 2. Default Implementations (for string keys)
//=============================================================================

/**
 * @brief Default FNV-1a hash function for string keys.
 */
static unsigned int fnv1a_hash(const void* key, size_t map_capacity) {
    const char* str = (const char*)key;
    unsigned int hash = 2166136261u; // FNV offset basis
    while (*str) {
        hash ^= (unsigned char)*str++;
        hash *= 16777619u; // FNV prime
    }
    return hash % map_capacity;
}

/**
 * @brief Default key comparison function for string keys.
 */
static int string_key_compare(const void* key1, const void* key2) {
    return strcmp((const char*)key1, (const char*)key2);
}

//=============================================================================
// 3. Public API Functions
//=============================================================================

map_t* map_create(size_t initial_capacity, float load_factor,
    hash_func_t hash, key_compare_func_t key_compare,
    free_func_t key_free, free_func_t value_free) {
    map_t* map = (map_t*)malloc(sizeof(map_t));
    if (map == NULL) {
        return NULL;
    }

    map->capacity = initial_capacity > 0 ? initial_capacity : 16; // Default capacity
    map->count = 0;
    map->load_factor_threshold = load_factor > 0.0f ? load_factor : 0.75f;
    map->hash = hash ? hash : fnv1a_hash;
    map->key_compare = key_compare ? key_compare : string_key_compare;
    map->key_free = key_free;
    map->value_free = value_free;

    map->buckets = (map_node_t**)calloc(map->capacity, sizeof(map_node_t*));
    if (map->buckets == NULL) {
        free(map);
        return NULL;
    }
    return map;
}

void map_destroy(map_t* map) {
    if (map == NULL) return;

    for (size_t i = 0; i < map->capacity; i++) {
        map_node_t* current = map->buckets[i];
        while (current) {
            map_node_t* temp = current;
            current = current->next;
            if (map->key_free) map->key_free(temp->entry.key);
            if (map->value_free) map->value_free(temp->entry.value);
            free(temp);
        }
    }
    free(map->buckets);
    free(map);
}

void map_insert(map_t* map, void* key, void* value) {
    if (map == NULL || key == NULL) return;

    // Grow the map if the load factor is exceeded
    if ((float)map->count / map->capacity > map->load_factor_threshold) {
        resize_and_rehash(map);
    }

    unsigned int index = map->hash(key, map->capacity);
    map_node_t* current = map->buckets[index];

    // Check if key already exists
    while (current) {
        if (map->key_compare(current->entry.key, key) == 0) {
            // Key found, update value
            if (map->value_free) map->value_free(current->entry.value);
            current->entry.value = value;
            // The new key is a duplicate, so free it
            if (map->key_free) map->key_free(key);
            return;
        }
        current = current->next;
    }

    // Key not found, create a new node
    map_node_t* new_node = create_node(key, value);
    if (new_node == NULL) return; // Allocation failed

    // Insert new node at the head of the list
    new_node->next = map->buckets[index];
    map->buckets[index] = new_node;
    map->count++;
}

void* map_find(const map_t* map, const void* key) {
    if (map == NULL || key == NULL) return NULL;

    unsigned int index = map->hash(key, map->capacity);
    map_node_t* current = map->buckets[index];
    while (current) {
        if (map->key_compare(current->entry.key, key) == 0) {
            return current->entry.value;
        }
        current = current->next;
    }
    return NULL;
}

void map_erase(map_t* map, const void* key) {
    if (map == NULL || key == NULL) return;

    unsigned int index = map->hash(key, map->capacity);
    map_node_t* current = map->buckets[index];
    map_node_t* prev = NULL;

    while (current) {
        if (map->key_compare(current->entry.key, key) == 0) {
            // Unlink the node from the list
            if (prev) {
                prev->next = current->next;
            }
            else {
                map->buckets[index] = current->next;
            }

            // Free resources and update count
            if (map->key_free) map->key_free(current->entry.key);
            if (map->value_free) map->value_free(current->entry.value);
            free(current);
            map->count--;
            return;
        }
        prev = current;
        current = current->next;
    }
}

size_t map_size(const map_t* map) {
    return map ? map->count : 0;
}

int map_is_empty(const map_t* map) {
    return map ? (map->count == 0) : 1;
}