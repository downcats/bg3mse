// Winsock 2, the part the Osiris debugger's server (Osiris/Debugger/DebugInterface.cpp) uses.
//
// The types have their Windows layouts and the functions their Windows
// signatures and semantics (C++ linkage, so they never bind to the BSD
// functions of the same names). They are implemented over BSD sockets in
// Win32Sockets.cpp; WSAGetLastError() returns Windows error codes. A send()
// to a peer that has gone returns SOCKET_ERROR with WSAECONNRESET, as on
// Windows, and does not raise SIGPIPE.
#pragma once

#include <windows.h>

#include <sys/types.h>

typedef UINT_PTR SOCKET;
typedef unsigned short u_short;
typedef unsigned long u_long;
#define INVALID_SOCKET ((SOCKET)~0)
#define SOCKET_ERROR (-1)

#define AF_INET 2
#define SOCK_STREAM 1
#define IPPROTO_TCP 6
#define WSADESCRIPTION_LEN 256
#define WSASYS_STATUS_LEN 128

typedef struct WSAData {
    WORD wVersion;
    WORD wHighVersion;
    unsigned short iMaxSockets;
    unsigned short iMaxUdpDg;
    char* lpVendorInfo;
    char szDescription[WSADESCRIPTION_LEN + 1];
    char szSystemStatus[WSASYS_STATUS_LEN + 1];
} WSADATA, *LPWSADATA;

typedef struct in_addr {
    union {
        struct { BYTE s_b1, s_b2, s_b3, s_b4; } S_un_b;
        struct { WORD s_w1, s_w2; } S_un_w;
        ULONG S_addr;
    } S_un;
} IN_ADDR;

struct sockaddr {
    u_short sa_family;
    char sa_data[14];
};

struct sockaddr_in {
    short sin_family;
    u_short sin_port;
    struct in_addr sin_addr;
    char sin_zero[8];
};

static_assert(sizeof(sockaddr) == 16 && sizeof(sockaddr_in) == 16);

// Windows error codes (winerror.h); the BSD errno values they come from have the same numbers less 10000.
#define WSAEINTR 10004L
#define WSAEBADF 10009L
#define WSAEACCES 10013L
#define WSAEFAULT 10014L
#define WSAEINVAL 10022L
#define WSAEMFILE 10024L
#define WSAEWOULDBLOCK 10035L
#define WSAEINPROGRESS 10036L
#define WSAEALREADY 10037L
#define WSAENOTSOCK 10038L
#define WSAEDESTADDRREQ 10039L
#define WSAEMSGSIZE 10040L
#define WSAEPROTOTYPE 10041L
#define WSAENOPROTOOPT 10042L
#define WSAEPROTONOSUPPORT 10043L
#define WSAESOCKTNOSUPPORT 10044L
#define WSAEOPNOTSUPP 10045L
#define WSAEPFNOSUPPORT 10046L
#define WSAEAFNOSUPPORT 10047L
#define WSAEADDRINUSE 10048L
#define WSAEADDRNOTAVAIL 10049L
#define WSAENETDOWN 10050L
#define WSAENETUNREACH 10051L
#define WSAENETRESET 10052L
#define WSAECONNABORTED 10053L
#define WSAECONNRESET 10054L
#define WSAENOBUFS 10055L
#define WSAEISCONN 10056L
#define WSAENOTCONN 10057L
#define WSAESHUTDOWN 10058L
#define WSAETIMEDOUT 10060L
#define WSAECONNREFUSED 10061L
#define WSAENAMETOOLONG 10063L
#define WSAEHOSTDOWN 10064L
#define WSAEHOSTUNREACH 10065L
#define WSAVERNOTSUPPORTED 10092L
#define WSANOTINITIALISED 10093L

// Version 2.2 is the only one there is to ask for. socket() fails with
// WSANOTINITIALISED until WSAStartup has succeeded.
int WSAStartup(WORD versionRequested, LPWSADATA data);
int WSACleanup();
int WSAGetLastError();

SOCKET socket(int family, int type, int protocol);
int bind(SOCKET s, const struct sockaddr* name, int nameLength);
int listen(SOCKET s, int backlog);
SOCKET accept(SOCKET s, struct sockaddr* addr, int* addrLength);
int recv(SOCKET s, char* buf, int length, int flags);
int send(SOCKET s, const char* buf, int length, int flags);
int closesocket(SOCKET s);

// htons, ntohs, htonl and ntohl are the system's (<sys/_endian.h>).
