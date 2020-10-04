#ifndef SOCKET_UTILS_H
#define SOCKET_UTILS_H

#include <inttypes.h>
#ifdef _WIN32
    #include <winsock2.h>
#endif
#include "bearssl/bearssl_ssl.h"

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

struct ssl_data {
    br_ssl_client_context ssl_client_context;
    uint8_t* io_buffer;
    br_sslio_context ssl_io_context;
    br_x509_minimal_context x509_client_context;
    SOCKET socket;
};

SOCKET __attribute__((warn_unused_result)) open_connection_s(char* ip, char* port);
SOCKET __attribute__((warn_unused_result)) open_connection(uint32_t ip, uint16_t port);

uint8_t** download_ranges(struct ssl_data* ssl_structs, char* url, ChunkList* chunks);

char* get_host(char* url, int* host_end);

HttpResponse* download_url(char* url);

int send_wrapper(void* client_context, const uint8_t* data, size_t length);

int recv_wrapper(void* client_context, uint8_t* buffer, size_t length);

#endif
