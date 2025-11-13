/**
 * @file proxy_logger.c
 * @brief Implementation of the thread-safe, file-based logger.
 * @details This logger uses a Windows CRITICAL_SECTION to ensure
 * that log messages from different threads do not interleave,
 * making the log file readable and reliable.
 */

// Suppress warnings for using standard C functions (like fopen, localtime)
#define _CRT_SECURE_NO_WARNINGS

#include "proxy_logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h> // For variadic functions (va_list, va_start, va_end)
#include <time.h>
#include <Windows.h> // For CRITICAL_SECTION and time functions

// --- Global (static) variables ---

// File pointer to the log file.
// 'static' limits its scope to this file only.
static FILE *g_logfile = NULL;

// The mutex (lock) to protect file writes.
// This ensures only one thread can write to the file at a time.
static CRITICAL_SECTION g_log_mutex;

/**
 * @brief Initializes the logger.
 * @details Opens the log file and initializes the critical section (mutex).
 * @param filename The path to the log file (e.g., "proxy.log").
 * @return 1 on success, 0 on failure.
 */
int logger_init(const char *filename)
{
    // Open the log file in "append" mode ("a").
    // This creates the file if it doesn't exist and adds to the end
    // if it does.
    g_logfile = fopen(filename, "a");
    if (g_logfile == NULL)
    {
        // Use printf for this error since the logger itself failed to start.
        printf("[FATAL ERROR] Could not open log file: %s\n", filename);
        return 0; // Failure
    }

    // Initialize the mutex that will protect g_logfile.
    InitializeCriticalSection(&g_log_mutex);

    // Write an initial "server start" message to the log
    log_message("SYSTEM|Logger initialized. Server starting.");
    return 1; // Success
}

/**
 * @brief Shuts down the logger.
 * @details Writes a final message, closes the log file,
 * and destroys the critical section (mutex).
 */
void logger_destroy()
{
    log_message("SYSTEM|Server shutting down. Logger closing.");

    if (g_logfile != NULL)
    {
        fclose(g_logfile); // Close the file handle
        g_logfile = NULL;  // Prevent further use
    }

    // Release the resources associated with the mutex.
    DeleteCriticalSection(&g_log_mutex);
}

/**
 * @brief Writes a formatted message to the log file in a thread-safe way.
 * @details This function is variadic, meaning it accepts a variable
 * number of arguments, just like printf.
 * @param format The format string (e.g., "INFO|CLIENT %d|%s").
 * @param ... The variable arguments matching the format string.
 */
void log_message(const char *format, ...)
{
    // If logger failed to initialize or was destroyed, do nothing.
    if (g_logfile == NULL)
        return;

    // --- Get current time ---
    char time_buf[100];
    time_t now = time(NULL);        // Get current calendar time
    struct tm *t = localtime(&now); // Convert to local time structure
    // Format the time as "YYYY-MM-DD HH:MM:SS"
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", t);

    // --- Thread-Safe Writing ---

    // Acquire the lock. Any other thread calling this function
    // will block here until the lock is released.
    EnterCriticalSection(&g_log_mutex);

    // Write the timestamp prefix
    fprintf(g_logfile, "[%s] ", time_buf);

    // --- Handle variadic arguments ---
    va_list args; // Declare a list to hold the ... arguments
    // Initialize 'args' to point to the first arg after 'format'
    va_start(args, format);
    // Write the formatted message using the argument list
    vfprintf(g_logfile, format, args);
    // Clean up the argument list
    va_end(args);

    // Write a newline to complete the log entry
    fprintf(g_logfile, "\n");

    // Flush the file buffer. This ensures the log entry is
    // written to disk immediately, which is useful for debugging crashes.
    fflush(g_logfile);

    // Release the lock, allowing other threads to proceed.
    LeaveCriticalSection(&g_log_mutex);
}