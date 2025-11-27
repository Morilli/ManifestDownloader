#ifndef SOCKET_UTILS_H
#define SOCKET_UTILS_H

#ifdef _WIN32
    #include <winsock2.h>
#endif
#include <inttypes.h>
#include <wolfssl/ssl.h>

#include "rman.h"

#ifndef _WIN32
    #define closesocket(socket) close(socket)
    typedef int SOCKET;
#endif

extern WOLFSSL_CTX* ctx;

typedef struct http_response {
    int status_code;
    uint32_t length;
    uint8_t* data;
} HttpResponse;

typedef struct host_port {
    char* host;
    const char* port;
    int path_offset;
} HostPort;

struct ssl_data {
    SOCKET socket;
    HostPort* host_port;
    WOLFSSL* ssl;
};

SOCKET __attribute__((warn_unused_result)) open_connection_s(const char* ip, const char* port);
SOCKET __attribute__((warn_unused_result)) open_connection(uint32_t ip, uint16_t port);

uint8_t** get_ranges(const char* path, const ChunkList* chunks);
uint8_t** download_ranges(struct ssl_data* ssl_structs, const char* url, const ChunkList* chunks);

HostPort* get_host_port(const char* url);

HttpResponse* download_url(const char* url);

#endif
