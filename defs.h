#ifndef _DEFS_H
#define _DEFS_H

#include <inttypes.h>

#include "general_utils.h"

int VERBOSE;

typedef LIST(uint8_t) uint8_list;
typedef LIST(uint16_t) uint16_list;
typedef LIST(uint32_t) uint32_list;
typedef LIST(uint64_t) uint64_list;
typedef LIST(__uint128_t) uint128_list;


#endif
