#ifndef _socket_utils_H
#define _socket_utils_H

#include <inttypes.h>


int download_file(char* url, char* path);

int send_data(int socket, char* data, size_t length);

int receive_data(int socket, char* buffer, size_t length);

#endif
