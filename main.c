#define _FILE_OFFSET_BITS 64
#define PCRE2_STATIC
#define PCRE2_CODE_UNIT_WIDTH 8
#ifdef _WIN32
    #include <winsock2.h>
    #include <fcntl.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>
#include "pcre2/pcre2.h"
#include "sha/sha_extension.h"

#include "defs.h"
#include "download.h"
#include "general_utils.h"
#include "list.h"
#include "socket_utils.h"
#include "rman.h"


int VERBOSE;
int amount_of_threads = 1;
const char* bundle_base;

void print_manifest(Manifest* manifest, char* output_path)
{
    FILE* output_file = fopen(output_path, "wb");
    if (!output_file) {
        eprintf("Error: Failed to access file \"%s\"\n", output_path);
        exit(EXIT_FAILURE);
    }
    fprintf(output_file, "{\n  \"Manifest ID\": \"%016"PRIX64"\",\n", manifest->manifest_id);
    fprintf(output_file, "  \"languages\": [");
    for (uint32_t i = 0; i < manifest->languages.length; i++) {
        if (i) fprintf(output_file, ",\n"); else fprintf(output_file, "\n");
        fprintf(output_file, "    {\"ID\": %d, \"name\": \"%s\"}", manifest->languages.objects[i].language_id, manifest->languages.objects[i].name);
    }
    if (manifest->languages.length) {
        fprintf(output_file, "\n  ");
    }
    fprintf(output_file, "],\n  \"files\": [{\n");
    for (uint32_t i = 0; i < manifest->files.length; i++) {
        if (i) fprintf(output_file, ", {\n");
        fprintf(output_file, "    \"path\": \"%s\",\n", manifest->files.objects[i].name);
        fprintf(output_file, "    \"file_size\": \"%d bytes\",\n", manifest->files.objects[i].file_size);
        fprintf(output_file, "    \"languages\": [");
        for (uint32_t j = 0; j < manifest->files.objects[i].languages.length; j++) {
            if (j) fprintf(output_file, ", "); else fprintf(output_file, "\n      ");
            fprintf(output_file, "\"%s\"", manifest->files.objects[i].languages.objects[j].name);
        }
        if (manifest->files.objects[i].languages.length) {
            fprintf(output_file, "\n    ");
        };
        fprintf(output_file, "]\n  }");
    }
    fprintf(output_file, "]\n}\n");
}

void get_access_and_id_token(const HttpResponse* put_response, char** access_token, char** id_token)
{
    if (!put_response || !access_token || !id_token) return;

    char* string_data = malloc(put_response->length + 1);
    memcpy(string_data, put_response->data, put_response->length);
    string_data[put_response->length] = '\0';

    char* access_token_position = strstr(string_data, "access_token=");
    if (!access_token_position) return;
    access_token_position += 13;
    char* access_token_end = strstr(access_token_position, "&");
    if (!access_token_end) return;
    *access_token = calloc(1, access_token_end - access_token_position + 1);
    memcpy(*access_token, access_token_position, access_token_end - access_token_position);

    char* id_token_position = strstr(string_data, "id_token=");
    if (!id_token_position) return;
    id_token_position += 9;
    char* id_token_end = strstr(id_token_position, "&");
    if (!id_token_end) return;
    *id_token = calloc(1, id_token_end - id_token_position + 1);
    memcpy(*id_token, id_token_position, id_token_end - id_token_position);

    free(string_data);
}

int main(int argc, char* argv[])
{
    if (argc < 3) {
        eprintf("Syntax: %s \"username\" \"password\"\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    #ifdef _WIN32
        WSADATA wsaData;
        int iResult;

        // Initialize Winsock
        iResult = WSAStartup(MAKEWORD(2,2), &wsaData);
        if (iResult != 0) {
            eprintf("WSAStartup failed: %d\n", iResult);
            return 1;
        }
    #endif

    const char post_payload[] = "{\"client_id\": \"bacon-client\", \"nonce\": \"none\", \"response_type\": \"token id_token\", \"redirect_uri\": \"http://localhost/redirect\", \"scope\": \"openid ban lol link account\"}";
    const char* url = "https://auth.riotgames.com/api/v1/authorization";
    struct ssl_data* ssl_structs = setup_connection(url);
    HttpResponse* post_response = rest_request(url, post_payload, NULL, ssl_structs, "POST");
    if (!post_response) {
        eprintf("Error: Got no post response!\n");
        return -1;
    }
    free(post_response->data);
    free(post_response);

    // json escaping
    #define escape_json(string) do { for (char* c = string; *c; c++) { \
        switch (*c) \
        { \
            case '\b': add_objects(&put_payload, "\\\b", 2); break; \
            case '\t': add_objects(&put_payload, "\\\t", 2); break; \
            case '\n': add_objects(&put_payload, "\\\n", 2); break; \
            case '\f': add_objects(&put_payload, "\\\f", 2); break; \
            case '\r': add_objects(&put_payload, "\\\r", 2); break; \
            case '\"': add_objects(&put_payload, "\\\"", 2); break; \
            case '\\': add_objects(&put_payload, "\\\\", 2); break; \
            default: add_object(&put_payload, c); \
        } \
    }} while (0)

    // build the put_request payload from username and password
    LIST(char) put_payload;
    initialize_list(&put_payload);
    add_objects(&put_payload, "{\"type\": \"auth\", \"username\": \"", 30);
    escape_json(argv[1]);
    add_objects(&put_payload, "\",\"password\": \"", 15);
    escape_json(argv[2]);
    add_objects(&put_payload, "\"}", 3);
    HttpResponse* put_response = rest_request(url, put_payload.objects, NULL, ssl_structs, "PUT");
    free(put_payload.objects);
    if (!put_response) {
        eprintf("Error: Got no put response!\n");
        return -1;
    }

    // char* access_token = NULL;
    // char* id_token = NULL;
    // get_access_and_id_token(put_response, &access_token, &id_token);
    // free(put_response->data);
    // free(put_response);

    // if (!access_token || !id_token) {
        // eprintf("Error: Couldn't parse access and id token!\n");
        // return -1;
    // }
    // printf("%s\n%s\n", access_token, id_token); // Output the access token and id token to stdout for it to get read
    fwrite(put_response->data, put_response->length, 1, stdout);

    free(ssl_structs->host_port->host);
    free(ssl_structs->host_port);
    closesocket(ssl_structs->socket);
    for (uint32_t i = 0; i < ssl_structs->cookies.length; i++) {
        free(ssl_structs->cookies.objects[i]);
    }
    free(ssl_structs->cookies.objects);
    free(ssl_structs->io_buffer);
    free(ssl_structs);
    // free(access_token);
    // free(id_token);
    return 0;

    // The following would be necessary if passing back the access and id token would not be enough for python to work again


    // char authorization_header[strlen(access_token) + 23];
    // sprintf(authorization_header, "Authorization: Bearer %s", access_token);

    // char* entitlements_payload = "{\"urn\": \"urn:entitlement:%\"}";
    // free(ssl_structs->host_port->host);
    // free(ssl_structs->host_port); // TODO: free the entire ssl_structs here before reassigning it
    // ssl_structs = setup_connection("https://entitlements.auth.riotgames.com/api/token/v1");
    // HttpResponse* entitlements_response = rest_request("https://entitlements.auth.riotgames.com/api/token/v1", entitlements_payload, authorization_header, ssl_structs, "POST");
    // if (!entitlements_response) {
    //     eprintf("Error: Got no response!\n");
    //     return -1;
    // } else {
    //     fputs("entitlements response: \"", stdout);
    //     fwrite(entitlements_response->data, entitlements_response->length, 1, stdout);
    //     puts("\"");
    // }
    // free(entitlements_response->data);
    // free(entitlements_response);

    return 0;


    hasShaExtension = checkShaExtension();

    char* outputPath = "output";
    bool do_print_manifest = false;
    char* print_manifest_path = (char[22]) {0};
    bundle_base = "https://lol.dyn.riotcdn.net/channels/public/bundles";
    char* filter = "";
    char* unfilter = "";
    char* langs[65];
    bool download_locales = true;
    bool download_neutrals = false;
    int langs_length = 0;
    bool verify_only = false;
    bool skip_existing = false;
    bool existing_only = false;
    for (char** arg = &argv[2]; *arg; arg++) {
        if (strcmp(*arg, "-t") == 0 || strcmp(*arg, "--threads") == 0) {
            if (*(arg + 1)) {
                arg++;
                amount_of_threads = strtol(*arg, NULL, 10);
            }
        } else if (strcmp(*arg, "-o") == 0 || strcmp(*arg, "--output") == 0) {
            if (*(arg + 1)) {
                arg++;
                outputPath = *arg;
            }
        } else if (strcmp(*arg, "-b") == 0 || strncmp(*arg, "--bundle", 8) == 0) {
            if (*(arg + 1)) {
                arg++;
                bundle_base = *arg;
            }
        } else if (strcmp(*arg, "-f") == 0 || strcmp(*arg, "--filter") == 0) {
            if (*(arg + 1)) {
                arg++;
                filter = *arg;
            }
        } else if (strcmp(*arg, "-u") == 0 || strcmp(*arg, "--unfilter") == 0) {
            if (*(arg + 1)) {
                arg++;
                unfilter = *arg;
            }
        } else if (strcmp(*arg, "-l") == 0 || strcmp(*arg, "--langs") == 0 || strcmp(*arg, "--languages") == 0) {
            while (*(arg + 1) && **(arg + 1) != '-') {
                arg++;
                if (langs_length == 64) {
                    eprintf("Too many languages provided! Use a maximum of 64.\n");
                    exit(EXIT_FAILURE);
                } else {
                    langs[langs_length] = lower_inplace(*arg);
                    langs_length++;
                }
            }
        } else if (strcmp(*arg, "-n") == 0 || strcmp(*arg, "--neutral") == 0) {
            download_neutrals = true;
        } else if (strcmp(*arg, "--no-langs") == 0) {
            download_locales = false;
        } else if (strcmp(*arg, "--verify-only") == 0) {
            verify_only = true;
        } else if (strcmp(*arg, "--skip-existing") == 0) {
            skip_existing = true;
        } else if (strcmp(*arg, "--existing-only") == 0) {
            existing_only = true;
        } else if (strcmp(*arg, "--print-manifest") == 0) {
            do_print_manifest = true;
            if (*(arg + 1) && **(arg + 1) != '-') {
                arg++;
                print_manifest_path = *arg;
            }
        } else if (strcmp(*arg, "-v") == 0) {
            VERBOSE++;
        }
    }
    langs[langs_length] = NULL;

    v_printf(1, "output path: %s\n", outputPath);
    v_printf(1, "amount of threads: %d\n", amount_of_threads);
    v_printf(1, "base bundle download path: %s\n", bundle_base);
    v_printf(1, "Filter: \"%s\"\n", filter);
    v_printf(1, "Unfilter: \"%s\"\n", unfilter);
    for (int i = 0; langs[i]; i++) {
        v_printf(1, "langs[%d]: %s\n", i, langs[i]);
    }
    v_printf(1, "Downloading languages: %s\n", download_locales ? "true" : "false");
    v_printf(1, "Downloading language-neutral files: %s\n", download_neutrals || langs_length == 0 ? "true" : "false");

    char* manifestPath = argv[1];

    Manifest* parsed_manifest;
    if (access(manifestPath, F_OK) == 0) {
        parsed_manifest = parse_manifest(manifestPath);
    } else {
        v_printf(1, "Info: Assuming \"%s\" is a url.\n", manifestPath);
        HttpResponse* data = download_url(manifestPath);
        if (!data) {
            eprintf("Make sure the first argument is a valid path to a manifest file or a valid url.\n");
            exit(EXIT_FAILURE);
        } else if (data->status_code >= 400) {
            eprintf("Error: Got a %d response.\n", data->status_code);
            exit(EXIT_FAILURE);
        }
        parsed_manifest = parse_manifest(data->data);
        free(data->data);
        free(data);
    }
    if (!parsed_manifest) {
        exit(EXIT_FAILURE);
    }

    if (do_print_manifest) {
        if (!*print_manifest_path) {
            sprintf(print_manifest_path, "%016"PRIX64".json", parsed_manifest->manifest_id);
        }
        printf("Printing manifest info to \"%s\"...\n", print_manifest_path);
        print_manifest(parsed_manifest, print_manifest_path);
        exit(EXIT_SUCCESS);
    }

    FileList to_download;
    initialize_list(&to_download);
    pcre2_code* pattern = pcre2_compile((PCRE2_SPTR) filter, PCRE2_ZERO_TERMINATED, PCRE2_CASELESS, &(int) {0}, &(size_t) {0}, NULL);
    pcre2_code* antipattern = pcre2_compile((PCRE2_SPTR) unfilter, PCRE2_ZERO_TERMINATED, PCRE2_CASELESS, &(int) {0}, &(size_t) {0}, NULL);
    pcre2_match_data* match_data = pcre2_match_data_create(0, NULL);
    for (uint32_t i = 0; i < parsed_manifest->files.length; i++) {
        if (!download_locales && parsed_manifest->files.objects[i].languages.length != 0)
            continue;
        bool matches = false;
        if (pcre2_match(pattern, (PCRE2_SPTR) parsed_manifest->files.objects[i].name, PCRE2_ZERO_TERMINATED, 0, 0, match_data, NULL) >= 0 && pcre2_match(antipattern, (PCRE2_SPTR) parsed_manifest->files.objects[i].name, PCRE2_ZERO_TERMINATED, 0, PCRE2_NOTEMPTY, match_data, NULL) < 0) {
            if (!langs[0] || (download_neutrals && parsed_manifest->files.objects[i].languages.length == 0)) {
                matches = true;
            } else {
                for (uint32_t j = 0; j < parsed_manifest->files.objects[i].languages.length; j++) {
                    for (uint32_t k = 0; langs[k]; k++) {
                        if (strcasecmp(parsed_manifest->files.objects[i].languages.objects[j].name, langs[k]) == 0)
                            matches = true;
                    }
                }
            }
        }
        if (matches)
            add_object(&to_download, &parsed_manifest->files.objects[i]);
    }
    pcre2_match_data_free(match_data);
    pcre2_code_free(pattern);
    pcre2_code_free(antipattern);

    if (!verify_only) {
        v_printf(2, "To download:\n");
        for (uint32_t i = 0; i < to_download.length; i++) {
            v_printf(2, "\"%s\"\n", to_download.objects[i].name);
        }
        if (existing_only)
            v_printf(2, "Note: Non-existent files will be skipped.\n");
    }

    if (to_download.length) {
        create_dirs(outputPath, true);
        struct download_args download_args = {
            .to_download = &to_download,
            .output_path = outputPath,
            .verify_only = verify_only,
            .existing_only = existing_only,
            .skip_existing = skip_existing
        };
        download_files(&download_args);
    }

    #ifdef _WIN32
        WSACleanup();
    #endif
    free(to_download.objects);
    free_manifest(parsed_manifest);
}
