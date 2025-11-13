/**
 * @file proxy_cache.c
 * @brief A thread-safe, high-performance LRU cache implementation.
 *
 * This file implements a Least Recently Used (LRU) cache using a hash map
 * for O(1) lookups and a doubly-linked list to maintain the usage order.
 * It is designed for use in a multithreaded proxy server.
 *
 * The logic is as follows:
 * 1. A hash map (`map_t`) stores URL (string) -> `cache_element*`. This
 * provides O(1) average time complexity for finding elements.
 * 2. A doubly-linked list maintains the "age" of elements.
 * - The `head` of the list is the Most Recently Used (MRU) element.
 * - The `tail` of the list is the Least Recently Used (LRU) element.
 * 3. When an element is accessed (`cache_find`):
 * - It's found in the hash map.
 * - It's moved from its current position in the list to the `head`.
 * 4. When a new element is added (`cache_add`):
 * - It's added to the `head` of the list and to the hash map.
 * - If the cache is full, the element at the `tail` (the LRU) is
 * removed from both the list and the map to make space.
 * 5. All public functions (`cache_find`, `cache_add`, etc.) are made
 * thread-safe by wrapping their logic in a mutex (CRITICAL_SECTION
 * on Windows, pthread_mutex_t on POSIX).
 */
#define WIN32_LEAN_AND_MEAN				// Exclude rarely-used stuff from Windows headers
#define _WINSOCK_DEPRECATED_NO_WARNINGS // Suppress Winsock warnings
#define _CRT_SECURE_NO_WARNINGS			// Suppress warnings for strcpy, fopen, etc.

#include "proxy_cache.h" // Public header for this cache
#include "hashmap.h"	 // The hash map implementation

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h> // For defensive programming assertions

// Platform-specific mutex includes
#ifdef _WIN32
#include <Windows.h> // For CRITICAL_SECTION on Windows
#else
#include <pthread.h> // For pthread_mutex_t on POSIX (Linux, macOS)
#endif

/*=============================================================================
 * 1. Type Definitions & Static Globals
 *===========================================================================*/

/**
 * @brief Internal state of the cache system.
 * @details This struct holds all components needed to manage the cache.
 */
typedef struct
{
	map_t *map;			 // Maps URL -> cache_element* for O(1) lookups.
	cache_element *head; // Head of the list (Most Recently Used).
	cache_element *tail; // Tail of the list (Least Recently Used).

	size_t current_size; // Current total size of all *data* in cache (bytes).

#ifdef _WIN32
	CRITICAL_SECTION mutex; // Mutex for Windows
#else
	pthread_mutex_t mutex; // Mutex for POSIX
#endif
} proxy_cache_t;

/**
 * @brief The single, global instance of the cache.
 * @details 'static' limits its visibility to this file.
 */
static proxy_cache_t g_cache;

/*=============================================================================
 * 2. Static Helper Functions (Internal Logic)
 *===========================================================================*/

/**
 * @brief Detaches a node from the doubly-linked list.
 * @details This function is NOT thread-safe and must be called from
 * within a locked critical section. It safely unlinks an element
 * from the list, handling edge cases for the head and tail.
 * @param element The element to detach.
 */
static void detach_node_unlocked(cache_element *element)
{
	if (!element)
		return;

	// Link the previous node to the next node
	if (element->prev)
		element->prev->next = element->next;
	else
		// This element was the head, so make the next element the new head
		g_cache.head = element->next;

	// Link the next node to the previous node
	if (element->next)
		element->next->prev = element->prev;
	else
		// This element was the tail, so make the previous element the new tail
		g_cache.tail = element->prev;
}

/**
 * @brief Attaches a node to the front (head) of the list.
 * @details This marks the element as the Most Recently Used (MRU).
 * This function is NOT thread-safe and must be called from
 * within a locked critical section.
 * @param element The element to attach.
 */
static void attach_node_to_head_unlocked(cache_element *element)
{
	if (!element)
		return;

	// Point this element's 'next' to the current head
	element->next = g_cache.head;
	// This element has no predecessor
	element->prev = NULL;

	// If there was a head, update its 'prev' to point to this new element
	if (g_cache.head)
	{
		g_cache.head->prev = element;
	}

	// This element is now the head
	g_cache.head = element;

	// If the list was empty, this element is also the tail
	if (!g_cache.tail)
	{
		g_cache.tail = element;
	}
}

/**
 * @brief Evicts the least-recently-used element (the tail) from the cache.
 * @details This function is NOT thread-safe and must be called from
 * within a locked critical section.
 */
static void remove_lru_element_unlocked()
{
	// Get the element at the tail of the list
	cache_element *lru_element = g_cache.tail;
	if (!lru_element)
		return; // Cache is empty, nothing to evict

	// 1. Unlink from the list
	detach_node_unlocked(lru_element);

	// 2. Update the cache's total size
	g_cache.current_size -= lru_element->len;

	// 3. Erase from the hash map.
	// This will trigger the callbacks:
	// - `free` (passed as key_free) will be called on `lru_element->url`.
	// - `free_cache_element` (passed as value_free) will be called on `lru_element`.
	map_erase(g_cache.map, lru_element->url);
}

/**
 * @brief Custom free function for the hash map to completely destroy a cache element.
 * @details This is passed to `map_create` as the `value_free` callback.
 * When `map_erase` is called, the map frees the key (using `free`) and then
 * calls this function on the value (the `cache_element*`).
 */
static void free_cache_element(void *data)
{
	if (!data)
		return;
	cache_element *element = (cache_element *)data;

	// Free the actual cached content (the response body)
	free(element->data);

	// The map frees the key (element->url), but we must free the element struct
	free(element);
}

/*=============================================================================
 * 3. Public API Functions
 *===========================================================================*/

/**
 * @brief Initializes the global cache instance.
 */
void cache_init()
{
	// Initialize all members to a known zero-state
	g_cache.head = NULL;
	g_cache.tail = NULL;
	g_cache.current_size = 0;

	// Initialize the platform-specific mutex
#ifdef _WIN32
	InitializeCriticalSection(&g_cache.mutex);
#else
	pthread_mutex_init(&g_cache.mutex, NULL);
#endif

	// Create the hash map.
	// - 1024: initial capacity
	// - 0.75f: load factor (resize when 75% full)
	// - NULL, NULL: use default string hash and compare functions
	// - free: function to free the key (the URL string, which we strdup)
	// - free_cache_element: function to free the value (the cache_element)
	g_cache.map = map_create(1024, 0.75f, NULL, NULL, free, free_cache_element);

	if (g_cache.map == NULL)
	{
		// This is a fatal, unrecoverable error.
		fprintf(stderr, "Fatal: Failed to initialize proxy cache map.\n\n");
		exit(EXIT_FAILURE);
	}
}

/**
 * @brief Destroys the global cache, freeing all allocated memory.
 */
void cache_destroy()
{
	// Lock the cache for final cleanup
#ifdef _WIN32
	EnterCriticalSection(&g_cache.mutex);
#else
	pthread_mutex_lock(&g_cache.mutex);
#endif

	// Destroying the map will iterate over every entry and call
	// `free(key)` and `free_cache_element(value)` on each one.
	// This cleans up all URLs, all data buffers, and all cache_element structs.
	map_destroy(g_cache.map);

	// Reset cache state to prevent use-after-free
	g_cache.head = NULL;
	g_cache.tail = NULL;
	g_cache.current_size = 0;
	g_cache.map = NULL;

	// Unlock and then delete the synchronization object
#ifdef _WIN32
	LeaveCriticalSection(&g_cache.mutex);
	DeleteCriticalSection(&g_cache.mutex);
#else
	pthread_mutex_unlock(&g_cache.mutex);
	pthread_mutex_destroy(&g_cache.mutex);
#endif
}

/**
 * @brief Finds an element in the cache in a thread-safe way.
 * @details If found, the element is marked as Most Recently Used (MRU).
 * @param url The URL key to search for.
 * @return A pointer to the cache_element if found (read-only), or NULL if not.
 */
cache_element *cache_find(const char *url)
{
	if (!url)
		return NULL;

	// Acquire lock for safe cache access
#ifdef _WIN32
	EnterCriticalSection(&g_cache.mutex);
#else
	pthread_mutex_lock(&g_cache.mutex);
#endif

	// 1. Find in map (O(1) average)
	cache_element *element = (cache_element *)map_find(g_cache.map, url);

	if (element)
	{
		// 2. CACHE HIT! Move it to the front of the list (mark as MRU).
		detach_node_unlocked(element);
		attach_node_to_head_unlocked(element);
	}
	// 3. (CACHE MISS) element is NULL, just fall through.

	// 4. Release lock and return
#ifdef _WIN32
	LeaveCriticalSection(&g_cache.mutex);
#else
	pthread_mutex_unlock(&g_cache.mutex);
#endif
	return element; // Returns the element or NULL
}

/**
 * @brief Adds a new element to the cache or updates an existing one.
 * @details This function is thread-safe. It will evict LRU items if
 * the new data exceeds cache capacity.
 * @param url The URL key for this data.
 * @param data A pointer to the data buffer to cache.
 * @param length The size of the data buffer in bytes.
 */
void cache_add(const char *url, const char *data, size_t length)
{
	// Pre-condition checks (fail fast).
	// Do not cache null data, empty data, or data that is too large.
	if (url == NULL || data == NULL || length == 0 || length > MAX_CACHE_SIZE)
	{
		return;
	}

	// Acquire lock to modify the shared cache structure.
#ifdef _WIN32
	EnterCriticalSection(&g_cache.mutex);
#else
	pthread_mutex_lock(&g_cache.mutex);
#endif

	// Check if this item already exists in the cache
	cache_element *existing_element = (cache_element *)map_find(g_cache.map, url);

	// CASE 1: The item already exists. We need to UPDATE it.
	if (existing_element)
	{
		// Step 1: Detach from list and subtract its old size.
		detach_node_unlocked(existing_element);
		g_cache.current_size -= existing_element->len;

		// Step 2: Evict *other* elements if the *new* data won't fit.
		while (g_cache.current_size + length > MAX_CACHE_SIZE)
		{
			remove_lru_element_unlocked();
		}

		// Step 3: Free the old data buffer and allocate a new one.
		free(existing_element->data);
		existing_element->data = malloc(length);
		if (existing_element->data == NULL)
		{
			// Malloc failure! This is bad.
			// Remove the now-corrupt element from the map entirely.
			// map_erase will call free_cache_element, which will free the struct.
			map_erase(g_cache.map, existing_element->url);
			// --- Unlock Mutex ---
#ifdef _WIN32
			LeaveCriticalSection(&g_cache.mutex);
#else
			pthread_mutex_unlock(&g_cache.mutex);
#endif
			return;
		}
		// Copy the new data into the new buffer
		memcpy(existing_element->data, data, length);
		existing_element->len = length; // Update the length

		// Step 4: Add the updated size back and attach the node to the head (MRU).
		g_cache.current_size += length;
		attach_node_to_head_unlocked(existing_element);
	}
	// CASE 2: The item is new. We need to INSERT it.
	else
	{
		// Step 1: Evict old elements until there is enough space for the new one.
		while (g_cache.current_size + length > MAX_CACHE_SIZE)
		{
			remove_lru_element_unlocked();
		}

		// Allocate memory for the new cache element struct
		cache_element *new_element = calloc(1, sizeof(cache_element));
		if (new_element == NULL)
		{
			// --- Unlock Mutex ---
#ifdef _WIN32
			LeaveCriticalSection(&g_cache.mutex);
#else
			pthread_mutex_unlock(&g_cache.mutex);
#endif
			return; // Alloc failed
		}

		// Allocate and copy the URL string (for the map key)
		new_element->url = strdup(url); // `strdup` = malloc + strcpy
		// Allocate and copy the data buffer (for the cached content)
		new_element->data = malloc(length);

		if (!new_element->url || !new_element->data)
		{
			// Allocation failed, clean up everything we just allocated.
			free(new_element->url);
			free(new_element->data);
			free(new_element);

#ifdef _WIN32
			LeaveCriticalSection(&g_cache.mutex);
#else
			pthread_mutex_unlock(&g_cache.mutex);
#endif
			return;
		}

		memcpy(new_element->data, data, length); // Copy data
		new_element->len = length;				 // Set length

		// 2. Add the new element to the map and the front of the list.
		attach_node_to_head_unlocked(new_element);				// Becomes MRU
		map_insert(g_cache.map, new_element->url, new_element); // Add to lookup map
		g_cache.current_size += length;							// Update total cache size
	}

	// --- Unlock Mutex ---
#ifdef _WIN32
	LeaveCriticalSection(&g_cache.mutex);
#else
	pthread_mutex_unlock(&g_cache.mutex);
#endif
}