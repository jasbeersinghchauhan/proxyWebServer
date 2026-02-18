#include <iostream>
#include <algorithm>
#include <vector>
#include <string>
#include <cstring>
#include <memory>
#include <chrono>
#include <semaphore>

#include "proxy_handler.hpp"
#include "proxy_cache.hpp"
#include "proxy_logger.hpp"

constexpr size_t MAX_HEADER_SIZE = 8192;
constexpr size_t HTTPS_RECV_BUFFER_SIZE = 8192;
constexpr size_t HTTP_RECV_BUFFER_SIZE = 4096;

constexpr std::string_view HTTP_END = "\r\n";
constexpr std::string_view HEADER_END = "\r\n\r\n";

ProxyHandler::ProxyHandler() {}

ProxyHandler::~ProxyHandler() {}

void ProxyHandler::sendHttpError(const socket_t &client_socket, int status_code, const std::string &message)
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
            std::cerr << "ERROR|Failed to send error response: " << getSocketError() << "\n";
            return;
        }
        sent += n;
    }
}

std::string ProxyHandler::parseRequestTarget(const std::vector<char> &request)
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

bool ProxyHandler::parseHttpUrl(const std::string &url, HttpRequestPart &request_Part)
{
    if (url.empty())
    {
        log("WARN|HTTP|Empty URL provided for parsing.\n");
        return false;
    }

    std::string protocol_delimiter = "://";
    size_t protocol_pos = url.find(protocol_delimiter);
    if (protocol_pos == std::string::npos)
    {
        log("WARN|HTTP|Malformed URL: Missing protocol delimiter.\n");
        return false;
    }

    size_t host_start = protocol_pos + protocol_delimiter.length();
    size_t path_start = url.find("/", host_start);

    std::string authority_segment;

    if (path_start == std::string::npos)
    {
        authority_segment = url.substr(host_start);
        request_Part.path = "/";
    }
    else
    {
        authority_segment = url.substr(host_start, path_start - host_start);
        request_Part.path = url.substr(path_start);
    }

    size_t port_pos = authority_segment.rfind(":");
    if (port_pos != std::string::npos)
    {
        request_Part.host = authority_segment.substr(0, port_pos);
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

        request_Part.port = port_str;
    }
    else
    {
        request_Part.host = authority_segment;
        request_Part.port = "80";
    }
    return true;
}

bool ProxyHandler::isMethod(const std::vector<char> &request_buffer, const std::string &method)
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

socket_t ProxyHandler::connectToRemoteHost(const std::string &host, const std::string &port)
{
    socket_t remote_server_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (remote_server_socket == INVALID_SOCKET)
    {
        log("ERROR|REMOTE|Failed to create socket for remote host {}:{}\n", host, port);
        return INVALID_SOCKET;
    }

    setSocketTimeout(remote_server_socket, 30);

    struct addrinfo hints = {}, *result = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &result) != 0)
    {
        log("ERROR|REMOTE|Failed to resolve host: {}\n", host);
        closeSocket(remote_server_socket);
        return INVALID_SOCKET;
    }

    if (connect(remote_server_socket, result->ai_addr, (int)result->ai_addrlen) == SOCKET_ERROR)
    {
        log("ERROR|REMOTE|Failed to connect to remote host {}:{}\n", host, port);
        freeaddrinfo(result);
        closeSocket(remote_server_socket);
        return INVALID_SOCKET;
    }

    freeaddrinfo(result);
    return remote_server_socket;
}

void ProxyHandler::handleClient(const socket_t client_socket, proxy_cache::Cache &cache_system, std::counting_semaphore<INT_MAX> &connection_semaphore)
{
    SemaphoreGuard guard(connection_semaphore);
    SocketGuard socket_guard(client_socket);

    setSocketTimeout(client_socket, 30);

    const int client_id = (int)client_socket;

    std::vector<char> request_buffer;
    char temp_buffer[HTTP_RECV_BUFFER_SIZE];

    int bytes_received = recv(client_socket, temp_buffer, HTTP_RECV_BUFFER_SIZE, 0);

    if (bytes_received <= 0)
    {
        log("INFO|CLIENT|{}|Client disconnected immediately or timed out.\n", client_id);
        return;
    }

    request_buffer.insert(request_buffer.end(), temp_buffer, temp_buffer + bytes_received);
    int total_bytes_received = bytes_received;

    if (isMethod(request_buffer, "CONNECT ")) // HTTPS CONNECT Section
    {
        log("INFO|CLIENT|{}|HTTP CONNECT request received.\n", client_id);

        std::string host;
        std::string port = "443";

        std::string url = parseRequestTarget(request_buffer);
        if (url.empty())
        {
            log("WARN|CLIENT|{}|HTTPS|Malformed HTTPS request.\n", client_id);
            return;
        }

        size_t port_pos = url.rfind(":");
        if (port_pos != std::string::npos)
        {
            host = url.substr(0, port_pos);
            port = url.substr(port_pos + 1);
        }
        else
            host = url.substr(0);

        log("INFO|CLIENT|{}|CONNECT|CONNECT target {}:{}\n", client_id, host, port);

        socket_t remote_server_socket = connectToRemoteHost(host, port);

        SocketGuard remote_socket_guard(remote_server_socket);

        if (remote_server_socket == INVALID_SOCKET)
        {
            log("ERROR|CLIENT|{}|CONNECT|Failed to connect to {}\n", client_id, host);
            return;
        }

        std::string rebuilt = "HTTP/1.1 200 OK \r\n\r\n";

        int sent_rebuilt = 0;
        while (sent_rebuilt < rebuilt.size())
        {
            int len = send(client_socket, rebuilt.data() + sent_rebuilt, rebuilt.size() - sent_rebuilt, 0);
            sent_rebuilt += len;
        }

        log("INFO|CLIENT|{}|CONNECT|Tunnel established to {}:{}\n", client_id, host, port);

        fd_set read_fds;

        char tunnel_buffer[HTTPS_RECV_BUFFER_SIZE];

        size_t tunnel_bytes = 0;
        while (true)
        {
            struct timeval select_timeout;
            select_timeout.tv_sec = 100;
            select_timeout.tv_usec = 0;

            FD_ZERO(&read_fds);

            FD_SET(client_socket, &read_fds);
            FD_SET(remote_server_socket, &read_fds);

            socket_t max_socket_descriptor = (client_socket > remote_server_socket) ? client_socket : remote_server_socket;

            int activity = select(max_socket_descriptor + 1, &read_fds, NULL, NULL, &select_timeout);
            if (activity == SOCKET_ERROR)
            {
                log("ERROR|CLIENT|{}|CONNECT|select() failed {}\n", client_id, getSocketError());
                break;
            }
            else if (activity == 0)
            {
                log("INFO|CLIENT|{}|CONNECT|Tunnel timed out for {}:{}\n", client_id, host, port);
                break;
            }

            if (FD_ISSET(client_socket, &read_fds))
            {
                int len = recv(client_socket, tunnel_buffer, HTTPS_RECV_BUFFER_SIZE, 0);

                if (len <= 0)
                    break;

                int total_sent = 0;
                while (total_sent < len)
                {
                    int send_data = send(remote_server_socket, tunnel_buffer + total_sent, len - total_sent, 0);

                    if (send_data == SOCKET_ERROR)
                        return;

                    total_sent += send_data;
                    tunnel_bytes += send_data;
                }
            }

            if (FD_ISSET(remote_server_socket, &read_fds))
            {
                int len = recv(remote_server_socket, tunnel_buffer, HTTPS_RECV_BUFFER_SIZE, 0);

                if (len <= 0)
                    break;

                int total_sent = 0;
                while (total_sent < len)
                {
                    int send_data = send(client_socket, tunnel_buffer + total_sent, len - total_sent, 0);

                    if (send_data == SOCKET_ERROR)
                        return;

                    total_sent += send_data;
                    tunnel_bytes += send_data;
                }
            }
        }

        log("INFO|CLIENT|{}|CONNECT|Tunnel to {}:{} closed. {} bytes relayed.\n",
            client_id,
            host,
            port,
            tunnel_bytes);
    }
    else if (isMethod(request_buffer, "GET ")) // HTTP GET Section
    {
        log("INFO|CLIENT|{}|HTTP Get request received.\n", client_id);

        while (true)
        {
            auto it = std::search(request_buffer.begin(), request_buffer.end(), HEADER_END.begin(), HEADER_END.end());

            if (it != request_buffer.end())
                break;

            if (total_bytes_received >= MAX_HEADER_SIZE)
            {
                log("WARN|CLIENT|{}|HTTP|Header too large.\n", client_id);
                return;
            }

            bytes_received = recv(client_socket, temp_buffer, HTTP_RECV_BUFFER_SIZE, 0);

            if (bytes_received <= 0)
            {
                log("INFO|CLIENT|{}|Client disconnected while receiving HTTP headers.\n", client_id);
                return;
            }
            request_buffer.insert(request_buffer.end(), temp_buffer, temp_buffer + bytes_received);
            total_bytes_received += bytes_received;
        }

        // Absolute-form only
        // E.g. GET http://example.com::port/path HTTP/1.1

        std::string url = parseRequestTarget(request_buffer);

        if (url.empty())
        {
            log("WARN|CLIENT|{}|HTTP|Malformed HTTP request.\n", client_id);
            return;
        }

        log("INFO|CLIENT|{}|HTTP|Request URL: {}\n", client_id, url);

        std::vector<char> cached_response = cache_system.cacheFind(url);

        if (!cached_response.empty())
        {
            log("INFO|CLIENT|{}|CACHE_HIT|{}\n", client_id, url);
            send(client_socket, cached_response.data(), cached_response.size(), 0);
        }
        else
        {
            log("INFO|CLIENT|{}|CACHE_MISS|{}\n", client_id, url);

            HttpRequestPart request_Part;

            if (!parseHttpUrl(url, request_Part))
            {
                log("WARN|CLIENT|{}|HTTP|Failed to parse HTTP request.\n", client_id);
                return;
            }

            log("INFO|CLIENT|{}|REMOTE|Connecting to {}:{}\n", client_id, request_Part.host, request_Part.port);

            socket_t remote_server_socket = connectToRemoteHost(request_Part.host, request_Part.port);
            if (remote_server_socket == INVALID_SOCKET)
            {
                log("ERROR|CLIENT|{}|REMOTE|Failed to connect to remote host.\n", client_id);
                sendHttpError(client_socket, 502, "Bad Gateway");
                return;
            }

            SocketGuard remote_socket_guard(remote_server_socket);

            log("INFO|CLIENT|{}|REMOTE|Connected to {}:{}\n", client_id, request_Part.host, request_Part.port);

            std::vector<char> modified_request;
            modified_request.reserve(request_buffer.size());

            std::string rebuilt = "GET " + request_Part.path + " HTTP/1.1\r\n" +
                                  "Host: " + request_Part.host + "\r\n" +
                                  "Connection: close\r\n";
            modified_request.insert(modified_request.end(), rebuilt.begin(), rebuilt.end());

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

            log("INFO|CLIENT|{}|REMOTE|Forwarding: GET {}\n", client_id, request_Part.path);

            if (send(remote_server_socket, modified_request.data(), modified_request.size(), 0) == SOCKET_ERROR)
            {
                log("INFO|CLIENT|{}|REMOTE|send() failed: {}\n", client_id, getSocketError());
                return;
            }

            log("INFO|CLIENT|{}|REMOTE|Awaiting response from {}:{}\n", client_id, request_Part.host, request_Part.port);

            std::vector<char> server_response_data;

            int total_bytes_received = 0;
            bool status_logged = false;
            while (true)
            {
                char temp_buffer[HTTP_RECV_BUFFER_SIZE];
                int bytes_received = recv(remote_server_socket, temp_buffer, HTTP_RECV_BUFFER_SIZE, 0);

                if (bytes_received <= 0)
                    break;

                if (!status_logged)
                {
                    std::string header(temp_buffer, bytes_received);

                    size_t pos = header.find("\r\n");
                    if (pos != std::string::npos)
                    {
                        std::string first_line = header.substr(0, pos);
                        log("INFO|CLIENT|{}|REMOTE|Response: {}\n", client_id, first_line);
                        status_logged = true;
                    }
                }

                int bytes_sent = 0;
                while (bytes_sent < bytes_received)
                {
                    int sent = send(client_socket, temp_buffer + bytes_sent, bytes_received - bytes_sent, 0);
                    if (sent == SOCKET_ERROR)
                    {
                        log("INFO|CLIENT|{}|REMOTE|send() failed: {}\n", client_id, getSocketError());
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

            log("INFO|CLIENT|{}|REMOTE|Forwarded {} bytes to client.\n",
                client_id,
                total_bytes_received);

            if (total_bytes_received <= proxy_cache::MAX_CACHE_BYTES)
            {
                cache_system.cacheAdd(url, server_response_data);
                log("INFO|CLIENT|{}|CACHE_STORE|{} ({} bytes)\n",
                    client_id,
                    url,
                    server_response_data.size());
            }
            log("INFO|CLIENT|{}|REMOTE|Connection to {} closed.\n", client_id, request_Part.host);
        }
    }
    else
    {
        log("INFO|CLIENT|{}|Unsupported HTTP method.\n", client_id);
    }
    return;
}