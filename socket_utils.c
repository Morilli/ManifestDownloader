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
#include "bearssl/bearssl_ssl.h"
#include "bearssl/trust_anchors.h"

#include "socket_utils.h"
#include "defs.h"
#include "list.h"
#include "rman.h"

SOCKET __attribute__((warn_unused_result)) open_connection_s(const char* ip, const char* port)
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

        if (connect(socket_fd, _addrinfo->ai_addr, _addrinfo->ai_addrlen) != 0) {
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


int send_wrapper(void* client_context, const uint8_t* data, size_t length)
{
    int bytes_sent = send(*(SOCKET*) client_context, (const char*) data, length, 0);
    if (bytes_sent <= 0) {
        return -1;
    }
    return bytes_sent;
}

int recv_wrapper(void* client_context, uint8_t* buffer, size_t length)
{
    int received = recv(*(SOCKET*) client_context, (char*) buffer, length, 0);
    if (received <= 0) {
        return -1;
    }
    return received;
}

int send_data(void* socket, const uint8_t* data, size_t length)
{
    int bytes_sent;
    if ( (bytes_sent = send(*(SOCKET*) socket, (const char*) data, length, 0)) <= 0) {
        eprintf("Error: %s\n", bytes_sent == 0 ? "Socket was disconnected unexpectedly." : strerror(errno));
        return -1;
    }
    return 0;
}

int receive_data(void* socket, uint8_t* buffer, size_t length)
{
    size_t total_received = 0;
    while (total_received != length)
    {
        ssize_t received = recv(*(SOCKET*) socket, (char*) &buffer[total_received], length - total_received, 0);
        if (received <= 0) {
            eprintf("Error: %s\n", received == 0 ? "Socket was disconnected unexpectedly." : strerror(errno));
            return -1;
        }
        total_received += received;
    }
    return 0;
}

HostPort* get_host_port(const char* url)
{
    HostPort* host_port = malloc(sizeof(HostPort));
    const char* start_of_host = url;
    if (strncmp(url, "https://", 8) == 0) {
        start_of_host += 8;
        host_port->port = "443";
    } else if (strncmp(url, "http://", 7) == 0) {
        start_of_host += 7;
        host_port->port = "80";
    } else {
        host_port->port = "80";
    }
    char* host_end = strstr(start_of_host, "/");
    if (!host_end) {
        free(host_port);
        return NULL;
    }
    int host_length = host_end - start_of_host;
    host_port->host = malloc(host_length + 1);
    memcpy(host_port->host, start_of_host, host_length);
    host_port->host[host_length] = '\0';
    host_port->path_offset = host_end - url;

    return host_port;
}

HttpResponse* receive_http_body(struct ssl_data* ssl_structs, char* request)
{
    // dynamic function pointers; based on whether ssl functions or normal socket functions should be used
    void* io_context = &ssl_structs->ssl_io_context;
    int (*write_all)() = br_sslio_write_all;
    int (*recv_once)() = br_sslio_read;
    int (*recv_all)() = br_sslio_read_all;
    bool is_ssl = strcmp(ssl_structs->host_port->port, "443") == 0;
    if (!is_ssl) {
        io_context = &ssl_structs->socket;
        write_all = send_data;
        recv_once = recv_wrapper;
        recv_all = receive_data;
    }
    char* header_buffer = calloc(8193, 1);
    write_all(io_context, request, strlen(request));
    if (is_ssl) {
        br_sslio_flush(io_context);
        int last_error = br_ssl_engine_last_error(&ssl_structs->ssl_client_context.eng);
        if (last_error == BR_ERR_X509_NOT_TRUSTED) {
            eprintf("Error: Digicert certificate not valid for this server.\n");
            exit(EXIT_FAILURE);
        } else if (last_error != BR_ERR_OK) {
            eprintf("bearssl engine reported error no. %d\n", last_error);
            exit(EXIT_FAILURE);
        }
    }
    // assume the entire header can be received in one recv call
    int received = recv_once(io_context, header_buffer, 8192);
    dprintf("received header:\n\"%s\"\n", header_buffer);
    bool refresh = strstr(header_buffer, "Connection: close\r\n");
    char* status_code = header_buffer + 9;

    char* start_of_body = strstr(header_buffer, "\r\n\r\n") + 4;
    char* content_length_position = strstr(header_buffer, "Content-Length:");
    int already_received = received - (start_of_body - header_buffer);
    HttpResponse* body = calloc(1, sizeof(HttpResponse));
    body->status_code = strtol(status_code, NULL, 10);
    if (content_length_position) {
        // header contained the Content-Length: header, so I know how many bytes to receive
        body->length = strtoumax(content_length_position + 15, NULL, 10);
        body->data = malloc(body->length);
        memcpy(body->data, start_of_body, already_received);
        dprintf("already received %d, will try to receive the rest %u\n", already_received, body->length - already_received);
        recv_all(io_context, &body->data[already_received], body->length - already_received);
    } else if (strstr(header_buffer, "Transfer-Encoding: chunked")) {
        // header contained the transfer-encoding: chunked header, which is difficult to handle (no content-length)
        char* start_of_chunk = start_of_body;
        char chunk_size_buffer[32] = {0};
        while (1) {
            if (!strstr(start_of_chunk, "\r\n")) {
                strcpy(chunk_size_buffer, start_of_chunk);
                start_of_chunk = chunk_size_buffer;
                already_received += recv_once(io_context, &start_of_chunk[already_received], 31 - already_received);
            }
            int chunk_size = strtol(start_of_chunk, NULL, 16);
            if (!chunk_size) // chunk_size == 0, last chunk
                break;
            body->data = realloc(body->data, body->length + chunk_size);
            char* body_position = strstr(start_of_chunk, "\r\n") + 2;
            already_received -= body_position - start_of_chunk;
            if (already_received >= chunk_size + 2) {
                memcpy(&body->data[body->length], body_position, chunk_size);
                start_of_chunk = body_position + chunk_size + 2;
                already_received -= chunk_size + 2;
            } else {
                memcpy(&body->data[body->length], body_position, already_received);
                recv_all(io_context, &body->data[body->length + already_received], chunk_size - already_received);
                recv_all(io_context, &(uint16_t) {0}, 2);
                start_of_chunk = body_position + already_received;
                already_received = 0;
            }
            body->length += chunk_size;
        }
        if (!strstr(start_of_chunk + 3, "\r\n")) {
            // in the rare case the final chunk size (0) was received, but not the last \r\n (should never happen)
            recv_once(io_context, &(uint64_t) {0}, 8); // assume we get everything here
        }
    } else {
        // no content-length field, so there is no way to know everything was received
        // therefor, ensure Connection: close was given
        assert(refresh);
        body->length = already_received;
        uint64_t buffer_size = 8192 + (8192 >> 1);
        body->data = malloc(buffer_size);
        memcpy(body->data, start_of_body, already_received);
        while ( (received = recv_once(io_context, &body->data[body->length], buffer_size - body->length)) != -1) {
            body->length += received;
            if (body->length == buffer_size) {
                buffer_size += buffer_size >> 1;
                body->data = realloc(body->data, buffer_size);
            }
        }
        if (is_ssl) {
            int last_error = br_ssl_engine_last_error(&ssl_structs->ssl_client_context.eng);
            if (last_error != 0) {
                eprintf("bearssl engine reported error no. %d\n", last_error);
                exit(EXIT_FAILURE);
            }
        }
        else assert(recv(*(SOCKET*) io_context, &(char) {0}, 1, 0) == 0);
    }
    free(header_buffer);
    if (refresh) {
        closesocket(ssl_structs->socket);
        ssl_structs->socket = open_connection_s(ssl_structs->host_port->host, ssl_structs->host_port->port);
        if (is_ssl) {
            br_ssl_client_reset(&ssl_structs->ssl_client_context, ssl_structs->host_port->host, 1);
            br_sslio_init(&ssl_structs->ssl_io_context, &ssl_structs->ssl_client_context.eng, recv_wrapper, &ssl_structs->socket, send_wrapper, &ssl_structs->socket);
        }
    }
    return body;
}

uint8_t** download_ranges(struct ssl_data* ssl_structs, char* url, ChunkList* chunks)
{
    char request_header[8192];
    uint32_list range_indices;
    initialize_list(&range_indices);
    add_object(&range_indices, &(uint32_t) {0});
    uint32_t last_chunk = 0;
    uint32_t last_range = 0;
    sprintf(request_header, "GET %s HTTP/1.1\r\nHost: %s\r\nRange: bytes=", url + ssl_structs->host_port->path_offset, ssl_structs->host_port->host);
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
    HttpResponse* body = receive_http_body(ssl_structs, request_header);
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

    return ranges;
}

HttpResponse* download_url(char* url)
{
    dprintf("file to download: \"%s\"\n", url);
    struct ssl_data ssl_structs;
    ssl_structs.host_port = get_host_port(url);
    if (!ssl_structs.host_port)
        return NULL;
    bool is_ssl = strcmp(ssl_structs.host_port->port, "443") == 0;
    ssl_structs.socket = open_connection_s(ssl_structs.host_port->host, ssl_structs.host_port->port);
    if (is_ssl) {
        ssl_structs.io_buffer = malloc(BR_SSL_BUFSIZE_BIDI);
        br_ssl_client_init_full(&ssl_structs.ssl_client_context, &ssl_structs.x509_client_context, TAs, TAs_NUM);
        br_ssl_engine_set_buffer(&ssl_structs.ssl_client_context.eng, ssl_structs.io_buffer, BR_SSL_BUFSIZE_BIDI, 1);
        br_ssl_client_reset(&ssl_structs.ssl_client_context, ssl_structs.host_port->host, 0);
        br_sslio_init(&ssl_structs.ssl_io_context, &ssl_structs.ssl_client_context.eng, recv_wrapper, &ssl_structs.socket, send_wrapper, &ssl_structs.socket);
    }
    char request_header[1024];
    assert(sprintf(request_header, "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", url + ssl_structs.host_port->path_offset, ssl_structs.host_port->host) < 1024);
    HttpResponse* data = receive_http_body(&ssl_structs, request_header);
    free(ssl_structs.host_port->host);
    free(ssl_structs.host_port);
    closesocket(ssl_structs.socket);
    if (is_ssl) free(ssl_structs.io_buffer);

    return data;
}
