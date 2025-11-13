/**
 * @file proxy_handler.c
 * @brief Handles all logic for a single client connection.
 * @details This is the final version with all printf calls
 * replaced by thread-safe log_message calls.
 */

// Define Windows version target for advanced features (e.g., getaddrinfo)
#define _WIN32_WINNT 0x0601
// Exclude rarely-used stuff from Windows headers
#define WIN32_LEAN_AND_MEAN
// Suppress warnings for using older Winsock functions
#define _WINSOCK_DEPRECATED_NO_WARNINGS
// Suppress warnings for using standard C functions like sprintf, strcpy
#define _CRT_SECURE_NO_WARNINGS

// Standard C libraries
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Winsock libraries for networking
#include <WinSock2.h>
#include <Ws2tcpip.h>
// Windows API for threading and semaphores
#include <Windows.h>

// Project-specific headers
#include "proxy_handler.h" // Header for this file (e.g., function prototypes)
#include "proxy_cache.h"   // Header for the caching system
#include "proxy_logger.h"  // Include the new logger

// Define a common buffer size for read/write operations
#define MAX_BYTES 4096

// External reference to the global semaphore defined in another file (e.g., main.c)
// This semaphore is used to limit the total number of concurrent client threads.
extern HANDLE semaphore;

/**
 * @brief Holds the constituent parts of a parsed HTTP URL.
 * @details This struct simplifies handling different parts of a URL,
 * especially for connecting to the remote server and caching.
 */
typedef struct
{
    char host[256];  // e.g., "www.example.com"
    int port;        // e.g., 80
    char path[2048]; // e.g., "/page.html"
} http_request_parts;

/**
 * @brief Sends a pre-formatted error response to the client.
 * @param client_socket The socket descriptor for the connected client.
 * @param status_code The HTTP status code (e.g., 400, 502).
 * @param status_message The short message for the status code (e.g., "Bad Request").
 * @details This is a helper function to ensure clients receive a valid HTTP
 * response when an error occurs within the proxy.
 */
static void send_error_response(SOCKET client_socket, int status_code, const char *status_message)
{
    char response[512];  // Buffer for the full HTTP response
    char html_body[128]; // Buffer for the simple HTML body

    // 1. Create the simple HTML body
    // snprintf is used for safe string formatting, preventing buffer overflows.
    snprintf(html_body, sizeof(html_body),
             "<HTML><HEAD><TITLE>%d %s</TITLE></HEAD><BODY><H1>%d %s</H1></BODY></HTML>",
             status_code, status_message, status_code, status_message);

    // 2. Create the full HTTP response headers and body
    snprintf(response, sizeof(response),
             "HTTP/1.1 %d %s\r\n"    // Status line
             "Connection: close\r\n" // Tell client we will close the connection
             "Content-Type: text/html\r\n"
             "Content-Length: %d\r\n" // Length of the HTML body
             "\r\n"                   // End of headers
             "%s",                    // The HTML body itself
             status_code, status_message,
             (int)strlen(html_body),
             html_body);

    // 3. Send the complete response to the client
    send(client_socket, response, (int)strlen(response), 0);
}

/**
 * @brief Parses a full URL (e.g., "http://www.host.com:80/path") into its parts.
 * @param url The full, absolute URL string to parse.
 * @param parts A pointer to an http_request_parts struct to be filled.
 * @return 1 on success, 0 on failure (e.g., parse error, buffer too small).
 * @details This function uses string manipulation (strstr, strchr) to find
 * the host, port, and path. It modifies a temporary copy of the URL
 * string to isolate each part.
 */
static int parse_full_url(const char *url, http_request_parts *parts)
{
    char temp_url[4096]; // Buffer for string manipulation
    char *host_start;    // Pointer to the start of the host
    char *port_start;    // Pointer to the ':' before the port
    char *path_start;    // Pointer to the '/' starting the path

    // Make a local copy because we will modify it with null terminators
    strncpy(temp_url, url, sizeof(temp_url) - 1);
    temp_url[sizeof(temp_url) - 1] = '\0'; // Ensure null termination

    // 1. Find start of host (skip "http://")
    host_start = strstr(temp_url, "://");
    if (host_start)
        host_start += 3; // Move pointer past "://"
    else
        host_start = temp_url; // Assume URL starts with host (e.g., in CONNECT)

    // 2. Find start of path
    path_start = strchr(host_start, '/');
    if (path_start)
    {
        // Copy path, ensuring null termination
        strncpy(parts->path, path_start, sizeof(parts->path) - 1);
        parts->path[sizeof(parts->path) - 1] = '\0';

        // Place a null terminator to separate host/port from path
        *path_start = '\0'; // Cut the string here, so host_start is now just host:port
    }
    else
    {
        // No path found, default to "/"
        strcpy(parts->path, "/");
    }

    // 3. Find port (if any)
    parts->port = 80; // Default HTTP port
    port_start = strchr(host_start, ':');
    if (port_start)
    {
        // Place a null terminator to separate host from port
        *port_start = '\0';                 // Cut the string, so host_start is now just host
        parts->port = atoi(port_start + 1); // Convert port string to integer
    }

    // 4. Whatever is left in host_start is the host
    if (strlen(host_start) >= sizeof(parts->host))
    {
        return 0; // Host buffer is too small, parse failed
    }
    strcpy(parts->host, host_start);
    return 1; // Success
}

/**
 * @brief Resolves a hostname and connects a new socket to it.
 * @param host The hostname to resolve (e.g., "www.example.com").
 * @param port The port number to connect to.
 * @return A new, connected SOCKET, or INVALID_SOCKET on failure.
 * @details Uses getaddrinfo() for modern, protocol-independent (IPv4/IPv6)
 * hostname resolution. Also sets send and receive timeouts on the new socket.
 */
static SOCKET connect_to_remote(const char *host, int port)
{
    SOCKET remote_socket = INVALID_SOCKET;
    struct addrinfo hints = {0}, *result = NULL;
    char port_str[16];

    // Set up hints for getaddrinfo
    hints.ai_family = AF_INET;       // Force IPv4. Use AF_UNSPEC for IPv4/IPv6
    hints.ai_socktype = SOCK_STREAM; // TCP socket
    hints.ai_protocol = IPPROTO_TCP;

    // Convert port number to string
    sprintf(port_str, "%d", port);

    // Resolve the hostname
    if (getaddrinfo(host, port_str, &hints, &result) != 0)
    {
        return INVALID_SOCKET; // Host resolution failed
    }

    // Create a socket using the resolved address info
    remote_socket = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (remote_socket == INVALID_SOCKET)
    {
        freeaddrinfo(result); // Clean up address info list
        return INVALID_SOCKET;
    }

    // Set 30-second timeouts for the remote socket
    // This prevents the proxy thread from hanging if the remote server is slow
    DWORD timeout = 30000; // 30,000 milliseconds
    setsockopt(remote_socket, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
    setsockopt(remote_socket, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));

    // Attempt to connect to the remote server
    if (connect(remote_socket, result->ai_addr, (int)result->ai_addrlen) != 0)
    {
        closesocket(remote_socket); // Connection failed
        freeaddrinfo(result);
        return INVALID_SOCKET;
    }

    // Success!
    freeaddrinfo(result); // Free the address info list
    return remote_socket;
}

/**
 * @brief Extracts the URL from the first line of an HTTP request.
 * @param request The full HTTP request string.
 * @return A new, heap-allocated string containing the URL, or NULL on failure.
 * @details Finds the first and second spaces in the request
 * (e.g., "GET [URL] HTTP/1.1") and extracts the [URL] part.
 * The caller is responsible for freeing the returned string.
 */
static char *get_url_from_request(const char *request)
{
    // Find the end of the method (e.g., "GET ")
    const char *method_end = strchr(request, ' ');
    if (method_end == NULL)
        return NULL;

    // The URL starts right after the method and space
    const char *url_start = method_end + 1;

    // Find the end of the URL (the space before "HTTP/1.1")
    const char *url_end = strchr(url_start, ' ');
    if (url_end == NULL)
        return NULL;

    // Calculate length and allocate memory
    size_t url_length = url_end - url_start;
    char *url = (char *)malloc(url_length + 1); // +1 for null terminator
    if (url == NULL)
        return NULL; // Malloc failed

    // Copy the URL string
    strncpy(url, url_start, url_length);
    url[url_length] = '\0'; // Manually add null terminator
    return url;
}

/**
 * @brief Main function for handling a single client connection.
 * @details This function is executed in a new thread for each client.
 * It reads the client request, determines if it's HTTP or HTTPS (CONNECT),
 * handles caching for HTTP, and tunnels data for HTTPS.
 * @param arg A pointer to a heap-allocated SOCKET descriptor for the client.
 * @return 0 on completion.
 */
DWORD WINAPI handle_client(LPVOID arg)
{
    // Retrieve socket descriptor from argument and cast it
    SOCKET client_socket = *(SOCKET *)arg;
    // Free the heap-allocated pointer passed from main.c
    // The socket descriptor itself is just a value, not a pointer
    free(arg);

    // Set 30-second timeouts for all client operations
    // This prevents a slow client from tying up a thread indefinitely
    DWORD timeout_ms = 30000;
    setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms, sizeof(timeout_ms));
    setsockopt(client_socket, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout_ms, sizeof(timeout_ms));

    // Use the socket descriptor as a unique Client ID for logging
    int client_id = (int)client_socket;
    int bytes_recv; // Number of bytes received in a single recv() call
    int len = 0;    // Total length of data read into the buffer

    // Allocate a buffer for reading the client's request
    // calloc initializes the memory to zero
    char *buffer = (char *)calloc(MAX_BYTES, sizeof(char));
    if (buffer == NULL)
    {
        log_message("ERROR|CLIENT %d|Failed to allocate memory for client buffer.", client_id);
        closesocket(client_socket);
        ReleaseSemaphore(semaphore, 1, NULL); // Release slot in semaphore
        return 0;
    }

    // Read the first packet from the client
    ZeroMemory(buffer, MAX_BYTES);                              // Ensure buffer is clear
    bytes_recv = recv(client_socket, buffer, MAX_BYTES - 1, 0); // -1 to leave room for '\0'
    if (bytes_recv <= 0)
    {
        // Client disconnected or timed out before sending any data
        log_message("INFO|CLIENT %d|Client disconnected immediately or timed out.", client_id);
        free(buffer);
        closesocket(client_socket);
        ReleaseSemaphore(semaphore, 1, NULL); // Release slot
        return 0;
    }
    buffer[bytes_recv] = '\0'; // Null-terminate the received data
    len = bytes_recv;          // Store the total length

    // --- HTTPS CONNECT Section ---
    // Check if the request starts with "CONNECT "
    if (strncmp(buffer, "CONNECT ", 8) == 0)
    {
        log_message("INFO|CLIENT %d|CONNECT|HTTPS tunnel request.", client_id);
        char remote_host[256];
        int remote_port = 443; // Default to 443 for HTTPS
        int parsed = 0;        // Flag to track if parsing was successful

        // The "URL" in a CONNECT request is just "host:port"
        char *host_port_str = get_url_from_request(buffer);

        if (host_port_str == NULL)
        {
            log_message("ERROR|CLIENT %d|CONNECT|Could not parse request.", client_id);
            send_error_response(client_socket, 400, "Bad Request");
        }
        else
        {
            // Find the colon separating host and port
            char *colon = strchr(host_port_str, ':');
            if (colon != NULL)
            {
                // Port is specified
                *colon = '\0';                 // Null-terminate host string
                remote_port = atoi(colon + 1); // Parse port
                strncpy(remote_host, host_port_str, sizeof(remote_host) - 1);
                remote_host[sizeof(remote_host) - 1] = '\0';
            }
            else
            {
                // No port specified, use default (443)
                strncpy(remote_host, host_port_str, sizeof(remote_host) - 1);
                remote_host[sizeof(remote_host) - 1] = '\0';
            }
            free(host_port_str); // Free the string from get_url_from_request
            parsed = 1;          // Mark as successfully parsed
        }

        if (parsed > 0)
        {
            log_message("INFO|CLIENT %d|CONNECT|Tunneling to %s:%d", client_id, remote_host, remote_port);
            // Connect to the remote server (e.g., google.com:443)
            SOCKET remote_socket = connect_to_remote(remote_host, remote_port);

            if (remote_socket == INVALID_SOCKET)
            {
                log_message("ERROR|CLIENT %d|CONNECT|Failed to connect to %s", client_id, remote_host);
                send_error_response(client_socket, 502, "Bad Gateway");
            }
            else
            {
                // Connection successful!
                log_message("INFO|CLIENT %d|CONNECT|Tunnel established to %s:%d", client_id, remote_host, remote_port);

                // 1. Send "200 OK" back to the client to signal the tunnel is ready
                const char *ok_response = "HTTP/1.1 200 OK\r\n\r\n";
                send(client_socket, ok_response, (int)strlen(ok_response), 0);

                // 2. Enter the data-tunneling loop
                fd_set read_fds;
                char tunnel_buffer[MAX_BYTES];
                int n_bytes;
                struct timeval select_timeout;

                // Loop indefinitely, relaying data
                while (TRUE)
                {
                    // Set a 120-second timeout for select()
                    // If no data is received from either side for 2 minutes,
                    // the loop will break and the tunnel will close.
                    select_timeout.tv_sec = 120;
                    select_timeout.tv_usec = 0;

                    // Zero the set and add both sockets
                    FD_ZERO(&read_fds);
                    FD_SET(client_socket, &read_fds);
                    FD_SET(remote_socket, &read_fds);

                    // Find the highest socket descriptor (needed for select())
                    SOCKET max_sd = (client_socket > remote_socket) ? client_socket : remote_socket;

                    // Wait for data to be available on EITHER socket
                    int activity = select(max_sd + 1, &read_fds, NULL, NULL, &select_timeout);

                    if (activity == SOCKET_ERROR)
                    {
                        log_message("ERROR|CLIENT %d|CONNECT|select() failed: %d", client_id, WSAGetLastError());
                        break; // Exit loop on error
                    }
                    if (activity == 0)
                    {
                        log_message("INFO|CLIENT %d|CONNECT|Tunnel timed out for %s:%d", client_id, remote_host, remote_port);
                        break; // Exit loop on timeout
                    }

                    // Check if client sent data
                    if (FD_ISSET(client_socket, &read_fds))
                    {
                        n_bytes = recv(client_socket, tunnel_buffer, MAX_BYTES, 0);
                        if (n_bytes <= 0) // Client disconnected or error
                            break;
                        // Forward data to remote server
                        if (send(remote_socket, tunnel_buffer, n_bytes, 0) == SOCKET_ERROR)
                            break;
                    }

                    // Check if remote server sent data
                    if (FD_ISSET(remote_socket, &read_fds))
                    {
                        n_bytes = recv(remote_socket, tunnel_buffer, MAX_BYTES, 0);
                        if (n_bytes <= 0) // Remote disconnected or error
                            break;
                        // Forward data to client
                        if (send(client_socket, tunnel_buffer, n_bytes, 0) == SOCKET_ERROR)
                            break;
                    }
                }
                // Loop has exited, close the remote socket
                closesocket(remote_socket);
            }
        }
    }
    else // --- START HTTP SECTION ---
    {
        log_message("INFO|CLIENT %d|HTTP|Request received.", client_id);

        // Keep reading from client until we have the full headers ("\r\n\r\n")
        // This is necessary because the initial recv() might only get part of the request
        while (!strstr(buffer, "\r\n\r\n"))
        {
            if (len >= MAX_BYTES - 1)
            {
                // Buffer is full but we still don't have the end of headers
                log_message("WARN|CLIENT %d|HTTP|Header too large.", client_id);
                break;
            }
            // Read more data, appending it to the existing data in 'buffer'
            bytes_recv = recv(client_socket, buffer + len, MAX_BYTES - len - 1, 0);
            if (bytes_recv <= 0) // Client disconnected
                break;
            len += bytes_recv;
            buffer[len] = '\0'; // Re-apply null terminator
        }

        // Check if the loop broke because of disconnection before headers were complete
        if (bytes_recv <= 0 && !strstr(buffer, "\r\n\r\n"))
        {
            log_message("INFO|CLIENT %d|HTTP|Client disconnected before headers complete.", client_id);
            free(buffer);
            closesocket(client_socket);
            ReleaseSemaphore(semaphore, 1, NULL);
            return 0;
        }

        // Now we have the full request headers in 'buffer'
        char *url = get_url_from_request(buffer);
        if (url == NULL)
        {
            log_message("ERROR|CLIENT %d|HTTP|Failed to parse URL from request.", client_id);
            send_error_response(client_socket, 400, "Bad Request");
        }
        else
        {
            // --- CACHE ---
            // Try to find this URL in the cache
            cache_element *cached_response = cache_find(url);

            if (cached_response != NULL)
            {
                // --- CACHE HIT ---
                log_message("INFO|CLIENT %d|CACHE_HIT|%s", client_id, url);
                // Send the cached data directly to the client
                send(client_socket, cached_response->data, (int)cached_response->len, 0);
            }
            else
            {
                // --- CACHE MISS ---
                log_message("INFO|CLIENT %d|CACHE_MISS|%s", client_id, url);

                http_request_parts parts; // Struct to hold parsed URL

                if (!parse_full_url(url, &parts))
                {
                    log_message("ERROR|CLIENT %d|HTTP|Could not parse URL: %s", client_id, url);
                    send_error_response(client_socket, 400, "Bad Request");
                }
                else
                {
                    log_message("INFO|CLIENT %d|REMOTE|Connecting to %s:%d", client_id, parts.host, parts.port);
                    // Connect to the remote server
                    SOCKET remote_socket = connect_to_remote(parts.host, parts.port);

                    if (remote_socket == INVALID_SOCKET)
                    {
                        log_message("ERROR|CLIENT %d|REMOTE|Failed to connect to %s (Error: %d)", client_id, parts.host, WSAGetLastError());
                        send_error_response(client_socket, 502, "Bad Gateway");
                    }
                    else
                    {
                        log_message("INFO|CLIENT %d|REMOTE|Connected to %s:%d", client_id, parts.host, parts.port);

                        // --- Modify and forward request ---
                        // We need to build a new request.
                        // Client request: "GET http://www.example.com/path HTTP/1.1"
                        // Remote request: "GET /path HTTP/1.1"

                        // Find the start of the headers (after the first \r\n)
                        char *headers_start = strstr(buffer, "\r\n");
                        if (headers_start)
                            headers_start += 2; // Move past \r\n
                        else
                            headers_start = ""; // No headers? (unlikely)

                        // Extract the method (GET, POST, etc.)
                        char method[20];
                        const char *method_end = strchr(buffer, ' ');
                        size_t method_len = method_end - buffer;
                        strncpy(method, buffer, method_len);
                        method[method_len] = '\0';

                        // --- Build the new request string ---
                        char new_request[MAX_BYTES];
                        char *current_pos = new_request; // Pointer to write location
                        int remaining_len = MAX_BYTES;   // Remaining space
                        int len_written;                 // Bytes written by snprintf

                        // 1. Write the new request line (e.g., "GET /path HTTP/1.1")
                        len_written = snprintf(current_pos, remaining_len, "%s %s HTTP/1.1\r\n", method, parts.path);
                        current_pos += len_written;
                        remaining_len -= len_written;

                        // 2. Write the mandatory "Host:" header
                        len_written = snprintf(current_pos, remaining_len, "Host: %s\r\n", parts.host);
                        current_pos += len_written;
                        remaining_len -= len_written;

                        // 3. Write "Connection: close" - we don't support keep-alive
                        len_written = snprintf(current_pos, remaining_len, "Connection: close\r\n");
                        current_pos += len_written;
                        remaining_len -= len_written;

                        // 4. Copy all other headers from the original request
                        const char *line_start = headers_start;
                        const char *line_end;

                        // Loop through each header line
                        while ((line_end = strstr(line_start, "\r\n")) != NULL && remaining_len > 0)
                        {
                            size_t line_len = line_end - line_start;
                            if (line_len > 0)
                            {
                                // We've already written Host and Connection, so skip them
                                if (_strnicmp(line_start, "Host:", 5) != 0 && _strnicmp(line_start, "Connection:", 11) != 0)
                                {
                                    // Copy the line
                                    len_written = snprintf(current_pos, remaining_len, "%.*s\r\n", (int)line_len, line_start);
                                    current_pos += len_written;
                                    remaining_len -= len_written;
                                }
                            }
                            else
                            {
                                break; // Empty line, marks end of headers
                            }
                            line_start = line_end + 2; // Move to next line
                        }

                        // 5. Add the final "\r\n" to end the headers
                        if (remaining_len > 2)
                        {
                            snprintf(current_pos, remaining_len, "\r\n");
                        }
                        // --- End of new request build ---

                        log_message("INFO|CLIENT %d|REMOTE|Forwarding: %s %s", client_id, method, parts.path);
                        // Send the modified request to the remote server
                        if (send(remote_socket, new_request, (int)strlen(new_request), 0) == SOCKET_ERROR)
                        {
                            log_message("ERROR|CLIENT %d|REMOTE|send() failed: %d", client_id, WSAGetLastError());
                        }
                        else
                        {
                            // --- Read response and forward to client ---
                            log_message("INFO|CLIENT %d|REMOTE|Awaiting response from %s:%d", client_id, parts.host, parts.port);

                            // --- Dynamic cache buffer setup ---
                            size_t cache_buffer_capacity = MAX_BYTES * 2; // Start with 8KB
                            char *cache_buffer = (char *)malloc(cache_buffer_capacity);
                            char response_buffer[MAX_BYTES]; // Temp buffer for recv()
                            size_t total_bytes_received = 0;
                            // Only cache GET requests
                            int cache_this_response = (strcmp(method, "GET") == 0);

                            if (cache_buffer == NULL)
                            {
                                log_message("WARN|CLIENT %d|CACHE|Failed to alloc initial cache buffer. Not caching.", client_id);
                                cache_this_response = 0;
                            }
                            else if (cache_this_response == 0)
                            {
                                log_message("INFO|CLIENT %d|CACHE|Not caching method: %s", client_id, method);
                            }

                            // Loop: read from remote, send to client, and store in cache buffer
                            while ((bytes_recv = recv(remote_socket, response_buffer, MAX_BYTES, 0)) > 0)
                            {
                                // 1. Forward to client
                                if (send(client_socket, response_buffer, bytes_recv, 0) == SOCKET_ERROR)
                                {
                                    log_message("ERROR|CLIENT %d|CLIENT|send() to client failed: %d", client_id, WSAGetLastError());
                                    break; // Client disconnected
                                }

                                // 2. Store in cache buffer (if caching is enabled)
                                if (cache_this_response)
                                {
                                    if (total_bytes_received + bytes_recv > MAX_CACHE_SIZE)
                                    {
                                        // Response is too big for our max cache element size
                                        log_message("WARN|CLIENT %d|CACHE|Response too large to cache.", client_id);
                                        cache_this_response = 0; // Stop caching
                                        free(cache_buffer);      // Free the memory
                                        cache_buffer = NULL;
                                    }
                                    else if (total_bytes_received + bytes_recv > cache_buffer_capacity)
                                    {
                                        // Not enough space, need to realloc
                                        size_t new_capacity = cache_buffer_capacity * 2; // Double the size
                                        if (new_capacity > MAX_CACHE_SIZE)
                                        {
                                            new_capacity = MAX_CACHE_SIZE; // Cap at max size
                                        }

                                        log_message("INFO|CLIENT %d|CACHE|Reallocating cache buffer to %d bytes", client_id, (int)new_capacity);
                                        char *new_cache_buffer = (char *)realloc(cache_buffer, new_capacity);

                                        if (new_cache_buffer == NULL)
                                        {
                                            // Realloc failed!
                                            log_message("ERROR|CLIENT %d|CACHE|realloc failed. Stopping cache.", client_id);
                                            cache_this_response = 0;
                                            free(cache_buffer);
                                            cache_buffer = NULL;
                                        }
                                        else
                                        {
                                            cache_buffer = new_cache_buffer;
                                            cache_buffer_capacity = new_capacity;
                                        }
                                    }

                                    // If we are still caching and have a valid buffer, copy the data
                                    if (cache_this_response && cache_buffer != NULL)
                                    {
                                        memcpy(cache_buffer + total_bytes_received, response_buffer, bytes_recv);
                                        total_bytes_received += bytes_recv;
                                    }
                                }
                            } // end recv() loop

                            // --- Add to cache ---
                            // Check if we should (and can) add the buffer to the cache
                            if (cache_this_response && total_bytes_received > 0 && cache_buffer != NULL)
                            {
                                log_message("INFO|CLIENT %d|CACHE_ADD|Storing %d bytes for: %s", client_id, (int)total_bytes_received, url);
                                // This function copies the data, so we can free our buffer
                                cache_add(url, cache_buffer, total_bytes_received);
                            }
                            // Clean up the dynamic cache buffer
                            if (cache_buffer)
                            {
                                free(cache_buffer);
                            }
                        }
                        // Close the connection to the remote server
                        closesocket(remote_socket);
                    }
                }
            }
            // Free the URL string from get_url_from_request
            free(url);
        }
    }

    // --- Common cleanup ---
    // Free the main client buffer
    free(buffer);
    log_message("INFO|CLIENT %d|Connection closed.", client_id);
    // Close the client's socket
    closesocket(client_socket);
    // Release this thread's slot in the semaphore
    ReleaseSemaphore(semaphore, 1, NULL);
    return 0; // End the thread
}