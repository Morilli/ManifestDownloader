#ifndef SOCKET_UTILS_H
#define SOCKET_UTILS_H

#include <inttypes.h>
#ifdef _WIN32
    #include <winsock2.h>
#endif

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

SOCKET __attribute__((warn_unused_result)) open_connection_s(char* ip, char* port);
SOCKET __attribute__((warn_unused_result)) open_connection(uint32_t ip, uint16_t port);

uint8_t** download_ranges(SOCKET* socket, char* url, ChunkList* chunks);

char* get_host(char* url, int* host_end);

HttpResponse* download_url(char* url);

int send_data(SOCKET socket, char* data, size_t length);

int receive_data(SOCKET socket, char* buffer, size_t length);

#endif
