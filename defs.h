#ifndef DEFS_H
#define DEFS_H

#include <stdio.h>
#include <inttypes.h>

extern int VERBOSE;

#ifdef _WIN32
    #define mkdir(path, mode) mkdir(path)
    #define flockfile(file) _lock_file(file)
    #define funlockfile(file) _unlock_file(file)
    #if defined(_FILE_OFFSET_BITS) && _FILE_OFFSET_BITS == 64
        #define truncate(path, length) truncate64(path, length)
    #endif
#endif

typedef struct {
    uint64_t length;
    uint8_t* data;
} BinaryData;

#ifdef DEBUG
    #define dprintf(...) printf(__VA_ARGS__)
#else
    #define dprintf(...)
#endif
#define eprintf(...) fprintf(stderr, __VA_ARGS__)
#define v_printf(level, ...) if (VERBOSE >= level) printf(__VA_ARGS__)

#ifndef __cplusplus
#ifdef max
#undef max
#endif
#define max(a, b) __extension__ ({ \
    __typeof__(a) _a = (a); \
    __typeof__(b) _b = (b); \
    _a > _b ? _a : _b; \
})

#ifdef min
#undef min
#endif
#define min(a, b) __extension__ ({ \
    __typeof__(a) _a = (a); \
    __typeof__(b) _b = (b); \
    _a < _b ? _a : _b; \
})
#endif

#endif
