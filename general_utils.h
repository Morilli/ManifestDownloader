#ifndef GENERAL_UTILS_H
#define GENERAL_UTILS_H

#include <stdbool.h>

char* lower(const char* string);

char* lower_inplace(char* string);

void hex2bytes(const char* input, void* output, int input_length);

void bytes2hex(const void* input, char* output, int input_length);

int create_dir(char* path);

int create_dirs(char* dir_path, bool create_last);

#endif
