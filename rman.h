#ifndef _RMAN_H
#define _RMAN_H

#include <inttypes.h>

#include "general_utils.h"
#include "defs.h"

#define Vector(type) struct __attribute__((packed)) { \
    uint32_t length; \
    type objects[]; \
}
typedef Vector(char) String;
typedef Vector(uint32_t) OffsetVector;

typedef struct vtable {
    uint16_t vtable_size;
    uint16_t object_size;
    uint16_t offsets[];
} VTable;

#define object_of(position) (void*) ((uint8_t*) (position) + *(uint32_t*) (position))
#define VTable_of(position) (VTable*) ((uint8_t*) (position) - *(int32_t*) (position))

typedef struct chunk {
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint64_t chunk_id;
    struct bundle* bundle;
    uint32_t bundle_offset;
    uint32_t file_offset;
} Chunk;

typedef LIST(Chunk) ChunkList;

typedef struct bundle {
    uint64_t bundle_id;
    ChunkList chunks;
} Bundle;

typedef struct language {
    uint8_t language_id;
    char* name;
} Language;

typedef LIST(struct bundle) BundleList;
typedef LIST(struct language) LanguageList;

typedef struct file_entry {
    uint64_t file_entry_id;
    uint64_t directory_id;
    uint32_t file_size;
    uint8_list language_ids;
    Vector(uint64_t)* chunk_ids;
    String* name;
    String* link;
} FileEntry;

typedef struct directory {
    uint64_t directory_id;
    uint64_t parent_id;
    String* name;
} Directory;

typedef struct file {
    char* name;
    char* link;
    LanguageList languages;
    uint32_t file_size;
    ChunkList chunks;
} File;

typedef LIST(FileEntry) FileEntryList;
typedef LIST(Directory) DirectoryList;
typedef LIST(File) FileList;

struct manifest {
    uint64_t manifest_id;
    ChunkList chunks;
    BundleList bundles;
    LanguageList languages;
    FileList files;
};

typedef struct manifest Manifest;

void free_manifest(Manifest* manifest);

Manifest* parse_manifest_data(uint8_t* data);
Manifest* parse_manifest_f(char* filepath);

#define parse_manifest(X) _Generic((X), \
    char*: parse_manifest_f, \
    uint8_t*: parse_manifest_data \
)(X)

BundleList* group_by_bundles(ChunkList* chunks);

bool chunk_valid(BinaryData* chunk, uint64_t chunk_id);

#endif
