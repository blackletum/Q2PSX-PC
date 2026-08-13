/*
 * sha256.h — FIPS 180-4 SHA-256.
 *
 * Used to fingerprint a disc's boot executable. Two releases can share a serial
 * but differ in revision, so the serial alone is not enough to pick the right
 * data tables; the executable hash is.
 */
#ifndef Q2PSX_SHA256_H
#define Q2PSX_SHA256_H

#include "q2psx.h"

#define SHA256_DIGEST_SIZE 32
#define SHA256_HEX_SIZE    (SHA256_DIGEST_SIZE * 2 + 1)

typedef struct sha256_ctx {
    u32 state[8];
    u64 bit_count;
    u8  block[64];
    u32 block_len;
} sha256_ctx;

void sha256_init(sha256_ctx *ctx);
void sha256_update(sha256_ctx *ctx, const void *data, size_t len);
void sha256_final(sha256_ctx *ctx, u8 out[SHA256_DIGEST_SIZE]);

void sha256_buffer(const void *data, size_t len, u8 out[SHA256_DIGEST_SIZE]);
void sha256_hex(const u8 digest[SHA256_DIGEST_SIZE], char out[SHA256_HEX_SIZE]);

#endif /* Q2PSX_SHA256_H */
