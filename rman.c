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
        free(manifest->files.objects[i].language_ids.objects);
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
    initialize_list(&manifest->bundles);
    initialize_list(&manifest->chunks);
    count = *(uint32_t*) (body + offsets[0]);
    offset = offsets[0] + 4;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t bundle_offset = offset + *(uint32_t*) (body + offset) + 4;
        int header_length = *(int32_t*) (body + bundle_offset);
        Bundle* new_bundle = malloc(sizeof(Bundle));
        new_bundle->bundle_id = *(uint64_t*) (body + bundle_offset + 4);
        bundle_offset += header_length;
        // for (int i = 0; i < 64; i++) {
        //     printf("%02X ", body[bundle_offset + i]);
        // }
        // printf("\n");

        initialize_list(&new_bundle->chunks);
        uint32_t chunk_amount = *(uint32_t*) (body + bundle_offset);
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
            add_object(&manifest->chunks, &new_chunk);
        }
        add_object(&manifest->bundles, new_bundle);
        offset += 4;
    }

    // languages
    initialize_list(&manifest->languages);
    count = *(uint32_t*) (body + offsets[1]);
    offset = offsets[1] + 4;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t language_offset = offset + *(uint32_t*) (body + offset) + 4;
        // for (int i = 0; i < 64; i++) {
        //     printf("%02X ", body[language_offset + i]);
        // }
        // printf("\n");
        Language new_language = {.language_id = body[language_offset + 3], .name = jump_unpack_string(body + language_offset + 4)};
        add_object(&manifest->languages, &new_language);

        offset += 4;
    }
    // for (int i = 0; i < manifest->languages.length; i++) {
        // printf("language at %d: %s\n", i, manifest->languages.objects[i].name);
    // }

    dprintf("started sorting...\n");
    sort_list(&manifest->chunks, chunk_id);
    // for (uint32_t i = 0; i < manifest->chunks.length; i++) {
        // printf("chunk_id: %016"PRIX64"\n", manifest->chunks.objects[i].chunk_id);
    // }
    // exit(EXIT_FAILURE);
    dprintf("finished sorting\n");

    // file entries
    FileEntryList file_entries;
    initialize_list(&file_entries);
    count = *(uint32_t*) (body + offsets[2]);
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
        initialize_list(&new_file_entry.chunk_ids);
        uint8_t* chunks_position = body + file_entry_offset + file_entry_vtable->offsets[7] + *(uint32_t*) (body + file_entry_offset + file_entry_vtable->offsets[7]);
        add_objects(&new_file_entry.chunk_ids, chunks_position + 4, *(uint32_t*) chunks_position);
        uint64_t language_mask = *(uint64_t*) (body + file_entry_offset + file_entry_vtable->offsets[4]);
        initialize_list(&new_file_entry.language_ids);
        for (int i = 0; i < 64; i++) {
            if (language_mask & (1 << i))
                add_object(&new_file_entry.language_ids, &(uint8_t) {i+1});
        }
        add_object(&file_entries, &new_file_entry);

        offset += 4;
    }

    // directories
    DirectoryList directories = {0};
    initialize_list(&directories);
    count = *(uint32_t*) (body + offsets[3]);
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
    initialize_list(&manifest->files);
    for (uint32_t i = 0; i < file_entries.length; i++) {
        File new_file = {
            .file_size = file_entries.objects[i].file_size,
            .link = file_entries.objects[i].link,
            .language_ids = file_entries.objects[i].language_ids
        };
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
        initialize_list(&new_file.chunks);
        uint32_t file_offset = 0;
        for (uint32_t j = 0; j < file_entries.objects[i].chunk_ids.length; j++) {
            Chunk* chunk;
            find_object_s(&manifest->chunks, chunk, chunk_id, file_entries.objects[i].chunk_ids.objects[j]);
            chunk->file_offset = file_offset;
            add_object(&new_file.chunks, chunk);
            file_offset += chunk->uncompressed_size;
        }
        add_object(&manifest->files, &new_file);

        dprintf("final file name: \"%s\"\n", new_file.name);
    }

    for (uint32_t i = 0; i < file_entries.length; i++) {
        free(file_entries.objects[i].name);
        free(file_entries.objects[i].chunk_ids.objects);
    }
    free(file_entries.objects);
    for (uint32_t i = 0; i < directories.length; i++) {
        free(directories.objects[i].name);
    }
    free(directories.objects);


    int chunks_amount = 0;
    for (uint32_t i = 0; i < manifest->bundles.length; i++) {
        chunks_amount += manifest->bundles.objects[i].chunks.length;
    }
    dprintf("amount of chunks in this manifest: %d\n", chunks_amount);

    return 0;
}

Manifest* parse_manifest(char* filepath)
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

    if (strncmp((char*) raw_manifest, "RMAN", 4)) {
        eprintf("Not a valid RMAN file! Missing magic bytes.\n");
        return NULL;
    }

    if (*(uint16_t*) (raw_manifest + 4) != 2) {
        eprintf("Unsupposed RMAN version: %d.%d\n", raw_manifest[4], raw_manifest[5]);
        return NULL;
    }

    Manifest* manifest = malloc(sizeof(Manifest));
    uint32_t contentOffset = *(uint32_t*) (raw_manifest + 8);
    uint32_t compressedSize = *(uint32_t*) (raw_manifest + 12);
    manifest->manifest_id = *(uint64_t*) (raw_manifest + 16);
    uint32_t uncompressedSize = *(uint32_t*) (raw_manifest + 24);
    // printf("content offset: %d, compressed size: %d, uncompressed size: %d, manifest id: %llX\n", contentOffset, compressedSize, uncompressedSize, manifest->manifest_id);

    uint8_t* uncompressed_body = malloc(uncompressedSize);
    assert(ZSTD_decompress(uncompressed_body, uncompressedSize, raw_manifest + contentOffset, compressedSize) == uncompressedSize);
    free(raw_manifest);

    parse_body(manifest, uncompressed_body);
    free(uncompressed_body);

    return manifest;
}
