#ifndef _WIN32
    #define _GNU_SOURCE
    #include <sys/socket.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <netdb.h>
#else
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <shlwapi.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include "BearSSL/inc/bearssl_ssl.h"
#include "BearSSL/trust_anchors.h"

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
            eprintf("WARNING: Failed to create socket!");
            continue;
        }

        if (connect(socket_fd, _addrinfo->ai_addr, _addrinfo->ai_addrlen) != 0) {
            eprintf("WARNING: Failed to connect socket.\n");
            continue;
        }

        break;
    }

    if (_addrinfo == NULL)
    {
        eprintf("ERROR: Failed to get a working connection.\n");
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

static int send_data(void* socket, const void* data, size_t length)
{
    int bytes_sent;
    if ( (bytes_sent = send(*(SOCKET*) socket, (const char*) data, length, 0)) <= 0) {
        eprintf("Error: %s\n", bytes_sent == 0 ? "Socket was disconnected unexpectedly." : strerror(errno));
        return -1;
    }
    return 0;
}

static int receive_data(void* socket, void* buffer, size_t length)
{
    size_t total_received = 0;
    while (total_received != length)
    {
        ssize_t received = recv(*(SOCKET*) socket, &((char*) buffer)[total_received], length - total_received, 0);
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
    const char* host_end = strstr(start_of_host, "/");
    if (!host_end) {
        host_end = start_of_host + strlen(start_of_host);
    }
    int host_length = host_end - start_of_host;
    host_port->host = malloc(host_length + 1);
    memcpy(host_port->host, start_of_host, host_length);
    host_port->host[host_length] = '\0';
    host_port->path_offset = host_end - url;

    return host_port;
}

static void refresh_connection(struct ssl_data* ssl_structs, bool is_ssl)
{
    closesocket(ssl_structs->socket);
    ssl_structs->socket = open_connection_s(ssl_structs->host_port->host, ssl_structs->host_port->port);
    if (is_ssl) {
        br_ssl_client_reset(&ssl_structs->ssl_client_context, ssl_structs->host_port->host, 1);
        br_sslio_init(&ssl_structs->ssl_io_context, &ssl_structs->ssl_client_context.eng, recv_wrapper, &ssl_structs->socket, send_wrapper, &ssl_structs->socket);
    }
}

static int br_sslio_write_all_wrapper(void* cc, const void* src, size_t len) {return br_sslio_write_all(cc, src, len);}
static int br_sslio_read_wrapper(void* cc, void* dst, size_t len) {return br_sslio_read(cc, dst, len);}
static int br_sslio_read_all_wrapper(void* cc, void* dst, size_t len) {return br_sslio_read_all(cc, dst, len);}
static int nossl_recv_wrapper(void* client_context, void* buffer, size_t len) {return recv_wrapper(client_context, buffer, len);}

HttpResponse* receive_http_body(struct ssl_data* ssl_structs, const char* request)
{
    // dynamic function pointers; based on whether ssl functions or normal socket functions should be used
    void* io_context = &ssl_structs->ssl_io_context;
    int (*write_all)(void*, const void*, size_t) = br_sslio_write_all_wrapper;
    int (*recv_once)(void*, void*, size_t) = br_sslio_read_wrapper;
    int (*recv_all)(void*, void*, size_t) = br_sslio_read_all_wrapper;
    bool is_ssl = strcmp(ssl_structs->host_port->port, "443") == 0;
    if (!is_ssl) {
        io_context = &ssl_structs->socket;
        write_all = send_data;
        recv_once = nossl_recv_wrapper;
        recv_all = receive_data;
    }

    int success = write_all(io_context, request, strlen(request));
    if (is_ssl) {
        br_sslio_flush(io_context);
        int last_error = br_ssl_engine_last_error(&ssl_structs->ssl_client_context.eng);
        if (last_error == BR_ERR_X509_NOT_TRUSTED) {
            eprintf("Error: No certificate was valid for this server. Please report this.\n");
            exit(EXIT_FAILURE);
        } else if (last_error == BR_ERR_IO) { // assume socket was closed due to inactivity and try again
            eprintf("Info: Underlying connection was closed. Trying again...\n");
            refresh_connection(ssl_structs, is_ssl);
            return receive_http_body(ssl_structs, request);
        } else if (last_error != BR_ERR_OK) {
            eprintf("bearssl engine reported error no. %d\n", last_error);
            exit(EXIT_FAILURE);
        }
    } else if (success == -1) {
        eprintf("Attempting reconnection...\n");
        refresh_connection(ssl_structs, is_ssl);
        return receive_http_body(ssl_structs, request);
    }
    char header_buffer[8193] = {0};
    int received = 0;
    do {
        int bytes_read = recv_once(io_context, header_buffer + received, 8192 - received);
        if (bytes_read == -1) return NULL;
        received += bytes_read;
    } while (!strstr(header_buffer, "\r\n\r\n"));
    dprintf("received header:\n\"%s\"\n", header_buffer);
    bool refresh = strcasestr(header_buffer, "Connection: close\r\n");
    char* status_code = header_buffer + 9;

    char* start_of_body = strstr(header_buffer, "\r\n\r\n") + 4;
    char* content_length_position = strcasestr(header_buffer, "Content-Length:");
    int already_received = received - (start_of_body - header_buffer);
    HttpResponse* body = calloc(1, sizeof(HttpResponse));
    body->status_code = strtol(status_code, NULL, 10);
    if (content_length_position) {
        // header contained the Content-Length: header, so I know how many bytes to receive
        body->length = strtoumax(content_length_position + 15, NULL, 10);
        body->data = malloc(body->length);
        memcpy(body->data, start_of_body, already_received);
        dprintf("already received %d, will try to receive the rest %u\n", already_received, body->length - already_received);
        if (recv_all(io_context, &body->data[already_received], body->length - already_received) != 0) return NULL;
    } else if (strcasestr(header_buffer, "Transfer-Encoding: chunked")) {
        // header contained the transfer-encoding: chunked header, which is difficult to handle (no content-length)
        char* start_of_chunk = start_of_body;
        char chunk_size_buffer[32] = {0};
        while (1) {
            while (!strstr(start_of_chunk, "\r\n")) {
                strcpy(chunk_size_buffer, start_of_chunk);
                start_of_chunk = chunk_size_buffer;
                int received = recv_once(io_context, &start_of_chunk[already_received], 31 - already_received);
                if (received == -1) return NULL;
                already_received += received;
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
                if (recv_all(io_context, &body->data[body->length + already_received], chunk_size - already_received) != 0 ||
                    recv_all(io_context, &(uint16_t) {0}, 2) != 0) {
                    return NULL;
                }
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
            if (last_error == BR_ERR_IO) { // idfk
                eprintf("Info: I/O error occured while receiving data. Trying again...\n");
                refresh_connection(ssl_structs, is_ssl);
                return receive_http_body(ssl_structs, request);
            } else if (last_error != BR_ERR_OK) {
                eprintf("bearssl engine reported error no. %d\n", last_error);
                exit(EXIT_FAILURE);
            }
        }
        else assert(recv(*(SOCKET*) io_context, &(char) {0}, 1, 0) == 0);
    }
    if (refresh) refresh_connection(ssl_structs, is_ssl);
    return body;
}

uint8_t** get_ranges(const char* bundle_path, const ChunkList* chunks)
{
    FILE* bundle_file = fopen(bundle_path, "rb");
    if (!bundle_file) {
        eprintf("Error: Failed to open file \"%s\"\n", bundle_path);
        return NULL;
    }

    uint8_t** ranges = malloc(chunks->length * sizeof(uint8_t*));
    for (uint32_t i = 0; i < chunks->length; i++) {
        fseek(bundle_file, chunks->objects[i].bundle_offset, SEEK_SET);
        ranges[i] = malloc(chunks->objects[i].compressed_size);
        assert(fread(ranges[i], chunks->objects[i].compressed_size, 1, bundle_file) == 1);
    }

    fclose(bundle_file);
    return ranges;
}

static uint32_t response_to_ranges(HttpResponse* body, const ChunkList* chunks, uint8_t** ranges, uint32_t first_chunk, uint32_t count, uint32_list chunk_to_range_map)
{
    uint32_t chunks_handled = 0;

    if (body->status_code == 200) { // got the entire bundle instead of just the ranges (note: this is rare and i'm not sure why it happens)
        for (uint32_t i = first_chunk; i < chunks->length; i++) {
            ranges[i] = malloc(chunks->objects[i].compressed_size);
            memcpy(ranges[i], &body->data[chunks->objects[i].bundle_offset], chunks->objects[i].compressed_size);
        }
        free(body->data);
        chunks_handled = chunks->length - first_chunk;
    } else if (count == 1) {
        ranges[first_chunk] = body->data;
        chunks_handled = 1;
    } else {
        char* pos = (char*) body->data;
        // don't skip first range boundary delimiter if only one range was requested
        if (chunk_to_range_map.objects[first_chunk + count - 1] != chunk_to_range_map.objects[first_chunk])
            pos = strstr(pos, "\r\n\r\n") + 4;
        for (uint32_t i = first_chunk; i < first_chunk + count; i++) {
            if (i != first_chunk && chunk_to_range_map.objects[i] > chunk_to_range_map.objects[i-1])
                pos = strstr(pos, "\r\n\r\n") + 4;
            ranges[i] = malloc(chunks->objects[i].compressed_size);
            memcpy(ranges[i], pos, chunks->objects[i].compressed_size);
            if (i != first_chunk+count-1 && chunks->objects[i+1].bundle_offset > chunks->objects[i].bundle_offset)
                pos += chunks->objects[i].compressed_size;
        }
        free(body->data);
        chunks_handled = count;
    }
    free(body);

    return chunks_handled;
}

uint8_t** download_ranges(struct ssl_data* ssl_structs, const char* url, const ChunkList* chunks)
{
    uint32_list chunk_to_range_map;
    initialize_list_size(&chunk_to_range_map, chunks->length);
    add_object(&chunk_to_range_map, &(uint32_t) {0}); // first chunk is always at the beginning, so maps to the first range (0)
    uint32_t last_range = 0;
    for (uint32_t i = 1; i < chunks->length; i++) {
        if (chunks->objects[i-1].bundle_offset + chunks->objects[i-1].compressed_size != chunks->objects[i].bundle_offset && chunks->objects[i-1].bundle_offset != chunks->objects[i].bundle_offset) {
            last_range++;
        }
        add_object(&chunk_to_range_map, &(uint32_t) {last_range});
    }

    char request_header[8000];
    char current_range[23];
    uint32_t first_chunk = 0;
    uint8_t** ranges = malloc(chunks->length * sizeof(char*));

    while (first_chunk < chunks->length) {
        uint32_t chunk_count = 0;
        sprintf(request_header, "GET %s HTTP/1.1\r\nHost: %s\r\nRange: bytes=", url + ssl_structs->host_port->path_offset, ssl_structs->host_port->host);
        for (uint32_t i = first_chunk, range_start_chunk = first_chunk; i < chunks->length; i++) {
            if (i == chunks->length-1 || chunk_to_range_map.objects[i+1] != chunk_to_range_map.objects[i]) {
                sprintf(current_range, "%u-%u", chunks->objects[range_start_chunk].bundle_offset, chunks->objects[i].bundle_offset + chunks->objects[i].compressed_size - 1);
                range_start_chunk = i + 1;

                if (i == chunks->length - 1 // last chunk reached
                    || strlen(request_header) + strlen(current_range) > sizeof(request_header) - sizeof(current_range) - 4) { // no further range is guaranteed to fit in the header
                    strcat(request_header, current_range);
                    chunk_count = i - first_chunk + 1;
                    break;
                }

                strcat(current_range, ",");
                strcat(request_header, current_range);
            }
        }

        strcat(request_header, "\r\n\r\n");
        dprintf("requesting %d chunk%s\n", chunk_count, chunk_count > 1 ? "s" : "");
        dprintf("request header:\n\"%s\"\n", request_header);
        HttpResponse* body = receive_http_body(ssl_structs, request_header);
        dprintf("status code: %d\n", body->status_code);
        if (!body || body->status_code >= 400) {
            if (body)
                eprintf("Error: Got a %d response.\n", body->status_code);
            else {
                eprintf("Error: Failed to receive response data.\n");
                if (strcmp(ssl_structs->host_port->port, "443") == 0)
                    eprintf("Bearssl error: %d\n", br_ssl_engine_last_error(&ssl_structs->ssl_client_context.eng));
                else
                    eprintf("Error: %s\n", strerror(errno));
            }
            free(ranges);
            return NULL;
        }

        uint32_t chunks_handled = response_to_ranges(body, chunks, ranges, first_chunk, chunk_count, chunk_to_range_map);
        assert(chunks_handled >= chunk_count);
        first_chunk += chunks_handled;
    }

    free(chunk_to_range_map.objects);

    return ranges;
}

HttpResponse* download_url(const char* url)
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
