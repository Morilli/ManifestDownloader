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
#include "defs.h"
#include "rman.h"
#include "general_utils.h"


int __attribute__((warn_unused_result)) open_connection_s(char* ip, char* port)
{
    struct addrinfo* addrinfos;
    if (getaddrinfo(ip, port, &(struct addrinfo) {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM}, &addrinfos) != 0) {
        eprintf("Error: getaddrinfo failed.\n");
        eprintf("error code: %d\n", getaddrinfo(ip, port, &(struct addrinfo) {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM}, &addrinfos));
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

char* get_host(char* url, int* host_end)
{
    char* start_of_host = url;
    if (strncmp(url, "https://", 8) == 0) {
        start_of_host += 8;
    }
    char* end_of_host = strstr(start_of_host, "/");
    if (!end_of_host)
        return NULL;
    int string_length = end_of_host - start_of_host;
    char* host = malloc(string_length + 1);
    memcpy(host, start_of_host, string_length);
    host[string_length] = '\0';

    if (host_end)
        *host_end = end_of_host - url;
    return host;
}

BinaryData* receive_http_body(int* socket, char* request, char* host, int thread_id)
{
    int retries = 10;
    send_data(*socket, request, strlen(request));
    char* header_buffer = calloc(8193, 1);
    int received = recv(*socket, header_buffer, 8192, 0);
    dprintf("%d: received header:\n\"%s\"\n", thread_id, header_buffer);
    bool refresh = strstr(header_buffer, "Connection: close\r\n");
    while (strncmp(header_buffer, "HTTP/1.1 404", 12) == 0 && retries --> 0) {
        dprintf("Got a 404. Will retry %d time%s.\n", retries + 1, retries > 0 ? "s" : "");
        if (refresh) {
            closesocket(*socket);
            *socket = open_connection_s(host, "80");
        }
        send_data(*socket, request, strlen(request));
        received = recv(*socket, header_buffer, 8192, 0);
        refresh = strstr(header_buffer, "Connection: close\r\n");
    }
    if (strncmp(header_buffer, "HTTP/1.1 404", 12) == 0) {
        dprintf("Retries failed. Will retry one last time with a new connection.\n");
        closesocket(*socket);
        *socket = open_connection_s(host, "80");
        send_data(*socket, request, strlen(request));
        received = recv(*socket, header_buffer, 8192, 0);
        refresh = strstr(header_buffer, "Connection: close\r\n");
    }
    if (strncmp(header_buffer, "HTTP/1.1 404", 12) == 0) {
        eprintf("Got too many 404s. Can't continue.\n");
        return NULL;
    } else if (strncmp(header_buffer, "HTTP/1.1 4", 10) == 0 || strncmp(header_buffer, "HTTP/1.1 5", 10) == 0) {
        eprintf("Error: Got a %c%c%c response.\n", header_buffer[9], header_buffer[10], header_buffer[11]);
        return NULL;
    }
    char* start_of_body = strstr(header_buffer, "\r\n\r\n") + 4;
    char* content_length_position = strstr(header_buffer, "Content-Length:");
    int already_received = received - (start_of_body - header_buffer);
    BinaryData* body = malloc(sizeof(BinaryData));
    if (content_length_position) {
        body->length = strtol(content_length_position + 16, NULL, 10);
        body->data = malloc(body->length);
        memcpy(body->data, start_of_body, already_received);
        free(header_buffer);
        dprintf("%d: already received %d, will try to receive the rest %"PRIu64"\n", thread_id, already_received, body->length - already_received);
        receive_data(*socket, (char*) &body->data[already_received], body->length - already_received);
    } else {
        assert(refresh);
        body->length = already_received;
        uint64_t buffer_size = 8192 + (8192 >> 1);
        body->data = malloc(buffer_size);
        memcpy(body->data, start_of_body, already_received);
        free(header_buffer);
        while ( (received = recv(*socket, (char*) &body->data[body->length], buffer_size - body->length, 0)) != 0) {
            body->length += received;
            if (body->length == buffer_size) {
                buffer_size += buffer_size >> 1;
                body->data = realloc(body->data, buffer_size);
            }
        }
    }
    if (refresh) {
        closesocket(*socket);
        *socket = open_connection_s(host, "80");
    }
    return body;
}

uint8_t** download_ranges(int* socket, char* url, ChunkList* chunks, int thread_id)
{
    int host_end;
    char* host = get_host(url, &host_end);
    char request_header[8192];
    uint32_list range_indices;
    initialize_list(&range_indices);
    add_object(&range_indices, &(uint32_t) {0});
    uint32_t last_chunk = 0;
    uint32_t last_range = 0;
    sprintf(request_header, "GET %s HTTP/1.1\r\nHost: %s\r\nRange: bytes=", url + host_end, host);
    char range[26];
    for (uint32_t i = 1; i < chunks->length; i++) {
        if (chunks->objects[i-1].bundle_offset + chunks->objects[i-1].compressed_size == chunks->objects[i].bundle_offset || chunks->objects[i-1].bundle_offset == chunks->objects[i].bundle_offset ) {
            add_object(&range_indices, &(uint32_t) {last_range});
        } else {
            sprintf(range, "%u-%u,", chunks->objects[last_chunk].bundle_offset, chunks->objects[i-1].bundle_offset + chunks->objects[i-1].compressed_size - 1);
            strcat(request_header, range);
            last_range++;
            last_chunk = i;
            add_object(&range_indices, &(uint32_t) {last_range});
        }
    }
    sprintf(range, "%u-%u\r\n\r\n", chunks->objects[last_chunk].bundle_offset, chunks->objects[chunks->length-1].bundle_offset + chunks->objects[chunks->length-1].compressed_size - 1);
    strcat(request_header, range);
    assert(strlen(request_header) < 8192);
    dprintf("%d: requesting %d chunk%s\n", thread_id, chunks->length, chunks->length > 1 ? "s" : "");
    dprintf("%d: request header:\n\"%s\"\n", thread_id, request_header);
    BinaryData* body = receive_http_body(socket, request_header, host, thread_id);
    if (!body)
        return NULL;
    uint8_t** ranges = malloc(chunks->length * sizeof(char*));
    if (chunks->length == 1) {
        ranges[0] = body->data;
        free(body);
    } else {
        char* pos = (char*) body->data;
        if (range_indices.objects[range_indices.length-1] != 0)
            pos = strstr(pos, "\r\n\r\n") + 4;
        for (uint32_t i = 0; i < chunks->length; i++) {
            if (i != 0 && range_indices.objects[i] > range_indices.objects[i-1])
                pos = strstr(pos, "\r\n\r\n") + 4;
            ranges[i] = malloc(chunks->objects[i].compressed_size);
            memcpy(ranges[i], pos, chunks->objects[i].compressed_size);
            if (i != chunks->length-1 && chunks->objects[i+1].bundle_offset > chunks->objects[i].bundle_offset)
                pos += chunks->objects[i].compressed_size;
        }
        free(body->data);
        free(body);
    }
    free(range_indices.objects);
    free(host);

    return ranges;
}

BinaryData* download_url(char* url)
{
    int host_end;
    dprintf("file to download: \"%s\"\n", url);
    char* host = get_host(url, &host_end);
    if (!host)
        return NULL;
    int socket = open_connection_s(host, "80");
    char request_header[1024];
    assert(sprintf(request_header, "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", url + host_end, host) < 1024);
    BinaryData* data = receive_http_body(&socket, request_header, host, 0);
    free(host);
    closesocket(socket);

    return data;
}
