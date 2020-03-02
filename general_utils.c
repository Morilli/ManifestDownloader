#ifndef _WIN32
#   include <sys/stat.h>
#else
#   include <unistd.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <stdbool.h>
#include <ctype.h>
#include <errno.h>

#include "general_utils.h"


char* lower_inplace(char* string)
{
    for (char* c = string; *c; c++) {
        *c = tolower(*c);
    }

    return string;
}

char* lower(const char* string)
{
    size_t string_length = strlen(string);
    char* lower_string = malloc(string_length);
    for (size_t i = 0; i < strlen(string); i++) {
        lower_string[i] = tolower(string[i]);
    }

    return lower_string;
}

int char2int(char input)
{
    if (input >= '0' && input <= '9')
        return input - '0';
    if (input >= 'A' && input <= 'F')
        return input - 'A' + 10;
    if (input >= 'a' && input <= 'f')
        return input - 'a' + 10;
    fprintf(stderr, "Error: Malformed input to \"%s\" (%X).\n", __func__, input);
    return -1;
}

void hex2bytes(const char* input, void* output, int input_length)
{
    for (int i = 0; i < input_length; i += 2) {
        ((uint8_t*) output)[i / 2] = char2int(input[i]) * 16 + char2int(input[i + 1]);
    }
}

// converts input_length bytes from input into uppercase hexadecimal representation and saves them in output.
// Will not modify input, add a null byte at the end of output, or allocate the output buffer (make sure it's big enough)
void bytes2hex(const void* input, char* output, int input_length)
{
    const uint8_t* in = input;
    for (int i = 0; i < input_length; i++) {
        output[2 * i] = ((in[i] & 0xF0) >> 4) + (in[i] > 159 ? 55 : 48);
        output[2 * i + 1] = (in[i] & 0x0F) + ((in[i] & 0x0F) > 9 ? 55 : 48);
    }
}

int create_dir(char* path)
{
    int ret = mkdir(path, 0700);
    if (ret == -1 && errno != EEXIST)
        return -1;
    return 0;
}

int create_dirs(char* dir_path, bool create_first, bool create_last)
{
    char* c = dir_path;
    bool skip_once = !create_first;
    while (*c != 0) {
        c++;
        if (*c == '/' || *c == '\\') {
            if (skip_once) {
                skip_once = false;
                continue;
            }
            char _c = *c;
            *c = '\0';
            if (create_dir(dir_path) == -1)
                return -1;
            *c = _c;
        }
    }
    if (create_last && create_dir(dir_path) == -1)
        return -1;
    return 0;
}
