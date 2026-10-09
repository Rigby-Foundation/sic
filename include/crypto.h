/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* What WPA2-Personal needs: SHA-1 and HMAC-SHA1, PBKDF2 (the passphrase
 * to the PMK), the 802.11 PRF (the PMK to the PTK), AES-128 and AES key
 * unwrap (RFC 3394: the group key in message 3 of the handshake). */
#pragma once
#include "types.h"

struct sha1 { uint32_t h[5]; uint64_t len; uint8_t buf[64]; };
void sha1_init(struct sha1 *s);
void sha1_update(struct sha1 *s, const void *data, size_t len);
void sha1_final(struct sha1 *s, uint8_t out[20]);

/* HMAC-SHA1 over n pieces, one after the other */
void hmac_sha1_vec(const uint8_t *key, size_t klen, int n, const void *const *data, const size_t *len, uint8_t out[20]);
void hmac_sha1(const uint8_t *key, size_t klen, const void *data, size_t len, uint8_t out[20]);
void pbkdf2_sha1(const char *pass, const uint8_t *salt, size_t slen, int iters, uint8_t *out, size_t olen);
/* PRF-n: HMAC-SHA1(key, label || 0 || data || counter), counter 0, 1, ... */
void prf_sha1(const uint8_t *key, size_t klen, const char *label, const uint8_t *data, size_t dlen, uint8_t *out, size_t olen);

struct aes128 { uint8_t rk[176]; };
void aes128_key(struct aes128 *a, const uint8_t key[16]);
void aes128_encrypt(const struct aes128 *a, const uint8_t in[16], uint8_t out[16]);
void aes128_decrypt(const struct aes128 *a, const uint8_t in[16], uint8_t out[16]);
/* RFC 3394: n 64-bit blocks of key data from n + 1 of cipher text; -1 if the check fails */
int aes_unwrap(const uint8_t kek[16], int n, const uint8_t *cipher, uint8_t *plain);
