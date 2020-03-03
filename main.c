#ifndef _WIN32
    #include <sys/socket.h>
#else
    #include <winsock2.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <ctype.h>
#include <pthread.h>
#include <unistd.h>
#include <assert.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "general_utils.h"
#include "socket_utils.h"
#include "defs.h"
#include "rman.h"

#include <zstd.h>

int amount_of_threads = 1;
struct file_thread {
    FileList* to_download;
    char* output_path;
    char* bundle_base;
    char* filter;
    char** langs;
    int offset_parity;
};

void* download_file(void* _args)
{
    struct file_thread* args = (struct file_thread*) _args;
    char* host = get_host(args->bundle_base, NULL);
    int socket = open_connection_s(host, "80");
    free(host);

    char* current_bundle_url = malloc(strlen(args->bundle_base) + 25);
    for (uint32_t i = args->offset_parity; i < args->to_download->length; i += amount_of_threads) {
        File to_download = args->to_download->objects[i];
        char* file_output_path = malloc(strlen(args->output_path) + strlen(to_download.name) + 2);
        sprintf(file_output_path, "%s/%s", args->output_path, to_download.name);
        struct stat file_info;
        stat(file_output_path, &file_info);
        if (access(file_output_path, F_OK) == 0 && file_info.st_size == to_download.file_size) {
            free(file_output_path);
            vprintf(2, "Skipping file %s\n", to_download.name);
            continue;
        }
        vprintf(2, "Downloading file %s\n", to_download.name);
        create_dirs(file_output_path, false);
        FILE* output_file = fopen(file_output_path, "wb");
        vprintf(2, "Downloading to %s\n", file_output_path);
        assert(output_file);
        BundleList unique_bundles;
        initialize_list(&unique_bundles);
        for (uint32_t i = 0; i < to_download.chunks.length; i++) {
            Bundle *to_find = NULL;
            find_object_s(&unique_bundles, to_find, bundle_id, to_download.chunks.objects[i].bundle->bundle_id);
            if (!to_find) {
                Bundle to_add = {.bundle_id = to_download.chunks.objects[i].bundle->bundle_id};
                initialize_list(&to_add.chunks);
                add_object_s(&to_add.chunks, &to_download.chunks.objects[i], bundle_offset);
                add_object_s(&unique_bundles, &to_add, bundle_id);
            } else {
                add_object_s(&to_find->chunks, &to_download.chunks.objects[i], bundle_offset);
            }
        }
        for (uint32_t i = 0; i < unique_bundles.length; i++) {
            sprintf(current_bundle_url, "%s/%016"PRIX64".bundle", args->bundle_base, unique_bundles.objects[i].bundle_id);
            uint8_t** ranges = download_ranges(&socket, current_bundle_url, &unique_bundles.objects[i].chunks);
            for (uint32_t j = 0; j < unique_bundles.objects[i].chunks.length; j++) {
                assert(unique_bundles.objects[i].chunks.objects[j].bundle->bundle_id == unique_bundles.objects[i].bundle_id);
                fseek(output_file, unique_bundles.objects[i].chunks.objects[j].file_offset, SEEK_SET);
                uint8_t* uncompressed_data = malloc(unique_bundles.objects[i].chunks.objects[j].uncompressed_size);
                assert(ZSTD_decompress(uncompressed_data, unique_bundles.objects[i].chunks.objects[j].uncompressed_size, ranges[j], unique_bundles.objects[i].chunks.objects[j].compressed_size) == unique_bundles.objects[i].chunks.objects[j].uncompressed_size);
                free(ranges[j]);
                fwrite(uncompressed_data, unique_bundles.objects[i].chunks.objects[j].uncompressed_size, 1, output_file);
                free(uncompressed_data);
            }
            free(ranges);
        }
        for (uint32_t i = 0; i < unique_bundles.length; i++) {
            free(unique_bundles.objects[i].chunks.objects);
        }
        free(unique_bundles.objects);
        fclose(output_file);
        free(file_output_path);
    }
    free(current_bundle_url);
    close(socket);

    return _args;
}
#include <errno.h>


int main(int argc, char* argv[])
{
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
    if (argc < 2) {
        eprintf("Missing argument! Just use the full manifest url as first argument.\n");
        exit(EXIT_FAILURE);
    }

    char* outputPath = "output";
    char* bundleBase = "https://lol.dyn.riotcdn.net/channels/public/bundles";
    char* filter = "";
    char* langs[65];
    bool download_locales = true;
    int langs_length = 0;
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
                bundleBase = *arg;
            }
        } else if (strcmp(*arg, "-f") == 0 || strcmp(*arg, "--filter") == 0) {
            if (*(arg + 1)) {
                arg++;
                filter = lower_inplace(*arg);
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
        } else if (strcmp(*arg, "--no-langs") == 0) {
            download_locales = false;
        } else if (strcmp(*arg, "-v") == 0) {
            VERBOSE++;
        }
    }
    langs[langs_length] = NULL;

    vprintf(1, "output path: %s\n", outputPath);
    vprintf(1, "amount of threads: %d\n", amount_of_threads);
    vprintf(1, "base bundle download path: %s\n", bundleBase);
    vprintf(1, "Filter: \"%s\"\n", filter);
    for (int i = 0; langs[i]; i++) {
        vprintf(1, "langs[%d]: %s\n", i, langs[i]);
    }

    char* manifestPath = argv[1];

    create_dirs(outputPath, true);

    Manifest* parsed_manifest;
    if (access(manifestPath, F_OK) == 0)
        parsed_manifest = parse_manifest(manifestPath);
    else {
        BinaryData* data = download_url(manifestPath);
        if (!data) {
            eprintf("Make sure the first argument is a valid path to a manifest file or a valid url.\n");
            exit(EXIT_FAILURE);
        }
        parsed_manifest = parse_manifest(data->data);
        free(data->data);
        free(data);
    }

    pthread_t tid[amount_of_threads];
    FileList to_download;
    initialize_list(&to_download);
    for (uint32_t i = 0; i < parsed_manifest->files.length; i++) {
        if (!download_locales && parsed_manifest->files.objects[i].languages.length != 0)
            continue;
        bool matches = false;
        char* name_lower = lower(parsed_manifest->files.objects[i].name);
        if (strstr(name_lower, filter))
            matches = true;
        else {
            for (uint32_t j = 0; j < parsed_manifest->files.objects[i].languages.length; j++) {
                for (uint32_t k = 0; langs[k]; k++) {
                    if (strcasecmp(parsed_manifest->files.objects[i].languages.objects[j].name, langs[k]) == 0)
                        matches = true;
                }
            }
        }
        free(name_lower);
        if (matches)
            add_object(&to_download, &parsed_manifest->files.objects[i]);
    }
    vprintf(2, "To download:\n");
    for (uint32_t i = 0; i < to_download.length; i++) {
        vprintf(2, "\"%s\"\n", to_download.objects[i].name);
    }

    for (uint8_t i = 0; i < amount_of_threads; i++) {
        struct file_thread* file_thread = malloc(sizeof(struct file_thread));
        file_thread->to_download = &to_download;
        file_thread->output_path = outputPath;
        file_thread->bundle_base = bundleBase;
        file_thread->offset_parity = i;
        pthread_create(&tid[i], NULL, download_file, (void*) file_thread);
    }
    for (uint8_t i = 0; i < amount_of_threads; i++) {
        void* to_free;
        pthread_join(tid[i], &to_free);
        free(to_free);
    }

    free_manifest(parsed_manifest);
}
