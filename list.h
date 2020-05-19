#ifndef LIST_H
#define LIST_H

#include <inttypes.h>
#include <stdlib.h>
#include "defs.h"

#define LIST(type) struct { \
    uint32_t length; \
    uint32_t allocated_length; \
    type* objects; \
}

#define initialize_list(list) do { \
    (list)->length = 0; \
    (list)->allocated_length = 16; \
    (list)->objects = malloc(16 * sizeof((list)->objects[0])); \
} while (0)

#define initialize_list_size(list, size) do { \
    (list)->length = 0; \
    (list)->allocated_length = size; \
    (list)->objects = malloc((size) * sizeof((list)->objects[0])); \
} while (0)

#define add_object(list, object) do { \
    if ((list)->length == (list)->allocated_length) { \
        (list)->objects = realloc((list)->objects, ((list)->allocated_length + ((list)->allocated_length >> 1)) * sizeof(*object)); \
        (list)->allocated_length += (list)->allocated_length >> 1; \
    } \
 \
    (list)->objects[(list)->length] = *object; \
    (list)->length++; \
} while (0)

#define remove_object(list, index) do { \
    (list)->objects[index] = (list)->objects[(list)->length - 1]; \
    (list)->length--; \
    if ((list)->allocated_length != 16 && (list)->length == (list)->allocated_length >> 1) { \
        (list)->objects = realloc((list)->objects, max((list)->length + ((list)->length >> 1), (uint32_t) 16) * sizeof(typeof((list)->objects[0]))); \
    } \
} while (0)

#define add_objects(list, position, amount) do { \
    if ((list)->allocated_length - ((list)->length) < (amount)) { \
        (list)->objects = realloc((list)->objects, ((list)->length + (amount)) * sizeof((list)->objects[0])); \
        (list)->allocated_length = (list)->length + (amount); \
    } \
 \
    memcpy(&(list)->objects[(list)->length], position, (amount) * sizeof((list)->objects[0])); \
    (list)->length += amount; \
} while (0)

#define find_object_s(list, out_object, key, value) do { \
    if ((list)->length != 0) { \
        uint32_t position = (list)->length / 2; \
        for (int step = position / 2; step > 2; step /= 2) { \
            if ((list)->objects[position].key > (value)) \
                position -= step; \
            else \
                position += step; \
        } \
        while (position > 0 && (list)->objects[position].key > (value)) \
            position--; \
        while (position < (list)->length-1 && (list)->objects[position].key < (value)) \
            position++; \
        if ((list)->objects[position].key == (value)) \
            (out_object) = &(list)->objects[position]; \
    } \
} while (0)


#define add_object_s(list, object, key) do { \
    if ((list)->length == 0) { \
        (list)->objects[(list)->length] = *object; \
        (list)->length++; \
    } else { \
        if ((list)->length == (list)->allocated_length) { \
            (list)->objects = realloc((list)->objects, ((list)->allocated_length + ((list)->allocated_length >> 1)) * sizeof(*object)); \
            (list)->allocated_length += (list)->allocated_length >> 1; \
        } \
    \
        uint32_t position = (list)->length / 2; \
        for (int step = position / 2; step > 2; step /= 2) { \
            if ((list)->objects[position].key > (object)->key) \
                position -= step; \
            else \
                position += step; \
        } \
        while (position > 0 && (list)->objects[position].key > (object)->key) \
            position--; \
        while (position < (list)->length && (list)->objects[position].key < (object)->key) \
            position++; \
        if (position < (list)->length) \
            memmove(&(list)->objects[position + 1], &(list)->objects[position], ((list)->length - position) * sizeof(*object)); \
        (list)->objects[position] = *object; \
        (list)->length++; \
    } \
} while (0)

#define sort_list(list, key) do { \
    int n = (list)->length; \
    __typeof__(list) temp = malloc(sizeof(__typeof__(*list))); \
    temp->length = (list)->length; temp->allocated_length = (list)->allocated_length; \
    temp->objects = malloc((list)->allocated_length * sizeof(__typeof__((list)->objects[0]))); \
    memcpy(temp->objects, (list)->objects, (list)->length * sizeof(__typeof__((list)->objects[0]))); \
    int parity = 0; \
    for (int width = 1; width < n; width = 2 * width) { \
        parity++; \
        for (int i = 0; i < n; i = i + 2 * width) { \
            if (parity % 2 == 0) \
                sortHelper(list, i, min(i+width, n), min(i+2*width, n), temp, key); \
            else \
                sortHelper(temp, i, min(i+width, n), min(i+2*width, n), list, key); \
        } \
    } \
    if (parity % 2 == 0) { \
        free((list)->objects); \
        (list)->objects = temp->objects; \
        free(temp); \
    } else { \
        free(temp->objects); \
        free(temp); \
    } \
} while (0)

#define sortHelper(listLeft, iLeft, iRight, iEnd, listRight, key) do { \
    int ___i = iLeft, ___j = iRight; \
    for (int ___k = iLeft; ___k < iEnd; ___k++) { \
        if (___i < iRight && (___j >= iEnd || (listLeft)->objects[___i].key <= (listLeft)->objects[___j].key)) { \
            (listRight)->objects[___k] = (listLeft)->objects[___i]; \
            ___i++; \
        } else { \
            (listRight)->objects[___k] = (listLeft)->objects[___j]; \
            ___j++; \
        } \
    } \
} while (0)

typedef LIST(uint8_t) uint8_list;
typedef LIST(uint16_t) uint16_list;
typedef LIST(uint32_t) uint32_list;
typedef LIST(uint64_t) uint64_list;
#ifdef __SIZEOF_INT128__
typedef LIST(__uint128_t) uint128_list;
#endif

#endif
