#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <string>

// Link with Windows Socket library
#pragma comment(lib, "ws2_32.lib")

// --- Configuration ---
#define PROXY_IP "192.168.1.9"
#define PROXY_PORT 19112

// --- Function Prototypes ---
int WINAPI DetourConnect(SOCKET s, const struct sockaddr* name, int namelen);
int WINAPI DetourRecv(SOCKET s, char* buf, int len, int flags);
int WINAPI DetourWSARecv(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount,
    LPDWORD lpNumberOfBytesRecvd, LPDWORD lpFlags,
    LPWSAOVERLAPPED lpOverlapped,
    LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine);
