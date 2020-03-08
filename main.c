#define _FILE_OFFSET_BITS 64
#ifndef _WIN32
    #include <sys/socket.h>
#else
    #include <winsock2.h>
    #include <fcntl.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
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
uint32_t target_download_size = 1024 * 1024;
struct write_thread {
    FILE* output_file;
    int pipe_to_downloader;
    int pipe_from_downloader;
};
struct file_thread {
    FileList* to_download;
    char* output_path;
    char* bundle_base;
    char* filter;
    char** langs;
    int offset_parity;
};

void* write_to_file(void* _args)
{
    struct write_thread* args = _args;
    BinaryData* data;

    while (1) {
        assert(write(args->pipe_to_downloader, &(uint8_t) {'\0'}, 1) == 1);
        assert(read(args->pipe_from_downloader, &data, sizeof(BinaryData*)) == sizeof(BinaryData*));
        if (!data)
            break;

        fwrite(data->data, data->length, 1, args->output_file);
        free(data->data);
        free(data);
    }

    return NULL;
}

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
            printf("Skipping file %s\n", to_download.name);
            continue;
        }
        printf("Downloading file %s...\n", to_download.name);
        create_dirs(file_output_path, false);
        FILE* output_file = fopen(file_output_path, "wb");
        vprintf(2, "Downloading to %s\n", file_output_path);
        assert(output_file);

        int pipe_to_worker[2], pipe_from_worker[2];
        #ifdef _WIN32
            assert(_pipe(pipe_to_worker, sizeof(BinaryData*), O_BINARY) == 0);
            assert(_pipe(pipe_from_worker, 1, O_BINARY) == 0);
        #else
            assert(pipe(pipe_to_worker) == 0);
            assert(pipe(pipe_from_worker) == 0);
        #endif

        pthread_t tid[1];
        pthread_create(tid, NULL, write_to_file, &(struct write_thread) {.output_file = output_file, .pipe_from_downloader = pipe_to_worker[0], .pipe_to_downloader = pipe_from_worker[1]});

        uint32_t current_chunk = 0;
        uint32_t initial_file_offset = 0;
        while (current_chunk < to_download.chunks.length) {
            ChunkList current_download_list;
            initialize_list(&current_download_list);
            uint32_t current_size = 0;
            while (current_size < target_download_size && current_chunk < to_download.chunks.length) {
                add_object(&current_download_list, &to_download.chunks.objects[current_chunk]);
                current_size += to_download.chunks.objects[current_chunk].uncompressed_size;
                current_chunk++;
            }
            dprintf("Downloading %u bytes at once before writing to disk.\n", current_size);

            BundleList unique_bundles;
            initialize_list(&unique_bundles);
            for (uint32_t i = 0; i < current_download_list.length; i++) {
                Bundle *to_find = NULL;
                find_object_s(&unique_bundles, to_find, bundle_id, current_download_list.objects[i].bundle->bundle_id);
                if (!to_find) {
                    Bundle to_add = {.bundle_id = current_download_list.objects[i].bundle->bundle_id};
                    initialize_list(&to_add.chunks);
                    add_object_s(&to_add.chunks, &current_download_list.objects[i], bundle_offset);
                    add_object(&unique_bundles, &to_add);
                } else {
                    add_object_s(&to_find->chunks, &current_download_list.objects[i], bundle_offset);
                }
            }
            uint8_t* buffer = malloc(current_size);
            for (uint32_t i = 0; i < unique_bundles.length; i++) {
                sprintf(current_bundle_url, "%s/%016"PRIX64".bundle", args->bundle_base, unique_bundles.objects[i].bundle_id);
                uint8_t** ranges = download_ranges(&socket, current_bundle_url, &unique_bundles.objects[i].chunks, args->offset_parity);
                if (!ranges) {
                    eprintf("Failed to download. Make sure to use the correct bundle base url (if necessary).\n");
                    exit(EXIT_FAILURE);
                }
                for (uint32_t j = 0; j < unique_bundles.objects[i].chunks.length; j++) {
                    assert(unique_bundles.objects[i].chunks.objects[j].bundle->bundle_id == unique_bundles.objects[i].bundle_id);
                    assert(ZSTD_decompress(buffer + unique_bundles.objects[i].chunks.objects[j].file_offset - initial_file_offset, unique_bundles.objects[i].chunks.objects[j].uncompressed_size, ranges[j], unique_bundles.objects[i].chunks.objects[j].compressed_size) == unique_bundles.objects[i].chunks.objects[j].uncompressed_size);
                    free(ranges[j]);
                }
                free(ranges);
            }
            BinaryData* to_write = malloc(sizeof(BinaryData));
            to_write->data = buffer;
            to_write->length = current_size;
            assert(read(pipe_from_worker[0], &(uint8_t) {0}, 1) == 1);
            assert(write(pipe_to_worker[1], &to_write, sizeof(BinaryData*)) == sizeof(BinaryData*));

            free(current_download_list.objects);
            for (uint32_t i = 0; i < unique_bundles.length; i++) {
                free(unique_bundles.objects[i].chunks.objects);
            }
            free(unique_bundles.objects);

            initial_file_offset += current_size;
        }

        assert(read(pipe_from_worker[0], &(uint8_t) {0}, 1) == 1);
        assert(write(pipe_to_worker[1], &(void*) {NULL}, sizeof(NULL)) == sizeof(NULL));
        pthread_join(tid[0], NULL);
        close(pipe_from_worker[0]);
        close(pipe_from_worker[1]);
        close(pipe_to_worker[0]);
        close(pipe_to_worker[1]);
        fclose(output_file);
        free(file_output_path);
    }
    free(current_bundle_url);
    closesocket(socket);

    return _args;
}

void print_help()
{
    printf("ManifestDownloader - a tool to download League of Legends files.\n\n");
    printf("Options: \n");
    printf("  [-t|--threads] amount\n    Specify amount of download-threads. Default is 1.\n\n");
    printf("  [-o|--output] path\n    Specify output path. Default is \"output\".\n\n");
    printf("  [-f|--filter] filter\n    Download only files whose full name matches \"filter\".\n\n");
    printf("  [-l|--langs|--languages] language1 language2 ...\n    Provide a list of languaes to download.\n    Will ONLY download files that match any of these languages.\n\n");
    printf("  [--no-langs]\n    Will ONLY download language-neutral files, aka no locale-specific ones.\n\n");
    printf("  [-b|--bundle-*]\n    Provide a different base bundle url. Default is \"https://lol.dyn.riotcdn.net/channels/public/bundles\".\n\n");
    printf("  [-v [-v ...]]\n    Increases verbosity level by one per \"-v\".\n");
}


int main(int argc, char* argv[])
{
    if (argc < 2) {
        eprintf("Missing arguments! Just use the full manifest url or file path as first argument (type --help for more info).\n");
        exit(EXIT_FAILURE);
    }
    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        print_help();
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
        if (strstr(name_lower, filter)) {
            if (!langs[0]) {
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
    free(to_download.objects);

    free_manifest(parsed_manifest);
}
