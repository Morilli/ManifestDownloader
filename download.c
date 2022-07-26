#define _FILE_OFFSET_BITS 64
#ifdef _WIN32
    #include <fcntl.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <assert.h>
#include "zstd/zstd.h"
#include "BearSSL/inc/bearssl_ssl.h"
#include "BearSSL/trust_anchors.h"

#include "download.h"
#include "defs.h"
#include "general_utils.h"
#include "list.h"
#include "rman.h"
#include "socket_utils.h"


void cleanup_variable_bundle_args(struct variable_bundle_args* args)
{
    fseeko(args->output_file, 0, SEEK_END);
    fflush(args->output_file);
    assert(ftruncate(fileno(args->output_file), ftello(args->output_file) - 1) == 0);
    fclose(args->output_file);
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
    size_t bundle_base_length = strlen(bundle_base);
    char current_bundle_url[bundle_base_length + 25];
    memcpy(current_bundle_url, bundle_base, bundle_base_length);
    bool visited = false;
    ZSTD_DCtx* context = ZSTD_createDCtx();
    while (1) {
        pthread_mutex_t* lock = args->variable_args->index_lock;
        pthread_mutex_lock(lock);
        index = *args->variable_args->index;
        (*args->variable_args->index)++;
        if (!visited) {
            (*args->variable_args->threads_visited)++;
            visited = true;
        }
        if (*args->variable_args->index == args->variable_args->to_download->length) {
            *args->file_index_finished = max(*args->file_index_finished, args->variable_args->file_index);
        }
        pthread_mutex_unlock(lock);

        if (index >= args->variable_args->to_download->length) {
            if (index == args->variable_args->to_download->length + (*args->variable_args->threads_visited) - 1) {
                cleanup_variable_bundle_args(args->variable_args);
            }
            free(args->variable_args);
            assert(write(args->coordinate_pipes[1], &(uint8_t) {0}, 1) == 1);
            assert(read(args->coordinate_pipes[0], &args->variable_args, sizeof(struct variable_bundle_args*)) == sizeof(struct variable_bundle_args*));
            visited = false;
            if (!args->variable_args)
                break;

            continue;
        }

        sprintf(current_bundle_url + bundle_base_length, "/%016"PRIX64".bundle", args->variable_args->to_download->objects[index].bundle_id);
        uint8_t** ranges;
        if (args->filesystem_only) {
            ranges = get_ranges(current_bundle_url, &args->variable_args->to_download->objects[index].chunks);
            if (!ranges) {
                eprintf("Make sure all required bundles exist and are accessable at \"%s\".\n", bundle_base);
                exit(EXIT_FAILURE);
            }
        } else {
            ranges = download_ranges(&args->ssl_structs, current_bundle_url, &args->variable_args->to_download->objects[index].chunks);
            if (!ranges) {
                eprintf("Failed to download. Make sure to use the correct bundle base url (if necessary).\n");
                exit(EXIT_FAILURE);
            }
        }
        for (uint32_t j = 0; j < args->variable_args->to_download->objects[index].chunks.length; j++) {
            uint8_t* to_write = malloc(args->variable_args->to_download->objects[index].chunks.objects[j].uncompressed_size);
            size_t decompressedSize = ZSTD_decompressDCtx(context, to_write, args->variable_args->to_download->objects[index].chunks.objects[j].uncompressed_size, ranges[j], args->variable_args->to_download->objects[index].chunks.objects[j].compressed_size);
            if (decompressedSize != args->variable_args->to_download->objects[index].chunks.objects[j].uncompressed_size) {
                eprintf("Error: ZSTD decompressed size doesn't match expected value! Expected %u, got %"PRId64"\n", args->variable_args->to_download->objects[index].chunks.objects[j].uncompressed_size, decompressedSize);
                eprintf("failing chunk id: %016"PRIx64", failing bundle id: %016"PRIx64"\n", args->variable_args->to_download->objects[index].chunks.objects[j].chunk_id, args->variable_args->to_download->objects[index].chunks.objects[j].bundle_id);
                exit(EXIT_FAILURE);
            }

            flockfile(args->variable_args->output_file);
            fseeko(args->variable_args->output_file, args->variable_args->to_download->objects[index].chunks.objects[j].file_offset, SEEK_SET);
            fwrite(to_write, args->variable_args->to_download->objects[index].chunks.objects[j].uncompressed_size, 1, args->variable_args->output_file);
            funlockfile(args->variable_args->output_file);
            free(to_write);
            free(ranges[j]);
        }
        free(ranges);
    }
    if (!args->filesystem_only)
        closesocket(args->ssl_structs.socket);
    ZSTD_freeDCtx(context);

    return _args;
}

void download_files(struct download_args* args)
{
    bool filesystem_only = access(bundle_base, F_OK) == 0;
    if (filesystem_only)
        v_printf(1, "Info: Assuming \"%s\" is a path on disk.\n", bundle_base);
    HostPort* host_port = get_host_port(bundle_base);
    bool is_ssl = strcmp(host_port->port, "443") == 0;
    char file_buffer[256*1024];

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
    int32_t file_index_finished = -1;

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
                v_printf(2, "Skipping file %s\n", to_download.name);
                continue;
            } else {
                fixup = true;
                flockfile(stdout);
                printf("Verifying file %s...\r", to_download.name);
                fflush(stdout);
                initialize_list(&chunks_to_download);
                if (args->verify_only && file_info.st_size != to_download.file_size) {
                    goto verify_failed;
                }
                FILE* input_file = fopen(file_output_path, "rb+");
                if (!input_file) {
                    eprintf("Error: Failed to open \"%s\"\n", file_output_path);
                    exit(EXIT_FAILURE);
                }
                setvbuf(input_file, file_buffer, _IOFBF, 256 * 1024); // notably boosts performance when fully reading large files in my testing
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
                    funlockfile(stdout);
                    continue;
                } else verify_failed: {
                    printf("File %s is incorrect.\n", to_download.name);
                    funlockfile(stdout);
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

        if (threads_created == amount_of_threads && do_read) {
            assert(read(pipe_from_downloader[0], &(uint8_t) {0}, 1) == 1);
            do_read = false;
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
            printf("%s file %s...\n", filesystem_only ? "Processing" : "Downloading", to_download.name);
            create_dirs(file_output_path, false);
            output_file = fopen(file_output_path, "wb");
        }
        v_printf(2, "Downloading to %s\n", file_output_path);
        assert(output_file);
        free(file_output_path);
        assert(ftruncate(fileno(output_file), to_download.file_size + 1) == 0);
        BundleList* unique_bundles = group_by_bundles(fixup ? &chunks_to_download : &to_download.chunks);
        if (fixup) {
            free(chunks_to_download.objects);
        }
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
                if (!filesystem_only) {
                    new_bundle_args->ssl_structs.socket = open_connection_s(host_port->host, host_port->port);
                    new_bundle_args->ssl_structs.host_port = host_port;
                    if (is_ssl) {
                        br_ssl_client_init_full(&new_bundle_args->ssl_structs.ssl_client_context, &new_bundle_args->ssl_structs.x509_client_context, TAs, TAs_NUM);
                        new_bundle_args->ssl_structs.io_buffer = malloc(BR_SSL_BUFSIZE_BIDI);
                        br_ssl_engine_set_buffer(&new_bundle_args->ssl_structs.ssl_client_context.eng, new_bundle_args->ssl_structs.io_buffer, BR_SSL_BUFSIZE_BIDI, 1);
                        br_ssl_client_reset(&new_bundle_args->ssl_structs.ssl_client_context, host_port->host, 0);
                        br_sslio_init(&new_bundle_args->ssl_structs.ssl_io_context, &new_bundle_args->ssl_structs.ssl_client_context.eng, recv_wrapper, &new_bundle_args->ssl_structs.socket, send_wrapper, &new_bundle_args->ssl_structs.socket);
                    }
                }
                new_bundle_args->filesystem_only = filesystem_only;
                new_bundle_args->coordinate_pipes[0] = pipe_to_downloader[0];
                new_bundle_args->coordinate_pipes[1] = pipe_from_downloader[1];
                new_bundle_args->file_index_finished = &file_index_finished;
                struct variable_bundle_args* new_variable_args = malloc(sizeof(struct variable_bundle_args));
                new_variable_args->to_download = unique_bundles;
                new_variable_args->output_file = output_file;
                new_variable_args->file_index = i;
                new_variable_args->index = index;
                new_variable_args->threads_visited = threads_visited;
                new_variable_args->index_lock = index_lock;
                new_bundle_args->variable_args = new_variable_args;
                pthread_create(&tid[threads_created], NULL, download_and_write_bundle, new_bundle_args);
                current_index++;
                threads_created++;
            }
            pthread_mutex_unlock(index_lock);
            if (amount_of_threads == 1)
                continue;
        }
        if (threads_created == amount_of_threads && current_index < unique_bundles->length) {
            while (1) {
                if (!do_read) {
                    do_read = true;
                } else {
                    assert(read(pipe_from_downloader[0], &(uint8_t) {0}, 1) == 1);
                }
                if (file_index_finished == (int32_t) i) {
                    do_read = false;
                    break;
                }
                struct variable_bundle_args* new_variable_bundle_args = malloc(sizeof(struct variable_bundle_args));
                new_variable_bundle_args->index = index;
                new_variable_bundle_args->threads_visited = threads_visited;
                new_variable_bundle_args->index_lock = index_lock;
                new_variable_bundle_args->output_file = output_file;
                new_variable_bundle_args->file_index = i;
                new_variable_bundle_args->to_download = unique_bundles;
                assert(write(pipe_to_downloader[1], &new_variable_bundle_args, sizeof(struct variable_bundle_args*)) == sizeof(struct variable_bundle_args*));
                if (amount_of_threads == 1 || unique_bundles->length == 1)
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
        if (is_ssl) free(((struct bundle_args*) to_free)->ssl_structs.io_buffer);
        free(to_free);
    }
    free(host_port->host);
    free(host_port);
    close(pipe_from_downloader[0]);
    close(pipe_from_downloader[1]);
    close(pipe_to_downloader[0]);
    close(pipe_to_downloader[1]);
}
