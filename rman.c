#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>
#include <unistd.h>

#include "general_utils.h"
#include "defs.h"
#include "rman.h"

#include <zstd.h>
#include "sha/sha2.h"


bool chunk_valid(BinaryData* chunk, uint64_t chunk_id)
{
    // code taken straight from moonshadow, no idea what this shit is lol
    sha256_ctx sha;
    sha256_begin(&sha);
    uint8_t key[64] = {0};
    sha256_hash(chunk->data, chunk->length, &sha);
    sha256_end(key, &sha);
    uint8_t ipad[64], opad[64];
    memcpy(ipad, key, 64);
    memcpy(opad, key, 64);
    for (int i = 0; i < 64; i++) {
        ipad[i] ^= 0x36;
        opad[i] ^= 0x5C;
    }
    uint8_t buffer[32];
    uint8_t index[4] = {0, 0, 0, 1};
    sha256_begin(&sha);
    sha256_hash(ipad, 64, &sha);
    sha256_hash(index, 4, &sha);
    sha256_end(buffer, &sha);
    sha256_begin(&sha);
    sha256_hash(opad, 64, &sha);
    sha256_hash(buffer, 32, &sha);
    sha256_end(buffer, &sha);
    uint8_t result[32];
    memcpy(result, buffer, 32);
    for (int i = 0; i < 31; i++) {
        sha256_begin(&sha);
        sha256_hash(ipad, 64, &sha);
        sha256_hash(buffer, 32, &sha);
        sha256_end(buffer, &sha);
        sha256_begin(&sha);
        sha256_hash(opad, 64, &sha);
        sha256_hash(buffer, 32, &sha);
        sha256_end(buffer, &sha);
        for (int i = 0; i < 8; i++) {
            result[i] ^= buffer[i];
        }
    }

    return memcmp(result, &chunk_id, 8) == 0;
}

BundleList* group_by_bundles(ChunkList* chunks)
{
    BundleList* unique_bundles = malloc(sizeof(BundleList));
    initialize_list(unique_bundles);
    for (uint32_t i = 0; i < chunks->length; i++) {
        Bundle* to_find = NULL;
        find_object_s(unique_bundles, to_find, bundle_id, chunks->objects[i].bundle->bundle_id);
        if (!to_find) {
            Bundle to_add = {.bundle_id = chunks->objects[i].bundle->bundle_id};
            initialize_list(&to_add.chunks);
            add_object_s(&to_add.chunks, &chunks->objects[i], bundle_offset);
            add_object_s(unique_bundles, &to_add, bundle_id);
        } else {
            add_object_s(&to_find->chunks, &chunks->objects[i], bundle_offset);
        }
    }

    return unique_bundles;
}

void free_manifest(Manifest* manifest)
{
    free(manifest->chunks.objects);

    for (uint32_t i = 0; i < manifest->bundles.length; i++) {
        free(manifest->bundles.objects[i].chunks.objects[0].bundle);
        free(manifest->bundles.objects[i].chunks.objects);
    }
    free(manifest->bundles.objects);

    for (uint32_t i = 0; i < manifest->files.length; i++) {
        free(manifest->files.objects[i].link);
        free(manifest->files.objects[i].name);
        free(manifest->files.objects[i].languages.objects);
        free(manifest->files.objects[i].chunks.objects);
    }
    free(manifest->files.objects);

    for (uint32_t i = 0; i < manifest->languages.length; i++) {
        free(manifest->languages.objects[i].name);
    }
    free(manifest->languages.objects);

    free(manifest);
}

char* unpack_string(uint8_t* string_position)
{
    uint32_t string_length = *(uint32_t*) string_position;
    char* string = malloc(string_length + 1);
    memcpy(string, string_position + 4, string_length + 1);
    return string;
}

char* jump_unpack_string(uint8_t* offset_position)
{
    uint8_t* string_position = offset_position + *(uint32_t*) offset_position;
    return unpack_string(string_position);
}

int parse_body(Manifest* manifest, uint8_t* body)
{
    uint32_t offsetsOffset = 4 + *(uint32_t*) body;
    uint32_t offsets[6] = {0};
    for (int i = 0; i < 6; i++) {
        offsets[i] = offsetsOffset + 4*i + *(uint32_t*) (body + offsetsOffset + 4*i);
    }

    uint32_t count;
    uint32_t offset;

    // bundles (and their chunks)
    count = *(uint32_t*) (body + offsets[0]);
    initialize_list_size(&manifest->bundles, count);
    offset = offsets[0] + 4;
    uint32_t total_chunks = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t bundle_offset = offset + *(uint32_t*) (body + offset) + 4;
        int header_length = *(int32_t*) (body + bundle_offset);
        Bundle* new_bundle = malloc(sizeof(Bundle));
        new_bundle->bundle_id = *(uint64_t*) (body + bundle_offset + 4);
        bundle_offset += header_length;

        uint32_t chunk_amount = *(uint32_t*) (body + bundle_offset);
        initialize_list_size(&new_bundle->chunks, chunk_amount);
        for (uint32_t i = 0; i < chunk_amount; i++) {
            uint32_t chunk_offset = bundle_offset + 4*i + *(uint32_t*) (body + bundle_offset + 4 + 4*i) + 4;
            VTable* chunk_vtable = (VTable*) (body + chunk_offset - *(int32_t*) (body + chunk_offset));
            chunk_offset += 4;

            Chunk new_chunk = {
                .compressed_size = *(uint32_t*) (body + chunk_offset),
                .uncompressed_size = *(uint32_t*) (body + chunk_offset + 4),
                .chunk_id = *(uint64_t*) (body + chunk_offset + 8),
                .bundle_offset = new_bundle->chunks.length == 0 ? 0 : new_bundle->chunks.objects[new_bundle->chunks.length - 1].bundle_offset + new_bundle->chunks.objects[new_bundle->chunks.length - 1].compressed_size,
                .bundle = new_bundle
            };
            add_object(&new_bundle->chunks, &new_chunk);
        }
        add_object(&manifest->bundles, new_bundle);
        total_chunks += chunk_amount;
        offset += 4;
    }
    initialize_list_size(&manifest->chunks, total_chunks);
    for (uint32_t i = 0; i < manifest->bundles.length; i++) {
        add_objects(&manifest->chunks, manifest->bundles.objects[i].chunks.objects, manifest->bundles.objects[i].chunks.length);
    }
    sort_list(&manifest->chunks, chunk_id);

    // languages
    count = *(uint32_t*) (body + offsets[1]);
    initialize_list_size(&manifest->languages, count);
    offset = offsets[1] + 4;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t language_offset = offset + *(uint32_t*) (body + offset) + 4;
        Language new_language = {.language_id = body[language_offset + 3], .name = jump_unpack_string(body + language_offset + 4)};
        add_object_s(&manifest->languages, &new_language, language_id);

        offset += 4;
    }

    // file entries
    FileEntryList file_entries;
    count = *(uint32_t*) (body + offsets[2]);
    initialize_list_size(&file_entries, count);
    offset = offsets[2] + 4;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t file_entry_offset = offset + *(uint32_t*) (body + offset);
        VTable* file_entry_vtable = (VTable*) (body + file_entry_offset - *(int32_t*) (body + file_entry_offset));

        FileEntry new_file_entry = {
            .file_entry_id = *(uint64_t*) (body + file_entry_offset + file_entry_vtable->offsets[0]),
            .directory_id = file_entry_vtable->offsets[1] ? *(uint64_t*) (body + file_entry_offset + file_entry_vtable->offsets[1]) : 0,
            .file_size = *(uint32_t*) (body + file_entry_offset + file_entry_vtable->offsets[2]),
            .name = jump_unpack_string(body + file_entry_offset + file_entry_vtable->offsets[3]),
            .link = jump_unpack_string(body + file_entry_offset + file_entry_vtable->offsets[9])
        };
        uint8_t* chunks_position = body + file_entry_offset + file_entry_vtable->offsets[7] + *(uint32_t*) (body + file_entry_offset + file_entry_vtable->offsets[7]);
        initialize_list_size(&new_file_entry.chunk_ids, *(uint32_t*) chunks_position);
        add_objects(&new_file_entry.chunk_ids, chunks_position + 4, *(uint32_t*) chunks_position);
        uint64_t language_mask = file_entry_vtable->offsets[4] ? *(uint64_t*) (body + file_entry_offset + file_entry_vtable->offsets[4]) : 0;
        initialize_list(&new_file_entry.language_ids);
        for (int i = 0; i < 64; i++) {
            if (language_mask & (1ull << i)) {
                add_object(&new_file_entry.language_ids, &(uint8_t) {i+1});
            }
        }
        add_object(&file_entries, &new_file_entry);

        offset += 4;
    }

    // directories
    DirectoryList directories;
    count = *(uint32_t*) (body + offsets[3]);
    initialize_list_size(&directories, count);
    offset = offsets[3] + 4;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t directory_offset = offset + *(uint32_t*) (body + offset);

        int32_t offsettable_offset = directory_offset - *(int32_t*) (body + directory_offset);
        uint16_t* directory_offsets = (uint16_t*) (body + offsettable_offset);
        Directory new_directory = {
            .directory_id = *(uint64_t*) (body + directory_offset + directory_offsets[2]),
            .parent_id = directory_offsets[3] ? *(uint64_t*) (body + directory_offset + directory_offsets[3]) : 0,
            .name = jump_unpack_string(body + directory_offset + directory_offsets[4])
        };
        add_object(&directories, &new_directory);

        offset += 4;
    }

    // merge directories and file_entries together to a list of files
    initialize_list_size(&manifest->files, file_entries.length);
    for (uint32_t i = 0; i < file_entries.length; i++) {
        File new_file = {
            .file_size = file_entries.objects[i].file_size,
            .link = file_entries.objects[i].link
        };
        initialize_list_size(&new_file.languages, file_entries.objects[i].language_ids.length);
        for (uint32_t j = 0; j < file_entries.objects[i].language_ids.length; j++) {
            Language* language = NULL;
            find_object_s(&manifest->languages, language, language_id, file_entries.objects[i].language_ids.objects[j]);
            add_object(&new_file.languages, language);
        }
        uint64_t directory_id = file_entries.objects[i].directory_id;
        char temp_name[256];
        strcpy(temp_name, file_entries.objects[i].name);
        while (directory_id) {
            uint32_t j;
            for (j = 0; j < directories.length; j++) {
                if (directories.objects[j].directory_id == directory_id)
                    break;
            }
            assert(j < directories.length);
            directory_id = directories.objects[j].parent_id;
            char* directory_name = directories.objects[j].name;
            char backup_name[255];
            strcpy(backup_name, temp_name);
            assert(sprintf(temp_name, "%s/%s", directory_name, backup_name) < 256);
        }
        new_file.name = strdup(temp_name);
        initialize_list_size(&new_file.chunks, file_entries.objects[i].chunk_ids.length);
        uint32_t file_offset = 0;
        for (uint32_t j = 0; j < file_entries.objects[i].chunk_ids.length; j++) {
            Chunk* chunk = NULL;
            find_object_s(&manifest->chunks, chunk, chunk_id, file_entries.objects[i].chunk_ids.objects[j]);
            chunk->file_offset = file_offset;
            add_object(&new_file.chunks, chunk);
            file_offset += chunk->uncompressed_size;
        }
        add_object(&manifest->files, &new_file);
    }

    for (uint32_t i = 0; i < file_entries.length; i++) {
        free(file_entries.objects[i].name);
        free(file_entries.objects[i].chunk_ids.objects);
        free(file_entries.objects[i].language_ids.objects);
    }
    free(file_entries.objects);
    for (uint32_t i = 0; i < directories.length; i++) {
        free(directories.objects[i].name);
    }
    free(directories.objects);

    dprintf("amount of chunks in this manifest: %u\n", total_chunks);

    return 0;
}

Manifest* parse_manifest_data(uint8_t* data)
{
    if (strncmp((char*) data, "RMAN", 4)) {
        eprintf("Not a valid RMAN file! Missing magic bytes.\n");
        return NULL;
    }

    if (data[4] == 2 && data[5] != 0) {
        vprintf(1, "Info: Untested manifest version %d.%d detected. Everything should still work though.\n", data[4], data[5]);
    } else if (data[4] != 2) {
        eprintf("Warning: Probably unsupported manifest version %d.%d detected. Will continue, but it might not work.\n", data[4], data[5]);
    }

    Manifest* manifest = malloc(sizeof(Manifest));
    uint32_t contentOffset = *(uint32_t*) (data + 8);
    uint32_t compressedSize = *(uint32_t*) (data + 12);
    manifest->manifest_id = *(uint64_t*) (data + 16);
    uint32_t uncompressedSize = *(uint32_t*) (data + 24);

    uint8_t* uncompressed_body = malloc(uncompressedSize);
    assert(ZSTD_decompress(uncompressed_body, uncompressedSize, data + contentOffset, compressedSize) == uncompressedSize);

    parse_body(manifest, uncompressed_body);
    free(uncompressed_body);

    return manifest;
}

Manifest* parse_manifest_f(char* filepath)
{
    FILE* manifest_file = fopen(filepath, "rb");
    if (!manifest_file) {
        eprintf("Error: Couldn't open manifest file (%s).\n", filepath);
        return NULL;
    }
    // figure out the length of the file, to allocate the exact amount of needed memory
    fseek(manifest_file, 0, SEEK_END);
    int file_size = ftell(manifest_file);
    rewind(manifest_file);
    uint8_t* raw_manifest = malloc(file_size);
    assert(fread(raw_manifest, 1, file_size, manifest_file) == (size_t) file_size);
    fclose(manifest_file);

    Manifest* parsed_manifest = parse_manifest_data(raw_manifest);
    free(raw_manifest);
    return parsed_manifest;
}
