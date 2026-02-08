#pragma once

#include <memory>
#include <semaphore>
#include <vector>
#include <string>

#include "proxy_utils.hpp"
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
        socket_t a_socket;
        SocketGuard(socket_t client_socket) : a_socket(a_socket) {}
        ~SocketGuard() {
            if (a_socket != INVALID_SOCKET)
                closeSocket(a_socket);
        }
    };

    typedef struct HttpRequestPart
    {
        std::string host;
        std::string path;
        std::string port;
    } HttpRequestPart;

    static void sendHttpError(const socket_t& client_socket, int status_code, const std::string& message);

    static bool isMethod(const std::vector<char> &request_buffer, const std::string &method);

    static std::string parseRequestTarget(const std::vector<char> &request);

    static bool parseHttpUrl(const std::string &url, HttpRequestPart &requestPart);

    static socket_t connectToRemoteHost(const std::string& host, const std::string& port);
public:
    ProxyHandler();
    ~ProxyHandler();

    static void handleClient(const socket_t client_socket, proxy_cache::Cache &cache_system, std::counting_semaphore<INT_MAX> &connection_semaphore);
};