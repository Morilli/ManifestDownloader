#ifndef _socket_utils_H
#define _socket_utils_H

#include <inttypes.h>
#include "rman.h"
#include "general_utils.h"

int __attribute__((warn_unused_result)) open_connection_s(char* ip, char* port);
int __attribute__((warn_unused_result)) open_connection(uint32_t ip, uint16_t port);

#ifndef _WIN32
    #define closesocket(socket) close(socket)
#endif

uint8_t** download_ranges(int* socket, char* url, ChunkList* chunks, int thread_id);

char* get_host(char* url, int* host_end);

BinaryData* download_url(char* url);

int send_data(int socket, char* data, size_t length);

int receive_data(int socket, char* buffer, size_t length);

#endif
