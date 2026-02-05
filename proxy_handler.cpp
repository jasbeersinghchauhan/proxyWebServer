#include <iostream>

#include <algorithm>
#include <vector>
#include <string>
#include <cstring>
#include <memory>
#include <chrono>
#include <semaphore>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "proxy_handler.hpp"
#include "proxy_cache.hpp"

constexpr size_t MAX_HEADER_SIZE = 8192;
constexpr size_t RECV_CHUNK_SIZE = 4096;

constexpr std::string_view HTTP_END = "\r\n";
constexpr std::string_view HEADER_END = "\r\n\r\n";

ProxyHandler::ProxyHandler() {}

ProxyHandler::~ProxyHandler() {}

void ProxyHandler::send_error(const SOCKET &client_socket, int status_code, const std::string &message)
{
    std::string body = "<html><body><h1>" + std::to_string(status_code) + " " + message + "</h1></body></html>";
    std::string response = "HTTP/1.1 " + std::to_string(status_code) + " " + message + "\r\n" +
                           "Content-Type: text/html\r\n" +
                           "Content-Length: " + std::to_string(body.length()) + "\r\n" +
                           "Connection: close\r\n\r\n" +
                           body;

    int sent = 0;
    while (sent < response.length())
    {
        int n = send(client_socket, response.c_str() + sent, response.length() - sent, 0);
        if (n == SOCKET_ERROR)
        {
            std::cerr << "ERROR|Failed to send error response: " << WSAGetLastError() << "\n";
            return;
        }
        sent += n;
    }
}

std::string ProxyHandler::parse_request_target(const std::vector<char> &request)
{
    auto first_space = std::find(request.begin(), request.end(), ' ');
    if (first_space == request.end())
        return {};

    auto target_start = std::next(first_space);
    if (target_start == request.end())
        return {};

    auto target_end = std::find(target_start, request.end(), ' ');
    if (target_end == request.end())
        return {};
    return std::string(target_start, target_end);
}

bool ProxyHandler::parse_http_request(const std::string &url, HttpRequestPart &request_part)
{
    if (url.empty())
    {
        std::cout << "WARN|HTTP|Empty URL provided for parsing.\n";
        return false;
    }

    std::string protocol_delimiter = "://";
    size_t protocol_pos = url.find(protocol_delimiter);
    if (protocol_pos == std::string::npos)
    {
        std::cout << "WARN|HTTP|Malformed URL: Missing protocol delimiter.\n";
        return false;
    }

    size_t host_start = protocol_pos + protocol_delimiter.length();
    size_t path_start = url.find("/", host_start);

    std::string authority_segment;

    if (path_start == std::string::npos)
    {
        authority_segment = url.substr(host_start);
        request_part.path = "/";
    }
    else
    {
        authority_segment = url.substr(host_start, path_start - host_start);
        request_part.path = url.substr(path_start);
    }

    size_t port_pos = authority_segment.rfind(":");
    if (port_pos != std::string::npos)
    {
        request_part.host = authority_segment.substr(0, port_pos);
        std::string port_str = authority_segment.substr(port_pos + 1);

        if (port_str.empty())
            return false;

        for (char c : port_str)
        {
            if (!isdigit(c))
                return false;
        }

        try
        {
            int port_num = std::stoi(port_str);
            if (port_num < 0 || port_num > 65535)
                return false;
        }
        catch (...)
        {
            return false;
        }

        request_part.port = port_str;
    }
    else
    {
        request_part.host = authority_segment;
        request_part.port = "80";
    }
    return true;
}

bool ProxyHandler::parse_method(const std::vector<char> &request_buffer, const std::string &method)
{
    const int method_length = method.length();

    if (request_buffer.size() < method_length)
        return false;

    for (int i = 0; i < method_length; ++i)
    {
        if (request_buffer[i] != method[i])
            return false;
    }
    return true;
}

SOCKET ProxyHandler::connect_to_remote_host(const HttpRequestPart &request_part)
{
    SOCKET remote_server_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (remote_server_socket == INVALID_SOCKET)
    {
        std::cout << "ERROR|Failed to create socket for remote host " << request_part.host << ":" << request_part.port << "\n";
        return INVALID_SOCKET;
    }

    DWORD timeout_duration = 30000; // 30 seconds
    setsockopt(remote_server_socket, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_duration, sizeof(timeout_duration));
    setsockopt(remote_server_socket, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout_duration, sizeof(timeout_duration));

    struct addrinfo hints = {}, *result = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    if (getaddrinfo(request_part.host.c_str(), request_part.port.c_str(), &hints, &result) != 0)
    {
        std::cout << "ERROR|Failed to resolve host: " << request_part.host << "\n";
        closesocket(remote_server_socket);
        return INVALID_SOCKET;
    }

    if (connect(remote_server_socket, result->ai_addr, (int)result->ai_addrlen) == SOCKET_ERROR)
    {
        std::cout << "ERROR|Failed to connect to remote host " << request_part.host << ":" << request_part.port << "\n";
        freeaddrinfo(result);
        closesocket(remote_server_socket);
        return INVALID_SOCKET;
    }

    freeaddrinfo(result);
    return remote_server_socket;
}

void ProxyHandler::client_handler(const SOCKET client_socket, proxy_cache::Cache &cache_system, std::counting_semaphore<INT_MAX> &connection_semaphore)
{
    SemaphoreGuard guard(connection_semaphore);
    SocketGuard socket_guard(client_socket);

    const DWORD timeout_duration = 30000;

    setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_duration, sizeof(timeout_duration));
    setsockopt(client_socket, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout_duration, sizeof(timeout_duration));

    const int client_id = (int)client_socket;

    std::vector<char> request_buffer;
    char temp_buffer[RECV_CHUNK_SIZE];

    int bytes_received = recv(client_socket, temp_buffer, RECV_CHUNK_SIZE, 0);

    if (bytes_received <= 0)
    {
        std::cout << "INFO|CLIENT " << client_id << "|Client disconnected immediately or timed out.\n";
        return;
    }

    request_buffer.insert(request_buffer.end(), temp_buffer, temp_buffer + bytes_received);
    int total_bytes_received = bytes_received;

    if (parse_method(request_buffer, "CONNECT "))
    {
        std::cout << "INFO|CLIENT " << client_id << "|HTTP CONNECT method not supported.\n";
        return;
    }
    else if (parse_method(request_buffer, "GET ")) // HTTP GET Section
    {
        std::cout << "INFO|CLIENT " << client_id << "|HTTP Get request received.\n";

        while (true)
        {
            auto it = std::search(request_buffer.begin(), request_buffer.end(), HEADER_END.begin(), HEADER_END.end());

            if (it != request_buffer.end())
                break;

            if (total_bytes_received >= MAX_HEADER_SIZE)
            {
                std::cout << "WARN|CLIENT " << client_id << "|HTTP|Header too large.\n";
                return;
            }

            bytes_received = recv(client_socket, temp_buffer, RECV_CHUNK_SIZE, 0);

            if (bytes_received <= 0)
            {
                std::cout << "INFO|CLIENT " << client_id << "|Client disconnected while receiving HTTP headers.\n";
                return;
            }
            request_buffer.insert(request_buffer.end(), temp_buffer, temp_buffer + bytes_received);
            total_bytes_received += bytes_received;
        }

        // Absolute-form only
        // E.g. GET http://example.com::port/path HTTP/1.1

        std::string url = parse_request_target(request_buffer);

        if (url.empty())
        {
            std::cout << "WARN|CLIENT " << client_id << "|HTTP|Malformed HTTP request.\n";
            return;
        }

        std::vector<char> cached_response = cache_system.cache_find(url);

        if (!cached_response.empty())
        {
            std::cout << "INFO|CLIENT " << client_id << "|CACHE_HIT|" << url << "\n";
            send(client_socket, cached_response.data(), cached_response.size(), 0);
        }
        else
        {
            std::cout << "INFO|CLIENT " << client_id << "|CACHE_MISS|" << url << "\n";

            HttpRequestPart request_part;

            if (!parse_http_request(url, request_part))
            {
                std::cout << "WARN|CLIENT " << client_id << "|HTTP|Failed to parse HTTP request.\n";
                return;
            }

            std::cout << "INFO|CLIENT " << client_id << "|REMOTE|Connecting to " << request_part.host << ":" << request_part.port << "\n";

            SOCKET remote_server_socket = connect_to_remote_host(request_part);
            if (remote_server_socket == INVALID_SOCKET)
            {
                std::cout << "ERROR|CLIENT " << client_id << "|REMOTE|Failed to connect to remote host.\n";
                send_error(client_socket, 502, "Bad Gateway");
                return;
            }

            SocketGuard remote_socket_guard(remote_server_socket);

            std::cout << "INFO|CLIENT " << client_id << "|REMOTE|Connected to " << request_part.host << ":" << request_part.port << "\n";

            std::vector<char> modified_request;
            modified_request.reserve(request_buffer.size());

            std::string rebuilt = "GET " + request_part.path + " HTTP/1.1\r\n" + 
            "Host: " + request_part.host + "\r\n" + 
            "Connection: close\r\n";
            modified_request.insert(modified_request.end(), rebuilt.begin(), rebuilt.end());

            bool connection_header_found = false, host_header_found = false;

            auto it_start = std::search(request_buffer.begin(), request_buffer.end(), HTTP_END.begin(), HTTP_END.end());
            if (it_start == request_buffer.end())
            {
                return;
            }

            it_start += HTTP_END.size();

            while (true)
            {
                auto it_end = std::search(it_start, request_buffer.end(), HTTP_END.begin(), HTTP_END.end());
                if (it_end == request_buffer.end())
                    break;

                if (it_end == it_start)
                {
                    modified_request.insert(modified_request.end(), HTTP_END.begin(), HTTP_END.end());
                    break;
                }
                auto line_begin = it_start, line_end = it_end;

                auto is_equals_prefix = [&](const char *h)
                {
                    auto it = line_begin;
                    for (const char *p = h; *p && it != line_end; ++p, ++it)
                    {
                        if (std::tolower(*it) != std::tolower(*p))
                            return false;
                    }
                    return *h && (line_begin + std::strlen(h) <= line_end);
                };

                if (is_equals_prefix("Host:") || is_equals_prefix("Connection:"))
                {
                    it_start = it_end + HTTP_END.size();
                    continue;
                }

                modified_request.insert(modified_request.end(), line_begin, line_end);

                modified_request.insert(modified_request.end(), HTTP_END.begin(), HTTP_END.end());

                it_start = it_end + HTTP_END.size();
            }

            std::cout << "INFO|CLIENT " << client_id << "|REMOTE|Forwarding: GET " << request_part.path << "\n";

            if (send(remote_server_socket, modified_request.data(), modified_request.size(), 0) == SOCKET_ERROR)
            {
                std::cerr << "INFO|CLIENT " << client_id << "|REMOTE|send() failed: " << WSAGetLastError() << "\n";
                return;
            }

            std::cout << "INFO|CLIENT " << client_id << "|REMOTE|Awaiting response from " << request_part.host << ":" << request_part.port << "\n";

            std::vector<char> server_response_data;

            int total_bytes_received = 0;
            while (true)
            {
                char temp_buffer[RECV_CHUNK_SIZE];
                int bytes_received = recv(remote_server_socket, temp_buffer, RECV_CHUNK_SIZE, 0);

                if (bytes_received <= 0)
                    break;

                int bytes_sent = 0;
                while (bytes_sent < bytes_received)
                {
                    int sent = send(client_socket, temp_buffer + bytes_sent, bytes_received - bytes_sent, 0);
                    if (sent == SOCKET_ERROR)
                    {
                        std::cerr << "INFO|CLIENT " << client_id << "|REMOTE|send() failed: " << WSAGetLastError() << "\n";
                        return;
                    }

                    if (sent == 0)
                        break;

                    bytes_sent += sent;
                }

                total_bytes_received += bytes_received;

                if (total_bytes_received <= proxy_cache::MAX_CACHE_BYTES)
                    server_response_data.insert(server_response_data.end(), temp_buffer, temp_buffer + bytes_received);
            }

            if (total_bytes_received <= proxy_cache::MAX_CACHE_BYTES)
            {
                cache_system.cache_add(url, server_response_data);
            }
            std::cout << "INFO|CLIENT " << client_id << "|REMOTE|Connection to " << request_part.host << " closed.\n";
        }
    }
    else
    {
        std::cout << "INFO|CLIENT " << client_id << "|Unsupported HTTP method.\n";
    }
    return;
}