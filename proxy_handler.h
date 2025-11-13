#pragma once

#define _WIN32_WINNT 0x0601
#include <windows.h>

/**
 * @brief Handles a single client connection in a separate thread.
 * @param arg A pointer to the client's SOCKET.
 */
DWORD WINAPI handle_client(LPVOID arg);