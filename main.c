#ifndef _WIN32
    #include <sys/socket.h>
#else
    #define __USE_MINGW_ANSI_STDIO
    #include <winsock2.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <ctype.h>
#include <pthread.h>

#include "general_utils.h"
#include "socket_utils.h"
#include "rman.h"

int amount_of_threads = 5;
struct um {
    Manifest* manifest;
    int offset_parity;
};

void* download_bundle(void* args)
{
    Manifest* passed_one = ((struct um*) args)->manifest;
    int offset_parity = ((struct um*) args)->offset_parity;

    char* current_bundle_url = malloc(51 + 25);
    char* bundleOutputPath = malloc(31);
    for (uint32_t i = offset_parity; i < passed_one->bundles.length; i += amount_of_threads) {
        sprintf(current_bundle_url, "%s/%016"PRIX64".bundle", "https://lol.dyn.riotcdn.net/channels/public/bundles", passed_one->bundles.objects[i].bundle_id);
        sprintf(bundleOutputPath, "output/%016"PRIX64".bundle", passed_one->bundles.objects[i].bundle_id);
        download_file(current_bundle_url, bundleOutputPath);
    }

    return NULL;
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
    if (argc != 2 && argc != 3) {
        eprintf("Missing argument! Just use the full manifest url as first argument.\n");
        exit(EXIT_FAILURE);
    }
    if (argc == 3) {
        amount_of_threads = strtol(argv[2], NULL, 10);
    }

    const char*const bundleBase = "https://lol.dyn.riotcdn.net/channels/public/bundles";
    char* manifestUrl = argv[1];

    char* outputPath = "output";
    create_dir(outputPath);

    char* manifestPos = strstr(argv[1], ".manifest") - 16;
    char manifestOutputPath[strlen(outputPath) + 27];
    sprintf(manifestOutputPath, "%s/%s", outputPath, manifestPos);
    // sprintf(manifestOutputPath, "%s/")
    // download_file(manifestUrl, manifestOutputPath);

    Manifest* parsed_manifest = parse_manifest(manifestOutputPath);

    exit(EXIT_SUCCESS);

    pthread_t tid[amount_of_threads];
    for (uint8_t i = 0; i < amount_of_threads; i++) {
        struct um* ums = malloc(sizeof(struct um));
        ums->manifest = parsed_manifest;
        ums->offset_parity = i;
        pthread_create(&tid[i], NULL, download_bundle, (void*) ums);
    }
    for (uint8_t i = 0; i < 6; i++) {
        pthread_join(tid[i], NULL);
    }
    // char current_bundle_url[strlen(bundleBase) + 25];
    // sprintf(current_bundle_url, "%s/0123456789ABCDEF.bundle", bundleBase);
    // for (uint32_t i = 0; i < parsed_manifest->bundles.length; i++) {
    //     sprintf(&current_bundle_url[strlen(bundleBase) + 1], "%016"PRIX64".bundle", parsed_manifest->bundles.objects[i].bundle_id);
    //     char bundleOutputPath[strlen(outputPath) + 25];
    //     sprintf(bundleOutputPath, "%s/%016"PRIX64".bundle", outputPath, parsed_manifest->bundles.objects[i].bundle_id);
    //     download_file(current_bundle_url, bundleOutputPath);
    // }

}