/* armv8-chacha.c
 *
 * Copyright (C) 2006-2017 wolfSSL Inc.
 *
 * This file is part of wolfSSL.
 *
 * wolfSSL is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * wolfSSL is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335, USA
 *
 */


#ifdef WOLFSSL_ARMASM

#ifdef HAVE_CONFIG_H
    #include <config.h>
#endif

#include <wolfssl/wolfcrypt/settings.h>

#ifdef HAVE_CHACHA

#include <wolfssl/wolfcrypt/chacha.h>
#include <wolfssl/wolfcrypt/error-crypt.h>
#include <wolfssl/wolfcrypt/logging.h>
#include <wolfssl/wolfcrypt/cpuid.h>
#ifdef NO_INLINE
    #include <wolfssl/wolfcrypt/misc.h>
#else
    #define WOLFSSL_MISC_INCLUDED
    #include <wolfcrypt/src/misc.c>
#endif

#ifdef CHACHA_AEAD_TEST
    #include <stdio.h>
#endif

#ifdef BIG_ENDIAN_ORDER
    #define LITTLE32(x) ByteReverseWord32(x)
#else
    #define LITTLE32(x) (x)
#endif

/* Number of rounds */
#define ROUNDS  20

#define U32C(v) (v##U)
#define U32V(v) ((word32)(v) & U32C(0xFFFFFFFF))
#define U8TO32_LITTLE(p) LITTLE32(((word32*)(p))[0])

#define ROTATE(v,c) rotlFixed(v, c)
#define XOR(v,w)    ((v) ^ (w))
#define PLUS(v,w)   (U32V((v) + (w)))
#define PLUSONE(v)  (PLUS((v),1))

#define QUARTERROUND(a,b,c,d) \
  x[a] = PLUS(x[a],x[b]); x[d] = ROTATE(XOR(x[d],x[a]),16); \
  x[c] = PLUS(x[c],x[d]); x[b] = ROTATE(XOR(x[b],x[c]),12); \
  x[a] = PLUS(x[a],x[b]); x[d] = ROTATE(XOR(x[d],x[a]), 8); \
  x[c] = PLUS(x[c],x[d]); x[b] = ROTATE(XOR(x[b],x[c]), 7);

#define ARM_SIMD_LEN_BYTES 16

/**
  * Set up iv(nonce). Earlier versions used 64 bits instead of 96, this version
  * uses the typical AEAD 96 bit nonce and can do record sizes of 256 GB.
  */
int wc_Chacha_SetIV(ChaCha* ctx, const byte* inIv, word32 counter)
{
    word32 temp[CHACHA_IV_WORDS];/* used for alignment of memory */

#ifdef CHACHA_AEAD_TEST
    word32 i;
    printf("NONCE : ");
    for (i = 0; i < CHACHA_IV_BYTES; i++) {
        printf("%02x", inIv[i]);
    }
    printf("\n\n");
#endif

    if (ctx == NULL)
        return BAD_FUNC_ARG;

    XMEMCPY(temp, inIv, CHACHA_IV_BYTES);

    ctx->X[CHACHA_IV_BYTES+0] = counter;           /* block counter */
    ctx->X[CHACHA_IV_BYTES+1] = LITTLE32(temp[0]); /* fixed variable from nonce */
    ctx->X[CHACHA_IV_BYTES+2] = LITTLE32(temp[1]); /* counter from nonce */
    ctx->X[CHACHA_IV_BYTES+3] = LITTLE32(temp[2]); /* counter from nonce */

    return 0;
}

/* "expand 32-byte k" as unsigned 32 byte */
static const word32 sigma[4] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};
/* "expand 16-byte k" as unsigned 16 byte */
static const word32 tau[4] = {0x61707865, 0x3120646e, 0x79622d36, 0x6b206574};

/**
  * Key setup. 8 word iv (nonce)
  */
int wc_Chacha_SetKey(ChaCha* ctx, const byte* key, word32 keySz)
{
    const word32* constants;
    const byte*   k;

#ifdef XSTREAM_ALIGN
    word32 alignKey[8];
#endif

    if (ctx == NULL)
        return BAD_FUNC_ARG;

    if (keySz != (CHACHA_MAX_KEY_SZ/2) && keySz != CHACHA_MAX_KEY_SZ)
        return BAD_FUNC_ARG;

#ifdef XSTREAM_ALIGN
    if ((wolfssl_word)key % 4) {
        WOLFSSL_MSG("wc_ChachaSetKey unaligned key");
        XMEMCPY(alignKey, key, keySz);
        k = (byte*)alignKey;
    }
    else {
        k = key;
    }
#else
    k = key;
#endif /* XSTREAM_ALIGN */

#ifdef CHACHA_AEAD_TEST
    word32 i;
    printf("ChaCha key used :\n");
    for (i = 0; i < keySz; i++) {
        printf("%02x", key[i]);
        if ((i + 1) % 8 == 0)
           printf("\n");
    }
    printf("\n\n");
#endif

    ctx->X[4] = U8TO32_LITTLE(k +  0);
    ctx->X[5] = U8TO32_LITTLE(k +  4);
    ctx->X[6] = U8TO32_LITTLE(k +  8);
    ctx->X[7] = U8TO32_LITTLE(k + 12);
    if (keySz == CHACHA_MAX_KEY_SZ) {
        k += 16;
        constants = sigma;
    }
    else {
        constants = tau;
    }
    ctx->X[ 8] = U8TO32_LITTLE(k +  0);
    ctx->X[ 9] = U8TO32_LITTLE(k +  4);
    ctx->X[10] = U8TO32_LITTLE(k +  8);
    ctx->X[11] = U8TO32_LITTLE(k + 12);
    ctx->X[ 0] = constants[0];
    ctx->X[ 1] = constants[1];
    ctx->X[ 2] = constants[2];
    ctx->X[ 3] = constants[3];

    return 0;
}

/**
  * Converts word into bytes with rotations having been done.
  */
static WC_INLINE void wc_Chacha_wordtobyte(word32 output[CHACHA_CHUNK_WORDS],
    const word32 input[CHACHA_CHUNK_WORDS])
{
    word32 x[CHACHA_CHUNK_WORDS];
    word32 i;

    XMEMCPY(x, input, CHACHA_CHUNK_BYTES);

    __asm__ __volatile__ (
            // Load counter
            "MOV x0, %[rounds] \n"

            // v0  0  1  2  3
            // v1  4  5  6  7
            // v2  8  9 10 11
            // v3 12 13 14 15
            // load CHACHA state as shown above
            "LD1 { v0.4S-v3.4S }, %[x_in] \n"

            "loop: \n"

            // ODD ROUND

            "ADD v0.4S, v0.4S, v1.4S \n"
            "EOR v3.16B, v3.16B, v0.16B \n"
            // SIMD instructions don't support rotation so we have to cheat using shifts and a help register
            "SHL v4.4S, v3.4S, #16 \n"
            "USHR v3.4S, v3.4S, #16 \n"
            "ORR v3.16B, v3.16B, v4.16B \n"

            "ADD v2.4S, v2.4S, v3.4S \n"
            "EOR v1.16B, v1.16B, v2.16B \n"
            // SIMD instructions don't support rotation so we have to cheat using shifts and a help register
            "SHL v4.4S, v1.4S, #12 \n"
            "USHR v1.4S, v1.4S, #20 \n"
            "ORR v1.16B, v1.16B, v4.16B \n"

            "ADD v0.4S, v0.4S, v1.4S \n"
            "EOR v3.16B, v3.16B, v0.16B \n"
            // SIMD instructions don't support rotation so we have to cheat using shifts and a help register
            "SHL v4.4S, v3.4S, #8 \n"
            "USHR v3.4S, v3.4S, #24 \n"
            "ORR v3.16B, v3.16B, v4.16B \n"

            "ADD v2.4S, v2.4S, v3.4S \n"
            "EOR v1.16B, v1.16B, v2.16B \n"
            // SIMD instructions don't support rotation so we have to cheat using shifts and a help register
            "SHL v4.4S, v1.4S, #7 \n"
            "USHR v1.4S, v1.4S, #25 \n"
            "ORR v1.16B, v1.16B, v4.16B \n"

            // EVEN ROUND

            // v0   0  1  2  3
            // v1   5  6  7  4
            // v2  10 11  8  9
            // v3  15 12 13 14
            // CHACHA block vector elements shifted as shown above

            "EXT v1.16B, v1.16B, v1.16B, #4 \n" // permute elements left by one
            "EXT v2.16B, v2.16B, v2.16B, #8 \n" // permute elements left by two
            "EXT v3.16B, v3.16B, v3.16B, #12 \n" // permute elements left by three

            "ADD v0.4S, v0.4S, v1.4S \n"
            "EOR v3.16B, v3.16B, v0.16B \n"
            // SIMD instructions don't support rotation so we have to cheat using shifts and a help register
            "SHL v4.4S, v3.4S, #16 \n"
            "USHR v3.4S, v3.4S, #16 \n"
            "ORR v3.16B, v3.16B, v4.16B \n"

            "ADD v2.4S, v2.4S, v3.4S \n"
            "EOR v1.16B, v1.16B, v2.16B \n"
            // SIMD instructions don't support rotation so we have to cheat using shifts and a help register
            "SHL v4.4S, v1.4S, #12 \n"
            "USHR v1.4S, v1.4S, #20 \n"
            "ORR v1.16B, v1.16B, v4.16B \n"

            "ADD v0.4S, v0.4S, v1.4S \n"
            "EOR v3.16B, v3.16B, v0.16B \n"
            // SIMD instructions don't support rotation so we have to cheat using shifts and a help register
            "SHL v4.4S, v3.4S, #8 \n"
            "USHR v3.4S, v3.4S, #24 \n"
            "ORR v3.16B, v3.16B, v4.16B \n"

            "ADD v2.4S, v2.4S, v3.4S \n"
            "EOR v1.16B, v1.16B, v2.16B \n"
            // SIMD instructions don't support rotation so we have to cheat using shifts and a help register
            "SHL v4.4S, v1.4S, #7 \n"
            "USHR v1.4S, v1.4S, #25 \n"
            "ORR v1.16B, v1.16B, v4.16B \n"

            "EXT v1.16B, v1.16B, v1.16B, #12 \n" // permute elements left by three
            "EXT v2.16B, v2.16B, v2.16B, #8 \n" // permute elements left by two
            "EXT v3.16B, v3.16B, v3.16B, #4 \n" // permute elements left by one

            "SUB x0, x0, #1 \n"
            "CBNZ x0, loop \n"

            "LD1 { v4.4S-v7.4S }, [%[in]] \n"

            "ADD v0.4S, v0.4S, v4.4S \n"
            "ADD v1.4S, v1.4S, v5.4S \n"
            "ADD v2.4S, v2.4S, v6.4S \n"
            "ADD v3.4S, v3.4S, v7.4S \n"

            "ST1 { v0.4S-v3.4S }, %[x_out] \n"

            : [x_out] "=m" (x)
            : [x_in] "m" (x), [rounds] "I" (ROUNDS/2), [in] "r" (input)
            : "memory",
              "x0",
              "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7"
    );

    for (i = 0; i < CHACHA_CHUNK_WORDS; i++) {
        output[i] = LITTLE32(x[i]);
    }
}

/**
  * Encrypt a stream of bytes
  */
static void wc_Chacha_encrypt_bytes(ChaCha* ctx, const byte* m, byte* c,
                                    word32 bytes)
{
    byte*  output;
    word32 temp[CHACHA_CHUNK_WORDS]; /* used to make sure aligned */
    word32 i;

    output = (byte*)temp;

    for (; bytes > 0;) {
        wc_Chacha_wordtobyte(temp, ctx->X);
        ctx->X[CHACHA_IV_BYTES] = PLUSONE(ctx->X[CHACHA_IV_BYTES]);
        if (bytes <= CHACHA_CHUNK_BYTES) {

            while (bytes >= ARM_SIMD_LEN_BYTES) {
                __asm__ __volatile__ (
                        "LD1 { v0.16B }, [%[m]] \n"
                        "LD1 { v1.16B }, [%[output]] \n"
                        "EOR v0.16B, v0.16B, v1.16B \n"
                        "ST1 { v0.16B }, [%[c]] \n"
                        : [c] "=r" (c)
                        : "0" (c), [m] "r" (m), [output] "r" (output)
                        : "memory", "v0", "v1"
                );

                bytes -= ARM_SIMD_LEN_BYTES;
                c += ARM_SIMD_LEN_BYTES;
                m += ARM_SIMD_LEN_BYTES;
                output += ARM_SIMD_LEN_BYTES;
            }

            if (bytes >= ARM_SIMD_LEN_BYTES / 2) {
                __asm__ __volatile__ (
                        "LD1 { v0.8B }, [%[m]] \n"
                        "LD1 { v1.8B }, [%[output]] \n"
                        "EOR v0.8B, v0.8B, v1.8B \n"
                        "ST1 { v0.8B }, [%[c]] \n"
                        : [c] "=r" (c)
                        : "0" (c), [m] "r" (m), [output] "r" (output)
                        : "memory", "v0", "v1"
                );

                bytes -= ARM_SIMD_LEN_BYTES / 2;
                c += ARM_SIMD_LEN_BYTES / 2;
                m += ARM_SIMD_LEN_BYTES / 2;
                output += ARM_SIMD_LEN_BYTES / 2;
            }

            for (i = 0; i < bytes; ++i) {
                c[i] = m[i] ^ output[i];
            }

            return;
        }

        // assume CHACHA_CHUNK_BYTES == 64
        __asm__ __volatile__ (
                "LD1 { v0.16B-v3.16B }, [%[m]] \n"
                "LD1 { v4.16B-v7.16B }, [%[output]] \n"
                "EOR v0.16B, v0.16B, v4.16B \n"
                "EOR v1.16B, v1.16B, v5.16B \n"
                "EOR v2.16B, v2.16B, v6.16B \n"
                "EOR v3.16B, v3.16B, v7.16B \n"
                "ST1 { v0.16B-v3.16B }, [%[c]] \n"
                : [c] "=r" (c)
                : "0" (c), [m] "r" (m), [output] "r" (output)
                : "memory", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7"
        );

        bytes -= CHACHA_CHUNK_BYTES;
        c += CHACHA_CHUNK_BYTES;
        m += CHACHA_CHUNK_BYTES;
    }
}

/**
  * API to encrypt/decrypt a message of any size.
  */
int wc_Chacha_Process(ChaCha* ctx, byte* output, const byte* input,
                      word32 msglen)
{
    if (ctx == NULL)
        return BAD_FUNC_ARG;

    wc_Chacha_encrypt_bytes(ctx, input, output, msglen);

    return 0;
}

#endif /* HAVE_CHACHA*/

#endif /* WOLFSSL_ARMASM */
