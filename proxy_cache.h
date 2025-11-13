#ifndef PROXY_CACHE_H
#define PROXY_CACHE_H

#include <stddef.h>

/*=============================================================================
 * 1. Constants
 *===========================================================================*/

// 10 MiB: The total maximum size of all objects in the cache.
#define MAX_CACHE_SIZE 10485760 

/*=============================================================================
 * 2. Public Data Structures
 *===========================================================================*/

/**
 * @brief Represents a single element in the cache.
 */
typedef struct cache_element {
    char* url;
    char* data;
    size_t len;
    struct cache_element* next;
    struct cache_element* prev;
} cache_element;

/*=============================================================================
 * 3. Public API Functions
 *===========================================================================*/

/**
 * @brief Initializes the cache system. Must be called once at startup.
 */
void cache_init();

/**
 * @brief Frees all memory used by the cache. Must be called once at shutdown.
 */
void cache_destroy();

/**
 * @brief Finds an element in the cache by its URL.
 */
cache_element* cache_find(const char* url);

/**
 * @brief Adds a new data object to the cache.
 */
void cache_add(const char* url, const char* data, size_t length);

#endif // PROXY_CACHE_H