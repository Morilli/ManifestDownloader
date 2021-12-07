#ifndef DOWNLOAD_H
#define DOWNLOAD_H

#include <stdio.h>
#include <pthread.h>

#include "rman.h"
#include "socket_utils.h"

extern int amount_of_threads;
extern const char* bundle_base;

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
    struct ssl_data ssl_structs;
    int coordinate_pipes[2];
    int32_t* file_index_finished;
    struct variable_bundle_args* variable_args;
};
struct variable_bundle_args {
    BundleList* to_download;
    FILE* output_file;
    int32_t file_index;
    uint32_t* index;
    int* threads_visited;
    pthread_mutex_t* index_lock;
};

void download_files(struct download_args* args);

#endif
