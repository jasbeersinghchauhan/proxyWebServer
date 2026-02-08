#pragma once

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

using socket_t = SOCKET;
using socklen_t = int;

inline void closeSocket(socket_t s) { closesocket(s); }

inline bool initSockets()
{
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}

inline void cleanupSocket()
{
    WSACleanup();
}

inline int getSocketError()
{
    return WSAGetLastError();
}

#else

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <unistd.h>
#include <netdb.h>
#include <errno.h>
#include <signal.h>

using socket_t = int;
constexpr int INVALID_SOCKET = -1;
constexpr int SOCKET_ERROR = -1;

inline bool initSockets() { return true; }

inline void closeSocket(socket_t s) { close(s); }

inline void cleanupSocket() {}

inline int getSocketError()
{
    return errno;
}

#endif

inline void setSocketTimeout(socket_t s, int seconds)
{
#ifdef _WIN32
    DWORD timeout = seconds * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));
#else
    struct timeval timeout;
    timeout.tv_sec = seconds;
    timeout.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
}