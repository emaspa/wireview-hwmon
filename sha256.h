/*
 * Minimal SHA-256 + HMAC-SHA256 for wireviewd's write-command auth.
 * Vendored (public domain, Brad Conte) so the daemon needs no OpenSSL.
 * SPDX-License-Identifier: CC0-1.0
 */
#ifndef WIREVIEW_SHA256_H
#define WIREVIEW_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
	uint8_t  data[64];
	uint32_t datalen;
	uint64_t bitlen;
	uint32_t state[8];
} sha256_ctx;

void sha256_init(sha256_ctx *ctx);
void sha256_update(sha256_ctx *ctx, const uint8_t *data, size_t len);
void sha256_final(sha256_ctx *ctx, uint8_t out[32]);

/* HMAC-SHA256(key, msg) -> out[32]. */
void hmac_sha256(const uint8_t *key, size_t keylen,
		 const uint8_t *msg, size_t msglen, uint8_t out[32]);

/* Lowercase-hex encode len bytes into out (needs 2*len+1 bytes). */
void hex_encode(const uint8_t *in, size_t len, char *out);

/* Constant-time compare of two NUL-terminated hex strings; 1 if equal. */
int ct_str_equal(const char *a, const char *b);

#endif /* WIREVIEW_SHA256_H */
