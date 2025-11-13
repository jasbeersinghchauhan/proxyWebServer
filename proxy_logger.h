/**
 * @file proxy_logger.h
 * @brief A thread-safe, file-based logger for the proxy server.
 */

#ifndef PROXY_LOGGER_H
#define PROXY_LOGGER_H

/**
 * @brief Initializes the logging system.
 * @details Must be called once at startup, before any logging.
 * @param filename The name of the log file (e.g., "proxy.log").
 * @return 1 on success, 0 on failure.
 */
int logger_init(const char* filename);

/**
 * @brief Frees resources used by the logger.
 * @details Must be called once at shutdown.
 */
void logger_destroy();

/**
 * @brief Writes a formatted message to the log file in a thread-safe manner.
 * @details This function behaves like printf.
 * @param format The format string (e.g., "INFO|CLIENT %d|%s").
 * @param ... The variable arguments.
 */
void log_message(const char* format, ...);

#endif // PROXY_LOGGER_H