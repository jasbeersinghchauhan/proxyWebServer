#pragma once

#include <memory>
#include <semaphore>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <vector>
#include <string>

#include "proxy_cache.hpp"

class ProxyHandler
{
private:
    struct SemaphoreGuard
    {
        std::counting_semaphore<INT_MAX> &sem;
        SemaphoreGuard(std::counting_semaphore<INT_MAX> &s) : sem(s) {}
        ~SemaphoreGuard() { sem.release(); }
    };

    struct SocketGuard {
        SOCKET client_socket;
        SocketGuard(SOCKET client_socket) : client_socket(client_socket) {}
        ~SocketGuard() {
            if (client_socket != INVALID_SOCKET)
                closesocket(client_socket);
        }
    };

    typedef struct HttpRequestPart
    {
        std::string host;
        std::string path;
        std::string port;
        HttpRequestPart() : port("80") {}
    } HttpRequestPart;

    static void send_error(const SOCKET& client_socket, int status_code, const std::string& message);

    static bool parse_method(const std::vector<char> &request_buffer, const std::string &method);

    static std::string parse_request_target(const std::vector<char> &request);

    static bool parse_http_request(const std::string &url, HttpRequestPart &request_part);

    static SOCKET connect_to_remote_host(const HttpRequestPart &request_part);

public:
    ProxyHandler();
    ~ProxyHandler();

    static void client_handler(const SOCKET client_socket, proxy_cache::Cache &cache_system, std::counting_semaphore<INT_MAX> &connection_semaphore);
};