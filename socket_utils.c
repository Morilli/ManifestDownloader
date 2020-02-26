#ifndef _WIN32
    #define _GNU_SOURCE
    #include <sys/socket.h>
    #include <arpa/inet.h>
    #include <sys/types.h>
    #include <unistd.h>
    #include <netdb.h>
#else
    #include <winsock2.h>
    #include <ws2tcpip.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#include "socket_utils.h"
#include "general_utils.h"

int __attribute__((warn_unused_result)) open_connection_s(char* ip, char* port)
{
    struct addrinfo* addrinfos;
    if (getaddrinfo(ip, port, &(struct addrinfo) {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, &addrinfos) != 0) {
        fprintf(stderr, "Error: getaddrinfo failed.\n");
        printf("error code: %d\n", getaddrinfo(ip, port, &(struct addrinfo) {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, &addrinfos));   
        exit(EXIT_FAILURE);
    }

    struct addrinfo* _addrinfo;
    int socket_fd;
    for (_addrinfo = addrinfos; _addrinfo != NULL; _addrinfo = _addrinfo->ai_next)
    {
        //Create socket
        if ((socket_fd = socket(_addrinfo->ai_family, _addrinfo->ai_socktype, _addrinfo->ai_protocol)) == -1)
        {
            fprintf(stderr, "WARNING: Failed to create socket!");
            continue;
        }

        if (connect(socket_fd, addrinfos->ai_addr, addrinfos->ai_addrlen) != 0) {
            fprintf(stderr, "Failed to connect socket.\n");
            exit(EXIT_FAILURE);
        }

        break;
    }

    if (_addrinfo == NULL)
    {
        fprintf(stderr, "ERROR: Failed to get a working connection.\n");
        exit(EXIT_FAILURE);
    }

    freeaddrinfo(addrinfos);

    return socket_fd;

}
int __attribute__((warn_unused_result)) open_connection(uint32_t ip, uint16_t port)
{
    char port_string[6];
    sprintf(port_string, "%u", port);

    struct in_addr* formatted_ip = (struct in_addr*) &ip;
    return open_connection_s(inet_ntoa(*formatted_ip), port_string);
}


int send_data(int socket, char* data, size_t length)
{
    size_t bytes_sent_total = 0;
    while (bytes_sent_total != length) {
        ssize_t bytes_sent = send(socket, &data[bytes_sent_total], length - bytes_sent_total, 0);
        if (bytes_sent <= 0)
        {
            eprintf("Error: %s\n", strerror(errno));
            return -1;
        }

        bytes_sent_total += bytes_sent;
    }
    return 0;
}

int receive_data(int socket, char* buffer, size_t length)
{
    size_t total_received = 0;
    while (total_received != length)
    {
        ssize_t received = recv(socket, &buffer[total_received], length - total_received, 0);
        if (received <= 0) {
            eprintf("Error: %s\n", strerror(errno));
            return -1;
        }
        total_received += received;
    }
    return 0;
}

int download_file(char* url, char* path)
{
    printf("file to download: \"%s\"\n", url);
    char* host = url;
    if (strncmp(url, "https://", 8) == 0) {
        host += 8;
    }
    char* url_path = strstr(host, "/") + 1;
    *(url_path-1) = '\0';
    // printf("url here: \"%s\"\n", url);
    // printf("host here: \"%s\"\n", host);
    int socket = open_connection_s(host, "80");
    char request_header[1024];
    assert(sprintf(request_header, "GET /%s HTTP/1.1\r\nHost: %s\r\n\r\n", url_path, host) < 1024);
    *(url_path-1) = '/';
    // printf("request header:\n\"%s\"\n", request_header);
    send_data(socket, request_header, strlen(request_header));
    // printf("return value: %d\n", receive_data(socket, buffer, 1024));
    char* buffer = calloc(8192, 1);
    int received = recv(socket, buffer, 8192, 0);
    // printf("received header:\n\"%s\"\n", buffer);
    char* pos = strstr(buffer, "Content-Length:") + 16;
    long content_length = strtol(pos, NULL, 10);
    // printf("content length? %ld\n\n", content_length);
    pos = strstr(pos, "\r\n\r\n") + 4;
    char* response = malloc(content_length);
    int already_received = received - (pos - buffer);
    // printf("already received? %d, but the amount of bytes i got was %d\n", already_received, received);
    memcpy(response, pos, already_received);
    free(buffer);
    // printf("started receiving all data...\n");
    receive_data(socket, &response[already_received], content_length - already_received);
    // printf("finished receiving all data.\n");
    #ifdef _WIN32
        closesocket(socket);
    #else
        close(socket);
    #endif
    FILE* output = fopen(path, "wb");
    if (!output) {
        fprintf(stderr, "Error: Couldn't open output file \"%s\".\n", path);
        exit(EXIT_FAILURE);
    }
    fwrite(response, content_length, 1, output);
    fclose(output);
    free(response);
    
    return 0;
}

