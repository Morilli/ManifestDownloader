/**
 * \file sha2.h
 *
 *  Copyright (C) 2011, Con Kolivas <kernel@kolivas.org>
 *  Copyright (C) 2006-2010, Brainspark B.V.
 *
 *  This file is part of PolarSSL (http://www.polarssl.org)
 *  Lead Maintainer: Paul Bakker <polarssl_maintainer at polarssl.org>
 *
 *  All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#ifndef POLARSSL_SHA2_H
#define POLARSSL_SHA2_H

// file modified; original at https://github.com/pshep/cgminer/blob/master/sha2.h

#include "sha_extension.h"

/**
 * \brief          SHA-256 context structure
 */
typedef struct
{
    uint32_t total[2];     /*!< number of bytes processed  */
    uint32_t state[8];     /*!< intermediate digest state  */
    unsigned char buffer[64];   /*!< data block being processed */
}
sha256_context;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \brief          SHA-256 context setup
 *
 * \param ctx      context to be initialized
 */
void sha256_begin( sha256_context *ctx);

/**
 * \brief          SHA-256 process buffer
 *
 * \param ctx      SHA-256 context
 * \param input    buffer holding the  data
 * \param ilen     length of the input data
 */
void sha256_update( sha256_context *ctx, const unsigned char *input, int ilen );

/**
 * \brief          SHA-256 final digest
 *
 * \param ctx      SHA-256 context
 * \param output   SHA-224/256 checksum result
 */
void sha256_end( sha256_context *ctx, unsigned char output[32] );

/**
 * \brief          Output = SHA-256( input buffer )
 *
 * \param input    buffer holding the  data
 * \param ilen     length of the input data
 * \param output   SHA-224/256 checksum result
 * \param is224    0 = use SHA256, 1 = use SHA224
 */
void sha256( const unsigned char *input, int ilen,
           unsigned char output[32]);

#ifdef __cplusplus
}
#endif

#endif /* sha2.h */
