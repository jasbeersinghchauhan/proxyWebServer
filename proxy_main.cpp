#include <iostream>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <thread>
#include <semaphore>
#include <atomic>
#include <memory>
#include <format>

#include "proxy_cache.hpp"
#include "proxy_logger.hpp"
#include "proxy_handler.hpp"

constexpr int DEFAULT_PORT = 8080;
constexpr int MAX_CONNECTIONS = 2000;

// Global server socket for client acceptance
SOCKET g_listen_socket = INVALID_SOCKET;
std::atomic<bool> g_is_server_running{true};

BOOL WINAPI console_handler(DWORD signal)
{
    if (signal == CTRL_C_EVENT || signal == CTRL_CLOSE_EVENT)
    {
        log("INFO|SERVER|Signal for shutdown received...\n");
        g_is_server_running = false;
        if (g_listen_socket != INVALID_SOCKET)
        {
            closesocket(g_listen_socket);
            g_listen_socket = INVALID_SOCKET;
        }
        return TRUE;
    }
    return FALSE;
}

int main(int argc, char *argv[])
{
    SetConsoleCtrlHandler(console_handler, TRUE);

    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0)
    {
        log("ERROR|SERVER|Failed to start winsock2 library: {}\n", WSAGetLastError());
        return 1;
    }

    int server_port = DEFAULT_PORT;
    if (argc > 1)
    {
        try
        {
            int port_no = std::stoi(argv[1]);
            if (port_no < 0 || port_no > 65535)
            {
                log("INFO|SERVER|Invalid port. Using default port\n");
            }
            else
                server_port = port_no;
        }
        catch (...)
        {
            log("INFO|SERVER|Invalid port format. Using default port\n");
        }
    }
    log("INFO|SERVER|Using port {} for connections\n", server_port);

    proxy_cache::Cache cache_system;
    log("INFO|SERVER|LRU Cache initialized.\n");
    std::counting_semaphore<INT_MAX> connection_semaphore(MAX_CONNECTIONS);

    g_listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (g_listen_socket == INVALID_SOCKET)
    {
        log("ERROR|SERVER|Socket creation failed: {}\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    char exclusive = 1;
    if (setsockopt(g_listen_socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (char *)&exclusive, sizeof(exclusive)) == SOCKET_ERROR)
    {
        log("ERROR|SERVER|Set socket options failed: {}\n", WSAGetLastError());
        closesocket(g_listen_socket);
        WSACleanup();
        return 1;
    }

    sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(g_listen_socket, (sockaddr *)&server_addr, sizeof(server_addr)) == SOCKET_ERROR)
    {
        log("ERROR|SERVER|Binding failed: {}\n", WSAGetLastError());
        closesocket(g_listen_socket);
        WSACleanup();
        return 1;
    }
    log("INFO|SERVER|Socket bound successfully to port {}.\n", server_port);

    if (listen(g_listen_socket, MAX_CONNECTIONS) == SOCKET_ERROR)
    {
        log("ERROR|SERVER|Listen failed: {}\n", WSAGetLastError());
        closesocket(g_listen_socket);
        WSACleanup();
        return 1;
    }

    log("INFO|SERVER|Listening on port {}.\n", server_port);

    while (g_is_server_running)
    {
        connection_semaphore.acquire();

        sockaddr_in client_addr;
        int addr_len = sizeof(client_addr);

        SOCKET client_socket = accept(g_listen_socket, (sockaddr *)&client_addr, &addr_len);
        if (client_socket == INVALID_SOCKET)
        {  
            if (!g_is_server_running)
            {
                connection_semaphore.release();
                break;
            }
            log("ERROR|SERVER|Accept failed: {}\n", WSAGetLastError());
            connection_semaphore.release();
            continue;
        }

        if (!g_is_server_running)
        {
            closesocket(client_socket);
            connection_semaphore.release();
            break;
        }

        log("INFO|SERVER|Connection accepted from {}:{}\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

        try
        {
            std::thread client_thread(ProxyHandler::handleClient, client_socket, std::ref(cache_system), std::ref(connection_semaphore));
            client_thread.detach();
        }
        catch (const std::system_error &e)
        {
            log("ERROR|SERVER|Failed to create thread: {}\n", e.what());
            closesocket(client_socket);
            connection_semaphore.release();
            continue;
        }
    }

    log("INFO|SERVER|Shutting down...\n");
    if (g_listen_socket != INVALID_SOCKET)
        closesocket(g_listen_socket);

    log("INFO|SERVER|Waiting for active connections to finish...\n");
    for (int i = 0; i < MAX_CONNECTIONS; i++)
    {
        connection_semaphore.acquire();
    }

    log("INFO|SERVER|All connections finished.\n");
    WSACleanup();
}