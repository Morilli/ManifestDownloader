#ifndef SOCKET_UTILS_H
#define SOCKET_UTILS_H

#ifdef _WIN32
    #include <winsock2.h>
#endif
#include <inttypes.h>
#include "BearSSL/inc/bearssl_ssl.h"

#include "rman.h"

#ifndef _WIN32
    #define closesocket(socket) close(socket)
    typedef int SOCKET;
#endif

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
    br_ssl_client_context ssl_client_context;
    uint8_t* io_buffer;
    br_sslio_context ssl_io_context;
    br_x509_minimal_context x509_client_context;
};

SOCKET __attribute__((warn_unused_result)) open_connection_s(const char* ip, const char* port);
SOCKET __attribute__((warn_unused_result)) open_connection(uint32_t ip, uint16_t port);

uint8_t** download_ranges(struct ssl_data* ssl_structs, char* url, ChunkList* chunks);

HostPort* get_host_port(const char* url);

HttpResponse* download_url(char* url);

int send_wrapper(void* client_context, const uint8_t* data, size_t length);

int recv_wrapper(void* client_context, uint8_t* buffer, size_t length);

#endif
