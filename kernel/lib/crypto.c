/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* SHA-1 (FIPS 180-4), HMAC (RFC 2104), PBKDF2 (RFC 8018), the 802.11 PRF,
 * AES-128 (FIPS 197) and AES key unwrap (RFC 3394). Small and plain: a
 * handshake runs them a few times, not per packet (the radio encrypts). */
#include "crypto.h"
#include "string.h"

/* ---- SHA-1 ------------------------------------------------------------------------------ */

static inline uint32_t rol(uint32_t v, int n) { return v << n | v >> (32 - n); }

static void sha1_block(struct sha1 *s, const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
        else { f = b ^ c ^ d; k = 0xca62c1d6; }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}

void sha1_init(struct sha1 *s)
{
    static const uint32_t iv[5] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 };
    memcpy(s->h, iv, sizeof iv);
    s->len = 0;
}

void sha1_update(struct sha1 *s, const void *data, size_t len)
{
    const uint8_t *p = data;
    size_t have = s->len % 64;
    s->len += len;
    if (have) {
        size_t n = 64 - have < len ? 64 - have : len;
        memcpy(s->buf + have, p, n);
        p += n; len -= n;
        if (have + n < 64) return;
        sha1_block(s, s->buf);
    }
    for (; len >= 64; p += 64, len -= 64) sha1_block(s, p);
    memcpy(s->buf, p, len);
}

void sha1_final(struct sha1 *s, uint8_t out[20])
{
    uint64_t bits = s->len * 8;
    uint8_t pad[72] = { 0x80 };
    size_t n = (s->len % 64 < 56 ? 56 : 120) - s->len % 64;
    for (int i = 0; i < 8; i++) pad[n + i] = (uint8_t)(bits >> (56 - 8 * i));
    sha1_update(s, pad, n + 8);
    for (int i = 0; i < 20; i++) out[i] = (uint8_t)(s->h[i / 4] >> (24 - 8 * (i % 4)));
}

/* ---- HMAC, PBKDF2, PRF ------------------------------------------------------------------- */

void hmac_sha1_vec(const uint8_t *key, size_t klen, int n, const void *const *data, const size_t *len, uint8_t out[20])
{
    uint8_t k[64] = { 0 }, pad[64], inner[20];
    struct sha1 s;
    if (klen > 64) { sha1_init(&s); sha1_update(&s, key, klen); sha1_final(&s, k); }
    else memcpy(k, key, klen);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    sha1_init(&s);
    sha1_update(&s, pad, 64);
    for (int i = 0; i < n; i++) sha1_update(&s, data[i], len[i]);
    sha1_final(&s, inner);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    sha1_init(&s);
    sha1_update(&s, pad, 64);
    sha1_update(&s, inner, 20);
    sha1_final(&s, out);
}

void hmac_sha1(const uint8_t *key, size_t klen, const void *data, size_t len, uint8_t out[20])
{
    hmac_sha1_vec(key, klen, 1, &data, &len, out);
}

void pbkdf2_sha1(const char *pass, const uint8_t *salt, size_t slen, int iters, uint8_t *out, size_t olen)
{
    size_t plen = strlen(pass);
    for (uint32_t blk = 1; olen; blk++) {
        uint8_t be[4] = { (uint8_t)(blk >> 24), (uint8_t)(blk >> 16), (uint8_t)(blk >> 8), (uint8_t)blk };
        const void *d[2] = { salt, be };
        size_t l[2] = { slen, 4 };
        uint8_t u[20], t[20];
        hmac_sha1_vec((const uint8_t *)pass, plen, 2, d, l, u);
        memcpy(t, u, 20);
        for (int i = 1; i < iters; i++) {
            hmac_sha1((const uint8_t *)pass, plen, u, 20, u);
            for (int j = 0; j < 20; j++) t[j] ^= u[j];
        }
        size_t n = olen < 20 ? olen : 20;
        memcpy(out, t, n);
        out += n; olen -= n;
    }
}

void prf_sha1(const uint8_t *key, size_t klen, const char *label, const uint8_t *data, size_t dlen, uint8_t *out, size_t olen)
{
    uint8_t zero = 0, h[20];
    for (uint8_t ctr = 0; olen; ctr++) {
        const void *d[4] = { label, &zero, data, &ctr };
        size_t l[4] = { strlen(label), 1, dlen, 1 };
        hmac_sha1_vec(key, klen, 4, d, l, h);
        size_t n = olen < 20 ? olen : 20;
        memcpy(out, h, n);
        out += n; olen -= n;
    }
}

/* ---- AES-128 ---------------------------------------------------------------------------- */

static uint8_t sbox[256], isbox[256];

static uint8_t xt(uint8_t a) { return (uint8_t)(a << 1 ^ (a & 0x80 ? 0x1b : 0)); }
static uint8_t gmul(uint8_t a, uint8_t b)
{
    uint8_t r = 0;
    for (; b; b >>= 1, a = xt(a)) if (b & 1) r ^= a;
    return r;
}

/* The S-box from its definition: the inverse in GF(2^8), then the affine map. */
static void tables(void)
{
    if (sbox[0]) return;
    uint8_t p = 1, q = 1;
    do {
        p = p ^ xt(p);                                      /* p *= 3 */
        q ^= q << 1; q ^= q << 2; q ^= q << 4;              /* q /= 3 */
        if (q & 0x80) q ^= 0x09;
        uint8_t x = (uint8_t)(q ^ (uint8_t)(q << 1 | q >> 7) ^ (uint8_t)(q << 2 | q >> 6) ^ (uint8_t)(q << 3 | q >> 5) ^ (uint8_t)(q << 4 | q >> 4)) ^ 0x63;
        sbox[p] = x;
    } while (p != 1);
    sbox[0] = 0x63;
    for (int i = 0; i < 256; i++) isbox[sbox[i]] = (uint8_t)i;
}

void aes128_key(struct aes128 *a, const uint8_t key[16])
{
    tables();
    uint8_t *w = a->rk, rc = 1;
    memcpy(w, key, 16);
    for (int i = 16; i < 176; i += 4) {
        uint8_t t[4] = { w[i - 4], w[i - 3], w[i - 2], w[i - 1] };
        if (i % 16 == 0) {
            uint8_t u = t[0];
            t[0] = sbox[t[1]] ^ rc; t[1] = sbox[t[2]]; t[2] = sbox[t[3]]; t[3] = sbox[u];
            rc = xt(rc);
        }
        for (int j = 0; j < 4; j++) w[i + j] = w[i - 16 + j] ^ t[j];
    }
}

static void add_key(uint8_t s[16], const uint8_t *k) { for (int i = 0; i < 16; i++) s[i] ^= k[i]; }

void aes128_encrypt(const struct aes128 *a, const uint8_t in[16], uint8_t out[16])
{
    uint8_t s[16], t[16];
    memcpy(s, in, 16);
    add_key(s, a->rk);
    for (int r = 1; r <= 10; r++) {
        for (int i = 0; i < 16; i++) t[i] = sbox[s[(i + 4 * (i % 4)) % 16]];      /* sub bytes, shift rows */
        if (r < 10)
            for (int c = 0; c < 4; c++) {                                      /* mix columns */
                uint8_t *x = t + 4 * c, a0 = x[0], a1 = x[1], a2 = x[2], a3 = x[3], all = a0 ^ a1 ^ a2 ^ a3;
                x[0] ^= all ^ xt(a0 ^ a1); x[1] ^= all ^ xt(a1 ^ a2); x[2] ^= all ^ xt(a2 ^ a3); x[3] ^= all ^ xt(a3 ^ a0);
            }
        memcpy(s, t, 16);
        add_key(s, a->rk + 16 * r);
    }
    memcpy(out, s, 16);
}

void aes128_decrypt(const struct aes128 *a, const uint8_t in[16], uint8_t out[16])
{
    uint8_t s[16], t[16];
    memcpy(s, in, 16);
    for (int r = 10; r >= 1; r--) {
        add_key(s, a->rk + 16 * r);
        if (r < 10)
            for (int c = 0; c < 4; c++) {                                      /* inverse mix columns */
                uint8_t *x = s + 4 * c, a0 = x[0], a1 = x[1], a2 = x[2], a3 = x[3];
                x[0] = gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9);
                x[1] = gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13);
                x[2] = gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11);
                x[3] = gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14);
            }
        for (int i = 0; i < 16; i++) t[(i + 4 * (i % 4)) % 16] = isbox[s[i]];    /* inverse shift rows, sub bytes */
        memcpy(s, t, 16);
    }
    add_key(s, a->rk);
    memcpy(out, s, 16);
}

int aes_unwrap(const uint8_t kek[16], int n, const uint8_t *cipher, uint8_t *plain)
{
    struct aes128 a;
    uint8_t A[8], b[16];
    aes128_key(&a, kek);
    memcpy(A, cipher, 8);
    memcpy(plain, cipher + 8, (size_t)n * 8);
    for (int j = 5; j >= 0; j--)
        for (int i = n; i >= 1; i--) {
            uint64_t t = (uint64_t)n * (uint64_t)j + (uint64_t)i;
            memcpy(b, A, 8);
            for (int k = 0; k < 8; k++) b[7 - k] ^= (uint8_t)(t >> 8 * k);
            memcpy(b + 8, plain + 8 * (i - 1), 8);
            aes128_decrypt(&a, b, b);
            memcpy(A, b, 8);
            memcpy(plain + 8 * (i - 1), b + 8, 8);
        }
    for (int k = 0; k < 8; k++) if (A[k] != 0xa6) return -1;
    return 0;
}
