#define _FILE_OFFSET_BITS 64
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
#include <zstd.h>

#include "defs.h"
#include "download.h"
#include "general_utils.h"
#include "list.h"
#include "socket_utils.h"
#include "rman.h"

#define PCRE2_STATIC
#define PCRE2_CODE_UNIT_WIDTH 8
#include "pcre2/pcre2.h"


int VERBOSE;
int amount_of_threads = 1;
char* bundle_base;

void print_help()
{
    printf("ManifestDownloader - a tool to download League of Legends (and other Riot Games games') files.\n\n");
    printf("Options: \n");
    printf("  [-t|--threads] amount\n    Specify amount of download-threads. Default is 1.\n\n");
    printf("  [-o|--output] path\n    Specify output path. Default is \"output\".\n\n");
    printf("  [-f|--filter] filter\n    Download only files whose full name matches \"filter\".\n\n");
    printf("  [-u|--unfilter] unfilter\n    Download only files whose full name does not match \"unfilter\".\n\n    Note: Both -f and -u options use case-independent regex-matching.\n\n");
    printf("  [-l|--langs|--languages] language1 language2 ...\n    Provide a list of languages to download.\n    Will ONLY download files that match any of these languages.\n\n");
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
    char* unfilter = "";
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

    v_printf(1, "output path: %s\n", outputPath);
    v_printf(1, "amount of threads: %d\n", amount_of_threads);
    v_printf(1, "base bundle download path: %s\n", bundle_base);
    v_printf(1, "Filter: \"%s\"\n", filter);
    v_printf(1, "Unfilter: \"%s\"\n", unfilter);
    for (int i = 0; langs[i]; i++) {
        v_printf(1, "langs[%d]: %s\n", i, langs[i]);
    }
    v_printf(1, "Downloading languages: %s\n", download_locales ? "true" : "false");

    char* manifestPath = argv[1];

    create_dirs(outputPath, true);

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

    FileList to_download;
    initialize_list(&to_download);
    pcre2_code* pattern = pcre2_compile((PCRE2_SPTR) filter, PCRE2_ZERO_TERMINATED, PCRE2_CASELESS, &(int) {0}, &(size_t) {0}, NULL);
    pcre2_code* antipattern = pcre2_compile((PCRE2_SPTR) unfilter, PCRE2_ZERO_TERMINATED, PCRE2_CASELESS, &(int) {0}, &(size_t) {0}, NULL);
    pcre2_match_data* match_data = pcre2_match_data_create(1, NULL);
    for (uint32_t i = 0; i < parsed_manifest->files.length; i++) {
        if (!download_locales && parsed_manifest->files.objects[i].languages.length != 0)
            continue;
        bool matches = false;
        if (pcre2_match(pattern, (PCRE2_SPTR) parsed_manifest->files.objects[i].name, PCRE2_ZERO_TERMINATED, 0, 0, match_data, NULL) > 0 && pcre2_match(antipattern, (PCRE2_SPTR) parsed_manifest->files.objects[i].name, PCRE2_ZERO_TERMINATED, 0, PCRE2_NOTEMPTY, match_data, NULL) < 0) {
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
