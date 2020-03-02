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
        printf("downloading file %s\n", to_download.name);
        char* file_output_path = malloc(strlen(args->output_path) + strlen(to_download.name) + 2);
        sprintf(file_output_path, "%s/%s", args->output_path, to_download.name);
        struct stat file_info;
        stat(file_output_path, &file_info);
        if (access(file_output_path, F_OK) == 0 && file_info.st_size == to_download.file_size) {
            free(file_output_path);
            continue;
        }
        create_dirs(file_output_path, false, false);
        FILE* output_file = fopen(file_output_path, "wb");
        printf("outfile file: %s\n", file_output_path);
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
            printf("bundle id: %016"PRIX64"\n", unique_bundles.objects[i].bundle_id);
            // for (uint32_t j = 0; j < unique_bundles.objects[i].chunks.length; j++)
            // {
                // printf("chunk id for that bundle: %016"PRIX64"\n", unique_bundles.objects[i].chunks.objects[j].chunk_id);
            // }
            // printf("\n");

            sprintf(current_bundle_url, "%s/%016"PRIX64".bundle", args->bundle_base, unique_bundles.objects[i].bundle_id);
            uint8_t** ranges = download_ranges(&socket, current_bundle_url, &unique_bundles.objects[i].chunks);
            for (uint32_t j = 0; j < unique_bundles.objects[i].chunks.length; j++) {
                assert(unique_bundles.objects[i].chunks.objects[j].bundle->bundle_id == unique_bundles.objects[i].bundle_id);
                // printf("ranges[%d]: \"%s\"\n", j, ranges[j]);
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


int main(int argc, char* argv[])
{
    #ifdef _WIN32
        WSADATA wsaData;
        int iResult;

        // Initialize Winsock
        iResult = WSAStartup(MAKEWORD(2,2), &wsaData);
        if (iResult != 0) {
            printf("WSAStartup failed: %d\n", iResult);
            return 1;
        }
    #endif
    if (argc < 2 || argc > 6) {
        eprintf("Missing argument! Just use the full manifest url as first argument.\n");
        exit(EXIT_FAILURE);
    }

    char* outputPath = "output";
    char* bundleBase = "https://lol.dyn.riotcdn.net/channels/public/bundles";
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
        }
    }

    printf("output path: %s\n", outputPath);
    printf("amount of threads: %d\n", amount_of_threads);
    printf("base bundle download path: %s\n", bundleBase);

    char* manifestUrl = argv[1];

    create_dir(outputPath);

    char* manifestPos = strstr(manifestUrl, ".manifest") - 16;
    char manifestOutputPath[strlen(outputPath) + 27];
    sprintf(manifestOutputPath, "%s/%s", outputPath, manifestPos);
    printf("manifest path: %s\n", manifestOutputPath);
    if (access(manifestOutputPath, F_OK))
        download_url(manifestUrl, manifestOutputPath);

    Manifest* parsed_manifest = parse_manifest(manifestOutputPath);

    pthread_t tid[amount_of_threads];
    for (uint8_t i = 0; i < amount_of_threads; i++) {
        struct file_thread* file_thread = malloc(sizeof(struct file_thread));
        file_thread->to_download = &parsed_manifest->files;
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
