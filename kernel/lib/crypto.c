/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* SHA-1 (FIPS 180-4), HMAC (RFC 2104), PBKDF2 (RFC 8018), the 802.11 PRF,
 * AES-128 (FIPS 197) and AES key unwrap (RFC 3394). Small and plain: a
 * handshake runs them a few times, not per packet (the radio encrypts). */
#include "crypto.h"
#include "string.h"
#include "random.h"

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

/* ---- SHA-256 (FIPS 180-4) ------------------------------------------------------------------ */

static uint32_t ror(uint32_t v, int n) { return v >> n | v << (32 - n); }

static void sha256_block(struct sha256 *s, const uint8_t *p)
{
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    };
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ w[i - 15] >> 3;
        uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ w[i - 2] >> 10;
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25), ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + k[i] + w[i];
        uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22), maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

void sha256_init(struct sha256 *s)
{
    static const uint32_t iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(s->h, iv, sizeof iv);
    s->len = 0;
}

void sha256_update(struct sha256 *s, const void *data, size_t len)
{
    const uint8_t *p = data;
    size_t have = s->len % 64;
    s->len += len;
    if (have) {
        size_t n = 64 - have < len ? 64 - have : len;
        memcpy(s->buf + have, p, n);
        p += n; len -= n;
        if (have + n < 64) return;
        sha256_block(s, s->buf);
    }
    for (; len >= 64; p += 64, len -= 64) sha256_block(s, p);
    memcpy(s->buf, p, len);
}

void sha256_final(struct sha256 *s, uint8_t out[32])
{
    uint64_t bits = s->len * 8;
    uint8_t pad[72] = { 0x80 };
    size_t n = (s->len % 64 < 56 ? 56 : 120) - s->len % 64;
    for (int i = 0; i < 8; i++) pad[n + i] = (uint8_t)(bits >> (56 - 8 * i));
    sha256_update(s, pad, n + 8);
    for (int i = 0; i < 32; i++) out[i] = (uint8_t)(s->h[i / 4] >> (24 - 8 * (i % 4)));
}

void hmac_sha256_vec(const uint8_t *key, size_t klen, int n, const void *const *data, const size_t *len, uint8_t out[32])
{
    uint8_t k[64] = { 0 }, pad[64], inner[32];
    struct sha256 s;
    if (klen > 64) { sha256_init(&s); sha256_update(&s, key, klen); sha256_final(&s, k); }
    else memcpy(k, key, klen);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    sha256_init(&s);
    sha256_update(&s, pad, 64);
    for (int i = 0; i < n; i++) sha256_update(&s, data[i], len[i]);
    sha256_final(&s, inner);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    sha256_init(&s);
    sha256_update(&s, pad, 64);
    sha256_update(&s, inner, 32);
    sha256_final(&s, out);
}

void hmac_sha256(const uint8_t *key, size_t klen, const void *data, size_t len, uint8_t out[32])
{
    hmac_sha256_vec(key, klen, 1, &data, &len, out);
}

/* IEEE 802.11 KDF-Hash (12.7.1.6.2): HMAC-SHA256 counter mode, the counter
 * and the length (in bits) each a little-endian 16-bit value. */
void kdf_sha256(const uint8_t *key, size_t klen, const char *label, const uint8_t *ctx, size_t clen, uint8_t *out, size_t obits)
{
    size_t bytes = (obits + 7) / 8, done = 0;
    uint8_t bits[2] = { (uint8_t)obits, (uint8_t)(obits >> 8) };
    for (uint16_t i = 1; done < bytes; i++) {
        uint8_t ic[2] = { (uint8_t)i, (uint8_t)(i >> 8) }, dig[32];
        const void *d[4] = { ic, label, ctx, bits };
        size_t l[4] = { 2, strlen(label), clen, 2 };
        hmac_sha256_vec(key, klen, 4, d, l, dig);
        size_t n = bytes - done < 32 ? bytes - done : 32;
        memcpy(out + done, dig, n);
        done += n;
    }
}

/* ---- AES-128-CMAC (RFC 4493) --------------------------------------------------------------- */

static void cmac_dbl(uint8_t x[16])
{
    uint8_t carry = x[0] >> 7;
    for (int i = 0; i < 15; i++) x[i] = (uint8_t)(x[i] << 1 | x[i + 1] >> 7);
    x[15] = (uint8_t)(x[15] << 1);
    if (carry) x[15] ^= 0x87;
}

void aes_cmac(const uint8_t key[16], const void *data, size_t len, uint8_t mac[16])
{
    struct aes128 a;
    uint8_t k1[16] = { 0 }, x[16] = { 0 }, blk[16];
    const uint8_t *p = data;
    aes128_key(&a, key);
    aes128_encrypt(&a, k1, k1);                             /* L = E(0) */
    cmac_dbl(k1);                                           /* K1 */
    size_t full = len >= 16 ? (len - 1) / 16 : 0;           /* whole blocks before the last */
    for (size_t i = 0; i < full; i++) {
        for (int j = 0; j < 16; j++) x[j] ^= p[16 * i + j];
        aes128_encrypt(&a, x, x);
    }
    size_t rem = len - full * 16;
    memset(blk, 0, 16);
    memcpy(blk, p + full * 16, rem);
    if (rem == 16) {                                        /* a whole last block: xor K1 */
        for (int j = 0; j < 16; j++) blk[j] ^= k1[j];
    } else {                                                /* a short (or empty) last block: pad, xor K2 */
        blk[rem] = 0x80;
        cmac_dbl(k1);                                       /* K2 */
        for (int j = 0; j < 16; j++) blk[j] ^= k1[j];
    }
    for (int j = 0; j < 16; j++) x[j] ^= blk[j];
    aes128_encrypt(&a, x, mac);
}

/* ---- NIST P-256 (secp256r1) for SAE ------------------------------------------------------- */
/* Field elements and 256-bit integers as eight little-endian 32-bit limbs.
 * Field multiplication is Montgomery (the modulus is a fixed prime, so one
 * code path serves every curve operation); points are kept projective
 * (Jacobian) in Montgomery form and converted to affine only at the end.
 * Small, not constant-time: a handshake runs it a handful of times. */

typedef uint32_t fe[8];

static const fe P256 = { 0xffffffff, 0xffffffff, 0xffffffff, 0x00000000, 0x00000000, 0x00000000, 0x00000001, 0xffffffff };
static const fe N256 = { 0xfc632551, 0xf3b9cac2, 0xa7179e84, 0xbce6faad, 0xffffffff, 0xffffffff, 0x00000000, 0xffffffff };
static const fe B256 = { 0x27d2604b, 0x3bce3c3e, 0xcc53b0f6, 0x651d06b0, 0x769886bc, 0xb3ebbd55, 0xaa3a93e7, 0x5ac635d8 };
static const fe GX256 = { 0xd898c296, 0xf4a13945, 0x2deb33a0, 0x77037d81, 0x63a440f2, 0xf8bce6e5, 0xe12c4247, 0x6b17d1f2 };
static const fe GY256 = { 0x37bf51f5, 0xcbb64068, 0x6b315ece, 0x2bce3357, 0x7c0f9e16, 0x8ee7eb4a, 0xfe1a7f9b, 0x4fe342e2 };

static void fe_copy(fe r, const fe a) { memcpy(r, a, sizeof(fe)); }
static int fe_is_zero(const fe a) { uint32_t x = 0; for (int i = 0; i < 8; i++) x |= a[i]; return x == 0; }
static int fe_cmp(const fe a, const fe b) { for (int i = 7; i >= 0; i--) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1; return 0; }
static int fe_eq(const fe a, const fe b) { return fe_cmp(a, b) == 0; }

static uint32_t add_limbs(fe r, const fe a, const fe b)
{
    uint64_t c = 0;
    for (int i = 0; i < 8; i++) { uint64_t s = (uint64_t)a[i] + b[i] + c; r[i] = (uint32_t)s; c = s >> 32; }
    return (uint32_t)c;
}
static uint32_t sub_limbs(fe r, const fe a, const fe b)
{
    int64_t bo = 0;
    for (int i = 0; i < 8; i++) { int64_t d = (int64_t)a[i] - b[i] - bo; r[i] = (uint32_t)d; bo = d < 0; }
    return (uint32_t)bo;
}
static void fp_add(fe r, const fe a, const fe b)
{
    fe t;
    uint32_t c = add_limbs(r, a, b), bo = sub_limbs(t, r, P256);
    if (c || !bo) fe_copy(r, t);                            /* a + b >= 2^256, or r >= p: subtract p */
}
static void fp_sub(fe r, const fe a, const fe b)
{
    if (sub_limbs(r, a, b)) add_limbs(r, r, P256);
}

static void fe_from_be(fe r, const uint8_t b[32])
{
    for (int j = 0; j < 8; j++) {
        const uint8_t *p = b + 4 * (7 - j);
        r[j] = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    }
}
static void fe_to_be(uint8_t b[32], const fe r)
{
    for (int j = 0; j < 8; j++) {
        uint8_t *p = b + 4 * (7 - j);
        p[0] = (uint8_t)(r[j] >> 24); p[1] = (uint8_t)(r[j] >> 16); p[2] = (uint8_t)(r[j] >> 8); p[3] = (uint8_t)r[j];
    }
}

/* Montgomery multiply (CIOS), R = 2^256; n0' = 1 because p's low word is
 * 0xffffffff (that is -1 mod 2^32). */
static void mont_mul(fe r, const fe a, const fe b)
{
    uint32_t t[10] = { 0 };
    for (int i = 0; i < 8; i++) {
        uint64_t c = 0;
        for (int j = 0; j < 8; j++) { uint64_t s = (uint64_t)a[j] * b[i] + t[j] + c; t[j] = (uint32_t)s; c = s >> 32; }
        uint64_t s = (uint64_t)t[8] + c; t[8] = (uint32_t)s; t[9] = (uint32_t)(s >> 32);
        uint32_t m = t[0];                                  /* * n0' (= 1) */
        c = ((uint64_t)m * P256[0] + t[0]) >> 32;           /* low word becomes 0, keep the carry */
        for (int j = 1; j < 8; j++) { uint64_t s2 = (uint64_t)m * P256[j] + t[j] + c; t[j - 1] = (uint32_t)s2; c = s2 >> 32; }
        s = (uint64_t)t[8] + c; t[7] = (uint32_t)s; t[8] = t[9] + (uint32_t)(s >> 32);
    }
    for (int i = 0; i < 8; i++) r[i] = t[i];
    fe tmp;
    uint32_t bo = sub_limbs(tmp, r, P256);
    if (t[8] || !bo) fe_copy(r, tmp);                       /* >= p (or an extra top word): subtract once */
}

static int p256_ready;
static fe R2, ONE_M, B_M, PE_INV, PE_SQRT, PE_LEG;

static void to_mont(fe r, const fe a) { mont_mul(r, a, R2); }
static void from_mont(fe r, const fe a) { fe one = { 1 }; mont_mul(r, a, one); }
static void mont_pow(fe r, const fe base, const fe e)
{
    fe_copy(r, ONE_M);
    for (int i = 255; i >= 0; i--) {
        mont_mul(r, r, r);
        if ((e[i / 32] >> (i % 32)) & 1) mont_mul(r, r, base);
    }
}
static void mont_inv(fe r, const fe a) { mont_pow(r, a, PE_INV); }

static void rshift1(fe a) { for (int i = 0; i < 7; i++) a[i] = a[i] >> 1 | a[i + 1] << 31; a[7] >>= 1; }

static void p256_init(void)
{
    if (p256_ready) return;
    fe one = { 1 };
    fe_copy(R2, one);
    for (int i = 0; i < 512; i++) fp_add(R2, R2, R2);        /* 2^512 mod p */
    to_mont(ONE_M, one);
    to_mont(B_M, B256);
    fe_copy(PE_INV, P256); PE_INV[0] -= 2;                   /* p - 2 */
    fe_copy(PE_LEG, P256); rshift1(PE_LEG);                  /* (p - 1) / 2 */
    fe_copy(PE_SQRT, P256); { uint64_t c = 1; for (int i = 0; i < 8 && c; i++) { uint64_t s = (uint64_t)PE_SQRT[i] + c; PE_SQRT[i] = (uint32_t)s; c = s >> 32; } }
    rshift1(PE_SQRT); rshift1(PE_SQRT);                      /* (p + 1) / 4 */
    p256_ready = 1;
}

/* Jacobian point (X, Y, Z), all in Montgomery form; Z == 0 is infinity. */
struct jac { fe X, Y, Z; };

static void jac_double(struct jac *r, const struct jac *p)
{
    if (fe_is_zero(p->Z)) { *r = *p; return; }
    fe delta, gamma, beta, alpha, t0, t1, x3, y3, z3, tmp;
    mont_mul(delta, p->Z, p->Z);
    mont_mul(gamma, p->Y, p->Y);
    mont_mul(beta, p->X, gamma);
    fp_sub(t0, p->X, delta); fp_add(t1, p->X, delta);
    mont_mul(alpha, t0, t1);
    fp_add(t0, alpha, alpha); fp_add(alpha, t0, alpha);     /* 3*(X-delta)(X+delta) */
    mont_mul(x3, alpha, alpha);
    fp_add(tmp, beta, beta); fp_add(tmp, tmp, tmp); fp_add(t0, tmp, tmp);   /* 8*beta */
    fp_sub(x3, x3, t0);
    fp_add(t0, p->Y, p->Z); mont_mul(z3, t0, t0); fp_sub(z3, z3, gamma); fp_sub(z3, z3, delta);
    fp_add(t0, beta, beta); fp_add(t0, t0, t0); fp_sub(t0, t0, x3);        /* 4*beta - X3 */
    mont_mul(y3, alpha, t0);
    mont_mul(tmp, gamma, gamma); fp_add(tmp, tmp, tmp); fp_add(tmp, tmp, tmp); fp_add(tmp, tmp, tmp);  /* 8*gamma^2 */
    fp_sub(y3, y3, tmp);
    fe_copy(r->X, x3); fe_copy(r->Y, y3); fe_copy(r->Z, z3);
}

static void jac_add(struct jac *r, const struct jac *p, const struct jac *q)
{
    if (fe_is_zero(p->Z)) { *r = *q; return; }
    if (fe_is_zero(q->Z)) { *r = *p; return; }
    fe z1z1, z2z2, u1, u2, s1, s2, h, rr, i, j, v, t, t2, x3, y3;
    mont_mul(z1z1, p->Z, p->Z);
    mont_mul(z2z2, q->Z, q->Z);
    mont_mul(u1, p->X, z2z2);
    mont_mul(u2, q->X, z1z1);
    mont_mul(t, q->Z, z2z2); mont_mul(s1, p->Y, t);
    mont_mul(t, p->Z, z1z1); mont_mul(s2, q->Y, t);
    fp_sub(h, u2, u1);
    fp_sub(rr, s2, s1);
    if (fe_is_zero(h)) {
        if (fe_is_zero(rr)) jac_double(r, p);
        else { fe_copy(r->X, ONE_M); fe_copy(r->Y, ONE_M); memset(r->Z, 0, sizeof(fe)); }
        return;
    }
    fp_add(t, h, h); mont_mul(i, t, t);                     /* I = (2H)^2 */
    mont_mul(j, h, i);                                      /* J = H*I */
    fp_add(rr, rr, rr);                                     /* r = 2*(S2-S1) */
    mont_mul(v, u1, i);
    mont_mul(t, rr, rr); fp_sub(t, t, j); fp_add(t2, v, v); fp_sub(x3, t, t2);   /* X3 = r^2 - J - 2V */
    fp_sub(t, v, x3); mont_mul(t, rr, t);
    mont_mul(t2, s1, j); fp_add(t2, t2, t2); fp_sub(y3, t, t2);                   /* Y3 = r*(V-X3) - 2*S1*J */
    fp_add(t, p->Z, q->Z); mont_mul(t, t, t); fp_sub(t, t, z1z1); fp_sub(t, t, z2z2); mont_mul(t, t, h);  /* Z3 */
    fe_copy(r->X, x3); fe_copy(r->Y, y3); fe_copy(r->Z, t);
}

static void jac_mul(struct jac *r, const uint8_t k[32], const struct jac *p)
{
    struct jac acc;
    fe_copy(acc.X, ONE_M); fe_copy(acc.Y, ONE_M); memset(acc.Z, 0, sizeof(fe));  /* infinity */
    for (int by = 0; by < 32; by++)
        for (int bt = 7; bt >= 0; bt--) {
            jac_double(&acc, &acc);
            if ((k[by] >> bt) & 1) jac_add(&acc, &acc, p);
        }
    *r = acc;
}

static void jac_affine(uint8_t out[64], const struct jac *p)
{
    if (fe_is_zero(p->Z)) { memset(out, 0, 64); return; }
    fe zinv, zinv2, zinv3, x, y, xn, yn;
    mont_inv(zinv, p->Z);
    mont_mul(zinv2, zinv, zinv);
    mont_mul(zinv3, zinv2, zinv);
    mont_mul(x, p->X, zinv2);
    mont_mul(y, p->Y, zinv3);
    from_mont(xn, x); from_mont(yn, y);
    fe_to_be(out, xn); fe_to_be(out + 32, yn);
}

static void load_point(struct jac *j, const uint8_t p[64])
{
    fe x, y;
    fe_from_be(x, p); fe_from_be(y, p + 32);
    to_mont(j->X, x); to_mont(j->Y, y); fe_copy(j->Z, ONE_M);
}

void p256_mul(const uint8_t k[32], const uint8_t p[64], uint8_t out[64])
{
    p256_init();
    struct jac P, R;
    load_point(&P, p);
    jac_mul(&R, k, &P);
    jac_affine(out, &R);
}

void p256_add(const uint8_t a[64], const uint8_t b[64], uint8_t out[64])
{
    p256_init();
    struct jac A, B, R;
    load_point(&A, a); load_point(&B, b);
    jac_add(&R, &A, &B);
    jac_affine(out, &R);
}

void p256_negate(const uint8_t in[64], uint8_t out[64])
{
    fe y, z = { 0 };
    memcpy(out, in, 32);
    fe_from_be(y, in + 32);
    fp_sub(y, z, y);                                        /* p - y */
    fe_to_be(out + 32, y);
}

int p256_point_valid(const uint8_t p[64])
{
    p256_init();
    fe x, y;
    fe_from_be(x, p); fe_from_be(y, p + 32);
    if (fe_cmp(x, P256) >= 0 || fe_cmp(y, P256) >= 0) return 0;
    if (fe_is_zero(x) && fe_is_zero(y)) return 0;
    fe xm, ym, x2, x3, t, rhs, y2;
    to_mont(xm, x); to_mont(ym, y);
    mont_mul(x2, xm, xm); mont_mul(x3, x2, xm);
    fp_add(t, xm, xm); fp_add(t, t, xm);                    /* 3x */
    fp_sub(rhs, x3, t); fp_add(rhs, rhs, B_M);              /* x^3 - 3x + b */
    mont_mul(y2, ym, ym);
    return fe_eq(y2, rhs);
}

void p256_scalar_add(const uint8_t a[32], const uint8_t b[32], uint8_t out[32])
{
    fe A, B, r;
    fe_from_be(A, a); fe_from_be(B, b);
    uint32_t c = add_limbs(r, A, B);
    if (c) sub_limbs(r, r, N256);
    if (fe_cmp(r, N256) >= 0) sub_limbs(r, r, N256);
    fe_to_be(out, r);
}

int p256_scalar_rand(uint8_t out[32])
{
    for (int tries = 0; tries < 64; tries++) {
        uint8_t buf[32];
        fe r;
        random_bytes(buf, 32);
        fe_from_be(r, buf);
        if (fe_cmp(r, N256) >= 0 || fe_is_zero(r)) continue;
        memcpy(out, buf, 32);
        return 0;
    }
    return -1;
}

/* y^2 = x^3 - 3x + b in Montgomery form, for a candidate x (normal). */
static void curve_rhs(fe rhs, const fe x_normal)
{
    fe xm, x2, x3, t;
    to_mont(xm, x_normal);
    mont_mul(x2, xm, xm); mont_mul(x3, x2, xm);
    fp_add(t, xm, xm); fp_add(t, t, xm);
    fp_sub(rhs, x3, t); fp_add(rhs, rhs, B_M);
}

/* SAE's password element by hunting and pecking (802.11 12.4.4.3.2),
 * group 19: the first counter whose derived x lies on the curve. */
int p256_sae_pwe(const char *pass, const uint8_t a[6], const uint8_t b[6], uint8_t pwe[64])
{
    p256_init();
    uint8_t key[12], prime_be[32], save_x[32] = { 0 };
    int cmp = memcmp(a, b, 6), found = 0, save_lsb = 0;
    memcpy(key, cmp >= 0 ? a : b, 6);
    memcpy(key + 6, cmp >= 0 ? b : a, 6);
    fe_to_be(prime_be, P256);
    for (int counter = 1; counter <= 100 && !found; counter++) {
        uint8_t seed[32], val[32], ctr = (uint8_t)counter;
        const void *d[2] = { pass, &ctr };
        size_t l[2] = { strlen(pass), 1 };
        hmac_sha256_vec(key, 12, 2, d, l, seed);
        kdf_sha256(seed, 32, "SAE Hunting and Pecking", prime_be, 32, val, 256);
        fe x;
        fe_from_be(x, val);
        if (fe_cmp(x, P256) >= 0) continue;
        fe rhs, leg;
        curve_rhs(rhs, x);
        mont_pow(leg, rhs, PE_LEG);                         /* Legendre: a quadratic residue? */
        if (fe_eq(leg, ONE_M)) { memcpy(save_x, val, 32); save_lsb = seed[31] & 1; found = 1; }
    }
    if (!found) return -1;
    fe x, rhs, ym, yn, z = { 0 };
    fe_from_be(x, save_x);
    curve_rhs(rhs, x);
    mont_pow(ym, rhs, PE_SQRT);                             /* y = sqrt(rhs), p = 3 mod 4 */
    from_mont(yn, ym);
    if ((int)(yn[0] & 1) != save_lsb) fp_sub(yn, z, yn);    /* match the seed's parity */
    memcpy(pwe, save_x, 32);
    fe_to_be(pwe + 32, yn);
    return 0;
}

int p256_selftest(void)
{
    p256_init();
    uint8_t g[64], out[64], neg[64], g2a[64], g2b[64], two[32] = { 0 }, k1[32] = { 0 }, k2[32] = { 0 }, t1[64], t2[64];
    fe_to_be(g, GX256); fe_to_be(g + 32, GY256);
    if (!p256_point_valid(g)) return -1;
    /* (n-1)*G = -G */
    uint8_t nm1[32];
    fe nf; fe_copy(nf, N256); nf[0] -= 1; fe_to_be(nm1, nf);
    p256_mul(nm1, g, out);
    p256_negate(g, neg);
    if (memcmp(out, neg, 64)) return -2;
    /* 2*G by addition and by scalar both agree and lie on the curve */
    p256_add(g, g, g2a);
    two[31] = 2; p256_mul(two, g, g2b);
    if (memcmp(g2a, g2b, 64) || !p256_point_valid(g2a)) return -3;
    /* a*(b*G) == b*(a*G) */
    k1[31] = 7; k2[31] = 11;
    p256_mul(k1, g, t1); p256_mul(k2, t1, t1);
    p256_mul(k2, g, t2); p256_mul(k1, t2, t2);
    if (memcmp(t1, t2, 64)) return -4;
    return 0;
}
