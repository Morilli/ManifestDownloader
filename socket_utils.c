#ifndef _WIN32
    #define _GNU_SOURCE
    #include <sys/socket.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <netdb.h>
#else
    #include <winsock2.h>
    #include <ws2tcpip.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#include "socket_utils.h"
#include "defs.h"
#include "list.h"
#include "rman.h"

SOCKET __attribute__((warn_unused_result)) open_connection_s(char* ip, char* port)
{
    struct addrinfo* addrinfos;
    int error;
    if ( (error = getaddrinfo(ip, port, &(struct addrinfo) {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM}, &addrinfos)) != 0) {
        eprintf("Error: getaddrinfo failed.\n");
        eprintf("error string: \"%s\"\n", gai_strerror(error));
        exit(EXIT_FAILURE);
    }

    struct addrinfo* _addrinfo;
    SOCKET socket_fd;
    for (_addrinfo = addrinfos; _addrinfo != NULL; _addrinfo = _addrinfo->ai_next)
    {
        //Create socket
        if ((socket_fd = socket(_addrinfo->ai_family, _addrinfo->ai_socktype, _addrinfo->ai_protocol)) == (SOCKET) -1)
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
SOCKET __attribute__((warn_unused_result)) open_connection(uint32_t ip, uint16_t port)
{
    char port_string[6];
    sprintf(port_string, "%u", port);

    struct in_addr* formatted_ip = (struct in_addr*) &ip;
    return open_connection_s(inet_ntoa(*formatted_ip), port_string);
}


int send_data(SOCKET socket, char* data, size_t length)
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

int receive_data(SOCKET socket, char* buffer, size_t length)
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
    } else if (strncmp(url, "http://", 7) == 0) {
        start_of_host += 7;
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

HttpResponse* receive_http_body(SOCKET* socket, char* request, char* host)
{
    char* header_buffer = calloc(8193, 1);
    send_data(*socket, request, strlen(request));
    int received = recv(*socket, header_buffer, 8192, 0);
    if (!received) {
        eprintf("Sent data, but received nothing! Please report this.\n");
    }
    dprintf("received header:\n\"%s\"\n", header_buffer);
    bool refresh = strstr(header_buffer, "Connection: close\r\n");
    char* status_code = header_buffer + 9;

    char* start_of_body = strstr(header_buffer, "\r\n\r\n") + 4;
    char* content_length_position = strstr(header_buffer, "Content-Length:");
    int already_received = received - (start_of_body - header_buffer);
    HttpResponse* body = calloc(1, sizeof(HttpResponse));
    body->status_code = strtol(status_code, NULL, 10);
    if (content_length_position) {
        body->length = strtoumax(content_length_position + 15, NULL, 10);
        body->data = malloc(body->length);
        memcpy(body->data, start_of_body, already_received);
        dprintf("already received %d, will try to receive the rest %u\n", already_received, body->length - already_received);
        receive_data(*socket, (char*) &body->data[already_received], body->length - already_received);
    } else if (strstr(header_buffer, "Transfer-Encoding: chunked")) {
        char* start_of_chunk = start_of_body;
        char chunk_size_buffer[32] = {0};
        while (1) {
            if (!strstr(start_of_chunk, "\r\n")) {
                strcpy(chunk_size_buffer, start_of_chunk);
                start_of_chunk = chunk_size_buffer;
                already_received += recv(*socket, &start_of_chunk[already_received], 31 - already_received, 0);
            }
            int chunk_size = strtol(start_of_chunk, NULL, 16);
            if (!chunk_size)
                break;
            body->data = realloc(body->data, body->length + chunk_size);
            char* body_position = strstr(start_of_chunk, "\r\n") + 2;
            already_received -= body_position - start_of_chunk;
            if (already_received >= chunk_size + 2) {
                memcpy(&body->data[body->length], body_position, chunk_size);
                body->length += chunk_size;
                already_received -= chunk_size + 2;
            } else {
                memcpy(&body->data[body->length], body_position, already_received);
                body->length += already_received;
                receive_data(*socket, (char*) &body->data[body->length], chunk_size - already_received);
                body->length += chunk_size - already_received;
                receive_data(*socket, (char*) &(uint16_t) {0}, 2);
                already_received = 0;
            }
            start_of_chunk = body_position + chunk_size + 2;
        }
        while (!strstr(start_of_chunk + 3, "\r\n")) recv(*socket, (char*) &(uint64_t) {0}, 8, 0);
    } else {
        assert(refresh);
        body->length = already_received;
        uint64_t buffer_size = 8192 + (8192 >> 1);
        body->data = malloc(buffer_size);
        memcpy(body->data, start_of_body, already_received);
        while ( (received = recv(*socket, (char*) &body->data[body->length], buffer_size - body->length, 0)) != 0) {
            body->length += received;
            if (body->length == buffer_size) {
                buffer_size += buffer_size >> 1;
                body->data = realloc(body->data, buffer_size);
            }
        }
    }
    free(header_buffer);
    if (refresh) {
        closesocket(*socket);
        *socket = open_connection_s(host, "80");
    }
    return body;
}

uint8_t** download_ranges(SOCKET* socket, char* url, ChunkList* chunks)
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
    dprintf("requesting %d chunk%s\n", chunks->length, chunks->length > 1 ? "s" : "");
    dprintf("request header:\n\"%s\"\n", request_header);
    HttpResponse* body = receive_http_body(socket, request_header, host);
    dprintf("status code: %d\n", body->status_code);
    if (body->status_code >= 400) {
        eprintf("Error: Got a %d response.\n", body->status_code);
        return NULL;
    }
    uint8_t** ranges = malloc(chunks->length * sizeof(char*));
    if (body->status_code == 200) { // got the entire bundle instead of just the ranges (note: this is rare and i'm not sure why it happens)
        for (uint32_t i = 0; i < chunks->length; i++) {
            ranges[i] = malloc(chunks->objects[i].compressed_size);
            memcpy(ranges[i], &body->data[chunks->objects[i].bundle_offset], chunks->objects[i].compressed_size);
        }
        free(body->data);
    } else if (chunks->length == 1) {
        ranges[0] = body->data;
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
    }
    free(body);
    free(range_indices.objects);
    free(host);

    return ranges;
}

HttpResponse* download_url(char* url)
{
    int host_end;
    dprintf("file to download: \"%s\"\n", url);
    char* host = get_host(url, &host_end);
    if (!host)
        return NULL;
    SOCKET socket = open_connection_s(host, "80");
    char request_header[1024];
    assert(sprintf(request_header, "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", url + host_end, host) < 1024);
    HttpResponse* data = receive_http_body(&socket, request_header, host);
    free(host);
    closesocket(socket);

    return data;
}
