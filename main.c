#define _FILE_OFFSET_BITS 64
#ifndef _WIN32
    #include <sys/socket.h>
    #include <limits.h>
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
char* bundle_base;

struct download_args {
    FileList* to_download;
    char* output_path;
    char* filter;
    char** langs;
    bool verify_only;
    bool skip_existing;
    bool existing_only;
};
struct bundle_args {
    SOCKET socket;
    int coordinate_pipes[2];
    struct variable_bundle_args* variable_args;
};
struct variable_bundle_args {
    BundleList* to_download;
    FILE* output_file;
    pthread_mutex_t* file_lock;
    uint32_t* index;
    int* threads_visited;
    pthread_mutex_t* index_lock;
};

void cleanup_variable_bundle_args(struct variable_bundle_args* args)
{
    fseeko(args->output_file, 0, SEEK_END);
    assert(ftruncate(fileno(args->output_file), ftello(args->output_file) - 1) == 0);
    fclose(args->output_file);
    pthread_mutex_destroy(args->file_lock);
    free(args->file_lock);
    free(args->index);
    free(args->threads_visited);
    pthread_mutex_unlock(args->index_lock);
    pthread_mutex_destroy(args->index_lock);
    free(args->index_lock);
    for (uint32_t i = 0; i < args->to_download->length; i++) {
        free(args->to_download->objects[i].chunks.objects);
    }
    free(args->to_download->objects);
    free(args->to_download);
}

void* download_and_write_bundle(void* _args)
{
    struct bundle_args* args = _args;
    uint32_t index;
    char* current_bundle_url = malloc(strlen(bundle_base) + 25);
    bool visited = false;
    while (1) {
        pthread_mutex_t* lock = args->variable_args->index_lock;
        pthread_mutex_lock(lock);
        index = *args->variable_args->index;
        (*args->variable_args->index)++;
        if (!visited) {
            (*args->variable_args->threads_visited)++;
            visited = true;
        }
        pthread_mutex_unlock(lock);

        if (index >= args->variable_args->to_download->length) {
            if (index == args->variable_args->to_download->length + (*args->variable_args->threads_visited) - 1) {
                cleanup_variable_bundle_args(args->variable_args);
            }
            free(args->variable_args);
            assert(write(args->coordinate_pipes[1], &(uint8_t) {0}, 1) == 1);
            assert(read(args->coordinate_pipes[0], &args->variable_args, sizeof(struct variable_bundle_thread*)) == sizeof(struct variable_bundle_thread*));
            visited = false;
            if (!args->variable_args)
                break;

            continue;
        }

        sprintf(current_bundle_url, "%s/%016"PRIX64".bundle", bundle_base, args->variable_args->to_download->objects[index].bundle_id);
        uint8_t** ranges = download_ranges(&args->socket, current_bundle_url, &args->variable_args->to_download->objects[index].chunks);
        if (!ranges) {
            eprintf("Failed to download. Make sure to use the correct bundle base url (if necessary).\n");
            exit(EXIT_FAILURE);
        }
        for (uint32_t j = 0; j < args->variable_args->to_download->objects[index].chunks.length; j++) {
            uint8_t* to_write = malloc(args->variable_args->to_download->objects[index].chunks.objects[j].uncompressed_size);
            assert(ZSTD_decompress(to_write, args->variable_args->to_download->objects[index].chunks.objects[j].uncompressed_size, ranges[j], args->variable_args->to_download->objects[index].chunks.objects[j].compressed_size) == args->variable_args->to_download->objects[index].chunks.objects[j].uncompressed_size);

            pthread_mutex_lock(args->variable_args->file_lock);
            fseek(args->variable_args->output_file, args->variable_args->to_download->objects[index].chunks.objects[j].file_offset, SEEK_SET);
            fwrite(to_write, args->variable_args->to_download->objects[index].chunks.objects[j].uncompressed_size, 1, args->variable_args->output_file);
            pthread_mutex_unlock(args->variable_args->file_lock);
            free(to_write);
            free(ranges[j]);
        }
        free(ranges);
    }
    closesocket(args->socket);
    free(current_bundle_url);

    return _args;
}

void download_files(struct download_args* args)
{
    char* host = get_host(bundle_base, NULL);

    int pipe_to_downloader[2], pipe_from_downloader[2];
    #ifdef _WIN32
        assert(_pipe(pipe_to_downloader, sizeof(void*), O_BINARY) == 0);
        assert(_pipe(pipe_from_downloader, 1, O_BINARY) == 0);
    #else
        assert(pipe(pipe_to_downloader) == 0);
        assert(pipe(pipe_from_downloader) == 0);
    #endif

    pthread_t tid[amount_of_threads];

    int threads_created = 0;
    bool do_read = true;

    for (uint32_t i = 0; i < args->to_download->length; i++) {
        File to_download = args->to_download->objects[i];
        char* file_output_path = malloc(strlen(args->output_path) + strlen(to_download.name) + 2);
        sprintf(file_output_path, "%s/%s", args->output_path, to_download.name);
        struct stat file_info;
        stat(file_output_path, &file_info);
        ChunkList chunks_to_download;
        bool fixup = false;
        if (access(file_output_path, F_OK) == 0) {
            if (args->skip_existing && file_info.st_size == to_download.file_size) {
                free(file_output_path);
                vprintf(2, "Skipping file %s\n", to_download.name);
                continue;
            } else {
                fixup = true;
                printf("Verifying file %s...\r", to_download.name);
                fflush(stdout);
                initialize_list(&chunks_to_download);
                if (args->verify_only && file_info.st_size != to_download.file_size) {
                    goto verify_failed;
                }
                FILE* input_file = fopen(file_output_path, "rb+");
                assert(input_file);
                for (uint32_t i = 0; i < to_download.chunks.length; i++) {
                    if (file_info.st_size < to_download.chunks.objects[i].file_offset + to_download.chunks.objects[i].uncompressed_size) {
                        if (args->verify_only) {
                            fclose(input_file);
                            goto verify_failed;
                        }
                        add_objects(&chunks_to_download, &to_download.chunks.objects[i], to_download.chunks.length - i);
                        break;
                    } else {
                        BinaryData current_chunk = {
                            .length = to_download.chunks.objects[i].uncompressed_size,
                            .data = malloc(to_download.chunks.objects[i].uncompressed_size)
                        };
                        assert(fread(current_chunk.data, 1, to_download.chunks.objects[i].uncompressed_size, input_file) == to_download.chunks.objects[i].uncompressed_size);
                        if (!chunk_valid(&current_chunk, to_download.chunks.objects[i].chunk_id)) {
                            if (args->verify_only) {
                                fclose(input_file);
                                free(current_chunk.data);
                                goto verify_failed;
                            } else {
                                add_object(&chunks_to_download, &to_download.chunks.objects[i]);
                            }
                        }
                        free(current_chunk.data);
                    }
                }
                fclose(input_file);
                if (chunks_to_download.length == 0 && file_info.st_size == to_download.file_size) {
                    printf("File %s is correct. \n", to_download.name);
                    free(file_output_path);
                    free(chunks_to_download.objects);
                    continue;
                } else verify_failed: {
                    printf("File %s is incorrect.\n", to_download.name);
                    if (args->verify_only) {
                        free(file_output_path);
                        free(chunks_to_download.objects);
                        continue;
                    }
                }
            }
        } else if (args->existing_only) {
            free(file_output_path);
            continue;
        } else if (args->verify_only) {
            printf("File %s is missing.\n", to_download.name);
            free(file_output_path);
            continue;
        }


        FILE* output_file;
        if (fixup) {
            printf("Fixing up file %s...\n", to_download.name);
            if (chunks_to_download.length == 0) {
                free(chunks_to_download.objects);
                assert(truncate(file_output_path, to_download.file_size) == 0);
                free(file_output_path);
                continue;
            } else {
                output_file = fopen(file_output_path, "rb+");
            }
        } else {
            printf("Downloading file %s...\n", to_download.name);
            create_dirs(file_output_path, false);
            output_file = fopen(file_output_path, "wb");
        }
        vprintf(2, "Downloading to %s\n", file_output_path);
        assert(output_file);
        free(file_output_path);

        fseeko(output_file, to_download.file_size, SEEK_SET);
        putc(0, output_file);
        // rewind(output_file);
        pthread_mutex_t* file_lock = malloc(sizeof(pthread_mutex_t));
        pthread_mutex_init(file_lock, NULL);
        BundleList* unique_bundles = group_by_bundles(fixup ? &chunks_to_download : &to_download.chunks);
        if (fixup) {
            free(chunks_to_download.objects);
        }
        printf("amount of bundles to download: %d\n", unique_bundles->length);
        uint32_t* index = malloc(sizeof(uint32_t));
        *index = 0;
        int* threads_visited = malloc(sizeof(int));
        *threads_visited = 0;
        pthread_mutex_t* index_lock = malloc(sizeof(pthread_mutex_t));
        pthread_mutex_init(index_lock, NULL);

        uint32_t current_index = 0;
        if (threads_created < amount_of_threads) {
            pthread_mutex_lock(index_lock);
            while (threads_created < amount_of_threads && current_index < unique_bundles->length) {
                struct bundle_args* new_bundle_args = malloc(sizeof(struct bundle_args));
                new_bundle_args->socket = open_connection_s(host, "80");
                new_bundle_args->coordinate_pipes[0] = pipe_to_downloader[0];
                new_bundle_args->coordinate_pipes[1] = pipe_from_downloader[1];
                struct variable_bundle_args* new_variable_args = malloc(sizeof(struct variable_bundle_args));
                new_variable_args->to_download = unique_bundles;
                new_variable_args->output_file = output_file;
                new_variable_args->file_lock = file_lock;
                new_variable_args->index = index;
                new_variable_args->threads_visited = threads_visited;
                new_variable_args->index_lock = index_lock;
                new_bundle_args->variable_args = new_variable_args;
                pthread_create(&tid[threads_created], NULL, download_and_write_bundle, new_bundle_args);
                current_index++;
                threads_created++;
            }
            pthread_mutex_unlock(index_lock);
        }
        if (threads_created == amount_of_threads && current_index < unique_bundles->length) {
            while (1) {
                printf("entered this infinite loop.\n");
                if (!do_read) {
                    do_read = true;
                } else {
                    assert(read(pipe_from_downloader[0], &(uint8_t) {0}, 1) == 1);
                }
                if (*index >= unique_bundles->length || (*index == unique_bundles->length - 1 && unique_bundles->length != 1)) {
                    do_read = false;
                    break;
                }
                struct variable_bundle_args* new_variable_bundle_args = malloc(sizeof(struct variable_bundle_args));
                new_variable_bundle_args->index = index;
                new_variable_bundle_args->threads_visited = threads_visited;
                new_variable_bundle_args->index_lock = index_lock;
                new_variable_bundle_args->output_file = output_file;
                new_variable_bundle_args->file_lock = file_lock;
                new_variable_bundle_args->to_download = unique_bundles;
                assert(write(pipe_to_downloader[1], &new_variable_bundle_args, sizeof(struct bundle_thread*)) == sizeof(struct bundle_thread*));
                if (unique_bundles->length == 1)
                    break;
            }
        }
    }

    for (int i = 0; i < threads_created; i++) {
        if (!do_read)
            do_read = true;
        else
            assert(read(pipe_from_downloader[0], &(uint8_t) {0}, 1) == 1);
        assert(write(pipe_to_downloader[1], &(void*) {NULL}, sizeof(NULL)) == sizeof(NULL));
    }
    for (int i = 0; i < threads_created; i++) {
        void* to_free;
        pthread_join(tid[i], &to_free);
        free(to_free);
    }
    free(host);
    close(pipe_from_downloader[0]);
    close(pipe_from_downloader[1]);
    close(pipe_to_downloader[0]);
    close(pipe_to_downloader[1]);
}

void print_help()
{
    printf("ManifestDownloader - a tool to download League of Legends (and other Riot Games games') files.\n\n");
    printf("Options: \n");
    printf("  [-t|--threads] amount\n    Specify amount of download-threads. Default is 1.\n\n");
    printf("  [-o|--output] path\n    Specify output path. Default is \"output\".\n\n");
    printf("  [-f|--filter] filter\n    Download only files whose full name matches \"filter\".\n\n");
    printf("  [-l|--langs|--languages] language1 language2 ...\n    Provide a list of languaes to download.\n    Will ONLY download files that match any of these languages.\n\n");
    printf("  [--no-langs]\n    Will ONLY download language-neutral files, aka no locale-specific ones.\n\n");
    printf("  [-b|--bundle-*]\n    Provide a different base bundle url. Default is \"https://lol.dyn.riotcdn.net/channels/public/bundles\".\n\n");
    printf("  [--verify-only]\n    Check files only and print results, but don't update files on disk.\n\n");
    printf("  [--existing-only]\n    Only operate on existing files. Non-existent files are ignored / not created.\n\n");
    printf("  [--skip-existing]\n    By default, all existing files are verified and overwritten if they aren't correct.\n    By specifying this flag existing files will not be checked if their file size matches the expected one.\n\n");
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
    bundle_base = "https://lol.dyn.riotcdn.net/channels/public/bundles";
    char* filter = "";
    char* langs[65];
    bool download_locales = true;
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
        } else if (strcmp(*arg, "--verify-only") == 0) {
            verify_only = true;
        } else if (strcmp(*arg, "--skip-existing") == 0) {
            skip_existing = true;
        } else if (strcmp(*arg, "--existing-only") == 0) {
            existing_only = true;
        } else if (strcmp(*arg, "-v") == 0) {
            VERBOSE++;
        }
    }
    langs[langs_length] = NULL;

    vprintf(1, "output path: %s\n", outputPath);
    vprintf(1, "amount of threads: %d\n", amount_of_threads);
    vprintf(1, "base bundle download path: %s\n", bundle_base);
    vprintf(1, "Filter: \"%s\"\n", filter);
    for (int i = 0; langs[i]; i++) {
        vprintf(1, "langs[%d]: %s\n", i, langs[i]);
    }
    vprintf(1, "Downloading languages: %s\n", download_locales ? "true" : "false");

    char* manifestPath = argv[1];

    create_dirs(outputPath, true);

    Manifest* parsed_manifest;
    if (access(manifestPath, F_OK) == 0) {
        parsed_manifest = parse_manifest(manifestPath);
    } else {
        vprintf(1, "Info: Assuming \"%s\" is a url.\n", manifestPath);
        BinaryData* data = download_url(manifestPath);
        if (!data) {
            eprintf("Make sure the first argument is a valid path to a manifest file or a valid url.\n");
            exit(EXIT_FAILURE);
        }
        parsed_manifest = parse_manifest(data->data);
        free(data->data);
        free(data);
    }

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

    struct download_args download_args = {
        .to_download = &to_download,
        .output_path = outputPath,
        .verify_only = verify_only,
        .existing_only = existing_only,
        .skip_existing = skip_existing
    };
    download_files(&download_args);
    free(to_download.objects);

    free_manifest(parsed_manifest);
}
