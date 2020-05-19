#ifndef DOWNLOAD_H
#define DOWNLOAD_H

#include <stdio.h>
#include <pthread.h>

#include "rman.h"
#include "socket_utils.h"

extern int amount_of_threads;
extern char* bundle_base;

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

void download_files(struct download_args* args);

#endif
