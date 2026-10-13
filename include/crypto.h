/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* What WPA2- and WPA3-Personal need: SHA-1/HMAC-SHA1, PBKDF2 (the
 * passphrase to the PMK), the 802.11 PRF (the PMK to the PTK), AES-128 and
 * AES key unwrap (RFC 3394: the group key in the handshake); and for the
 * SHA-256 AKMs and SAE: SHA-256/HMAC-SHA256, the 802.11 KDF, AES-128-CMAC
 * (the MIC and BIP), and NIST P-256 (the dragonfly key exchange). */
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
/* AES-128-CMAC (RFC 4493 / NIST OMAC1): the EAPOL-Key MIC for the SHA-256
 * AKMs, and BIP for protected management frames */
void aes_cmac(const uint8_t key[16], const void *data, size_t len, uint8_t mac[16]);

/* SHA-256, HMAC-SHA256, the 802.11 KDF: the SHA-256 AKMs (PSK-SHA256 and
 * SAE) and WPA3's key schedule. */
struct sha256 { uint32_t h[8]; uint64_t len; uint8_t buf[64]; };
void sha256_init(struct sha256 *s);
void sha256_update(struct sha256 *s, const void *data, size_t len);
void sha256_final(struct sha256 *s, uint8_t out[32]);
void hmac_sha256_vec(const uint8_t *key, size_t klen, int n, const void *const *data, const size_t *len, uint8_t out[32]);
void hmac_sha256(const uint8_t *key, size_t klen, const void *data, size_t len, uint8_t out[32]);
/* IEEE 802.11 KDF: HMAC-SHA256(key, i_le16 || label || context || bits_le16), i = 1.. */
void kdf_sha256(const uint8_t *key, size_t klen, const char *label, const uint8_t *ctx, size_t clen, uint8_t *out, size_t obits);

/* NIST P-256 (secp256r1) for SAE/dragonfly. Points are x||y, 64 bytes
 * big-endian; the all-zero value is the point at infinity. Scalars are 32
 * bytes big-endian, taken mod the group order. */
int  p256_point_valid(const uint8_t p[64]);              /* on the curve and not infinity */
void p256_mul(const uint8_t k[32], const uint8_t p[64], uint8_t out[64]);   /* k*P */
void p256_add(const uint8_t a[64], const uint8_t b[64], uint8_t out[64]);
void p256_negate(const uint8_t in[64], uint8_t out[64]);
void p256_scalar_add(const uint8_t a[32], const uint8_t b[32], uint8_t out[32]);   /* (a + b) mod n */
int  p256_scalar_rand(uint8_t out[32]);                  /* random in [1, n-1]; 0 on success */
/* SAE PWE for group 19 by hunting and pecking: the password element from
 * the passphrase and the two MAC addresses; 0 on success */
int  p256_sae_pwe(const char *pass, const uint8_t a[6], const uint8_t b[6], uint8_t pwe[64]);
int  p256_selftest(void);                                /* known-answer checks; 0 ok */
