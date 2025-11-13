/**
 * @file proxy_main.c
 * @brief Main entry point for the multithreaded proxy server.
 * @details This file initializes Winsock, the logger, the cache,
 * and a semaphore. It then enters a loop, accepting new client
 * connections and dispatching each to a new thread for handling.
 */

// Define Windows version target for advanced features (e.g., getaddrinfo)
#define _WIN32_WINNT 0x0601

// Exclude rarely-used stuff from Windows headers
#define WIN32_LEAN_AND_MEAN
// Suppress warnings for using older Winsock functions (like inet_ntoa)
#define _WINSOCK_DEPRECATED_NO_WARNINGS
// Suppress warnings for using standard C functions (like sprintf, strcpy)
#define _CRT_SECURE_NO_WARNINGS

// Standard C libraries
#include <stdio.h>
#include <stdlib.h>

// Winsock libraries for networking
#include <WinSock2.h>
#include <Ws2tcpip.h>
// Windows API for threading and semaphores
#include <windows.h>

// Project-specific headers
#include "proxy_cache.h"   // LRU Cache system
#include "proxy_handler.h" // Client connection logic (handle_client)
#include "proxy_logger.h"  // Thread-safe logger

// --- Global settings ---
// Define the maximum number of concurrent client threads
#define MAX_CLIENTS 100
// Server address structure
struct sockaddr_in serverAddr;
// Default port number, can be overridden by command-line argument
int port_no = 8080;
// Semaphore to limit the number of concurrent clients to MAX_CLIENTS
HANDLE semaphore;

// Socket descriptor for the main proxy server (listening socket)
SOCKET proxy_socketId;

/**
 * @brief Main function of the proxy server.
 * @param argc Number of command-line arguments.
 * @param argv Array of command-line argument strings.
 * @return 0 on successful shutdown, 1 on fatal error.
 */
int main(int argc, char *argv[])
{
	// --- 1. Initialization ---

	// Initialize Winsock
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
	{
		// MAKEWORD(2,2) requests Winsock version 2.2
		printf("[FATAL] WSAStartup failed: %d\n", WSAGetLastError());
		return 1; // Cannot proceed without Winsock
	}

	// Initialize the thread-safe logger
	if (!logger_init("proxy.log"))
	{
		printf("[FATAL] Logger initialization failed.\n");
		WSACleanup(); // Clean up Winsock
		return 1;
	}

	log_message("INFO|SERVER|Windows Sockets (WSA) initialized.");

	// Initialize the cache system
	cache_init();
	log_message("INFO|SERVER|LRU Cache initialized.");

	// Initialize the semaphore
	// Initial count = MAX_CLIENTS, Max count = MAX_CLIENTS
	// This means up to MAX_CLIENTS threads can acquire the semaphore.
	semaphore = CreateSemaphoreW(NULL, MAX_CLIENTS, MAX_CLIENTS, NULL);
	if (semaphore == NULL)
	{
		log_message("FATAL|SERVER|CreateSemaphoreW failed: %d", GetLastError());
		logger_destroy(); // Clean up logger
		WSACleanup();	  // Clean up Winsock
		return 1;
	}

	// --- 2. Command-line Argument Parsing ---
	if (argc != 2)
	{
		log_message("FATAL|SERVER|Usage: %s <port>", argv[0]);
		logger_destroy();
		CloseHandle(semaphore); // Clean up semaphore
		WSACleanup();
		return 1;
	}

	// Convert the port argument from string to integer
	port_no = atoi(argv[1]);
	if (port_no <= 0 || port_no > 65535)
	{
		log_message("FATAL|SERVER|Invalid port number: %d", port_no);
		logger_destroy();
		CloseHandle(semaphore);
		WSACleanup();
		return 1;
	}

	// --- 3. Server Socket Setup ---

	// Create a socket for the proxy server
	// AF_INET = IPv4, SOCK_STREAM = TCP
	proxy_socketId = socket(AF_INET, SOCK_STREAM, 0);
	if (proxy_socketId == INVALID_SOCKET)
	{
		log_message("FATAL|SERVER|Socket creation failed: %d", WSAGetLastError());
		logger_destroy();
		CloseHandle(semaphore);
		WSACleanup();
		return 1;
	}

	// Set the SO_REUSEADDR socket option
	// This allows the server to restart and bind to the port quickly
	// without waiting for the TIME_WAIT state to expire.
	int reuse = 1;
	if (setsockopt(proxy_socketId, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse)) != 0)
	{
		log_message("FATAL|SERVER|setsockopt failed: %d", WSAGetLastError());
		closesocket(proxy_socketId);
		logger_destroy();
		CloseHandle(semaphore);
		WSACleanup();
		return 1;
	}

	// --- 4. Bind Socket ---

	// Zero out the server address structure
	ZeroMemory(&serverAddr, sizeof(serverAddr));
	// Set address family to IPv4
	serverAddr.sin_family = AF_INET;
	// Convert port number to network byte order (Big Endian)
	serverAddr.sin_port = htons(port_no);
	// Bind to any available local IP address
	serverAddr.sin_addr.s_addr = INADDR_ANY;

	// Bind the socket to the specified address and port
	if (bind(proxy_socketId, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) != 0)
	{
		log_message("FATAL|SERVER|Bind failed: %d", WSAGetLastError());
		closesocket(proxy_socketId);
		logger_destroy();
		CloseHandle(semaphore);
		WSACleanup();
		return 1;
	}
	log_message("INFO|SERVER|Socket bound successfully to port %d.", port_no);

	// --- 5. Listen for Connections ---

	// Put the socket into listening mode
	// MAX_CLIENTS is the backlog (queue size) for pending connections
	if (listen(proxy_socketId, MAX_CLIENTS) < 0)
	{
		log_message("FATAL|SERVER|Listen failed: %d", WSAGetLastError());
		closesocket(proxy_socketId);
		logger_destroy();
		CloseHandle(semaphore);
		WSACleanup();
		return 1;
	}

	log_message("INFO|SERVER|Proxy server listening on port %d... (Max clients: %d)", port_no, MAX_CLIENTS);

	// --- 6. Main Accept Loop ---
	while (TRUE)
	{
		struct sockaddr_in clientAddr; // Structure to hold client's address
		int client_length = sizeof(clientAddr);
		ZeroMemory(&clientAddr, sizeof(clientAddr));

		// Block and wait for a client to connect
		SOCKET client_socketId = accept(proxy_socketId, (struct sockaddr *)&clientAddr, &client_length);
		if (client_socketId == INVALID_SOCKET)
		{
			// Accept failed, log it and continue to the next iteration
			log_message("WARN|SERVER|accept failed: %d", WSAGetLastError());
			continue;
		}

		// --- Semaphore Check ---
		// Wait until a slot is free (semaphore count > 0)
		// This blocks if MAX_CLIENTS threads are already running.
		WaitForSingleObject(semaphore, INFINITE);
		// At this point, we have "taken" one slot, and the count is decremented.

		// --- Client Thread Creation ---

		// We must pass the client_socketId to the new thread.
		// We can't pass a pointer to the local variable 'client_socketId'
		// because it will change on the next loop iteration.
		// So, we allocate memory for it on the heap.
		// The new thread will be responsible for freeing this memory.
		SOCKET *client_socket_ptr = (SOCKET *)malloc(sizeof(SOCKET));
		if (!client_socket_ptr)
		{
			log_message("ERROR|SERVER|Failed to allocate memory for client socket.");
			closesocket(client_socketId);
			// We took a semaphore slot but failed to create a thread,
			// so we must release the slot back.
			ReleaseSemaphore(semaphore, 1, NULL);
			continue;
		}
		*client_socket_ptr = client_socketId; // Store the socket descriptor

		// Log the new connection
		log_message("INFO|CLIENT %d|Connection accepted from %s:%d",
					(int)client_socketId,
					inet_ntoa(clientAddr.sin_addr), // Convert client IP to string
					ntohs(clientAddr.sin_port));	// Convert client port to host byte order

		// Create a new thread to handle this client
		// It will execute the 'handle_client' function
		HANDLE clientThread = CreateThread(
			NULL,					   // Default security attributes
			0,						   // Default stack size
			handle_client,			   // Function to execute
			(LPVOID)client_socket_ptr, // Argument to pass to the function
			0,						   // Run immediately
			NULL);					   // Don't need the thread ID

		if (clientThread == NULL)
		{
			log_message("ERROR|CLIENT %d|CreateThread failed: %d",
						(int)client_socketId, GetLastError());
			closesocket(client_socketId);
			free(client_socket_ptr); // Free the memory we allocated
			// Release the semaphore slot
			ReleaseSemaphore(semaphore, 1, NULL);
		}
		else
		{
			// "Fire and forget" - Detach the thread.
			// We close the handle so we don't have to 'join' it later.
			// The thread will run independently, and its resources will be
			// released when it finishes.
			CloseHandle(clientThread);
		}
	} // End of while(TRUE) loop

	// --- 7. Shutdown (Code is unreachable in this design) ---
	// This code would only be reached if the while(TRUE) loop had a break condition.
	log_message("INFO|SERVER|Shutting down...");
	closesocket(proxy_socketId);
	cache_destroy();
	CloseHandle(semaphore);
	logger_destroy();
	WSACleanup();
	return 0;
}