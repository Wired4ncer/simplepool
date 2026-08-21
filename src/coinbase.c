#define _POSIX_C_SOURCE 200809L
#include "coinbase.h"
#include "sha256.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_err(char *errbuf, size_t errlen, const char *fmt, ...) {
    if (!errbuf || errlen == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(errbuf, errlen, fmt, ap);
    va_end(ap);
}

/* ---------- byte-buffer helpers ---------- */

typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
    int      oom;
} bbuf_t;

static void bbuf_init(bbuf_t *b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    b->oom = 0;
}
static void bbuf_free(bbuf_t *b) {
    free(b->data);
    bbuf_init(b);
}
static int bbuf_reserve(bbuf_t *b, size_t need) {
    if (b->cap >= need) return 0;
    size_t nc = b->cap ? b->cap : 64;
    while (nc < need) nc *= 2;
    uint8_t *p = (uint8_t *)realloc(b->data, nc);
    if (!p) { b->oom = 1; return -1; }
    b->data = p;
    b->cap = nc;
    return 0;
}
static int bbuf_push(bbuf_t *b, const void *src, size_t n) {
    if (bbuf_reserve(b, b->len + n) < 0) return -1;
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return 0;
}
static int bbuf_push_u8(bbuf_t *b, uint8_t v) { return bbuf_push(b, &v, 1); }
static int bbuf_push_u32_le(bbuf_t *b, uint32_t v) {
    uint8_t buf[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    return bbuf_push(b, buf, 4);
}
static int bbuf_push_i32_le(bbuf_t *b, int32_t v) {
    return bbuf_push_u32_le(b, (uint32_t)v);
}
static int bbuf_push_u64_le(bbuf_t *b, uint64_t v) {
    uint8_t buf[8];
    for (int i = 0; i < 8; i++) buf[i] = (uint8_t)(v >> (8 * i));
    return bbuf_push(b, buf, 8);
}
static int bbuf_push_varint(bbuf_t *b, uint64_t n) {
    if (n < 0xfd) return bbuf_push_u8(b, (uint8_t)n);
    if (n <= 0xffff) {
        if (bbuf_push_u8(b, 0xfd) < 0) return -1;
        uint8_t buf[2] = { (uint8_t)n, (uint8_t)(n >> 8) };
        return bbuf_push(b, buf, 2);
    }
    if (n <= 0xffffffffULL) {
        if (bbuf_push_u8(b, 0xfe) < 0) return -1;
        return bbuf_push_u32_le(b, (uint32_t)n);
    }
    if (bbuf_push_u8(b, 0xff) < 0) return -1;
    return bbuf_push_u64_le(b, n);
}

/* ---------- BIP34 height push ----------
 *
 * Bitcoin Core validates the coinbase scriptSig by comparing it against
 * `CScript() << nHeight`. That operator overload calls push_int64(), which
 * has three branches:
 *
 *   n == 0            -> OP_0  (single byte 0x00)
 *   1 <= n <= 16      -> OP_N  (single byte 0x50 + n)            <-- short form
 *   otherwise         -> length-prefixed CScriptNum::serialize(n)
 *
 * On a fresh chain (regtest / signet / drivechain testnet) the first 16
 * blocks therefore expect the OP_N short form. Encoding a height of 5 as
 * {0x01,0x05} instead of {0x55} causes ContextualCheckBlock to reject the
 * block with "bad-cb-height". Mainnet is unaffected because every accepted
 * block has height >= 17.
 *
 * Returns number of bytes written (always <= 6). */
static size_t bip34_height_push(uint32_t height, uint8_t out[8]) {
    if (height == 0) {
        out[0] = 0x00;        /* OP_0 */
        return 1;
    }
    if (height <= 16) {
        out[0] = (uint8_t)(0x50 + height);   /* OP_1 .. OP_16 */
        return 1;
    }
    /* CScriptNum minimal little-endian encoding with length prefix. The
     * extra zero byte handles the sign bit so high values aren't read as
     * negative. */
    uint8_t bytes[5];
    size_t n = 0;
    uint32_t v = height;
    while (v > 0) {
        bytes[n++] = (uint8_t)(v & 0xff);
        v >>= 8;
    }
    if (bytes[n - 1] & 0x80) {
        bytes[n++] = 0x00;
    }
    out[0] = (uint8_t)n;
    memcpy(out + 1, bytes, n);
    return n + 1;
}

/* ---------- hex ---------- */

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static int hex_decode(const char *hex, uint8_t *out, size_t cap, size_t *out_len) {
    size_t n = strlen(hex);
    if (n % 2 != 0) return -1;
    if (n / 2 > cap) return -1;
    for (size_t i = 0; i < n / 2; i++) {
        int hi = hex_nibble(hex[2 * i]);
        int lo = hex_nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    *out_len = n / 2;
    return 0;
}

/* ---------- base58check ---------- */

static const char b58_alpha[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

static int b58_decode(const char *s, uint8_t *out, size_t cap, size_t *out_len) {
    size_t slen = strlen(s);
    /* Count leading '1's. */
    size_t zeros = 0;
    while (zeros < slen && s[zeros] == '1') zeros++;

    /* Process. */
    size_t bsize = slen * 733 / 1000 + 1;
    uint8_t *b = (uint8_t *)calloc(bsize, 1);
    if (!b) return -1;

    for (size_t i = 0; i < slen; i++) {
        const char *p = strchr(b58_alpha, s[i]);
        if (!p) { free(b); return -1; }
        unsigned carry = (unsigned)(p - b58_alpha);
        for (ssize_t j = (ssize_t)bsize - 1; j >= 0; j--) {
            carry += 58u * b[j];
            b[j] = (uint8_t)(carry & 0xff);
            carry >>= 8;
        }
        if (carry != 0) { free(b); return -1; }
    }

    /* Skip leading zeros in big-int representation. */
    size_t skip = 0;
    while (skip < bsize && b[skip] == 0) skip++;

    size_t total = zeros + (bsize - skip);
    if (total > cap) { free(b); return -1; }

    memset(out, 0, zeros);
    memcpy(out + zeros, b + skip, bsize - skip);
    *out_len = total;
    free(b);
    return 0;
}

/* ---------- bech32 / segwit (BIP173) ---------- */

static const char bech32_alpha[] = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";

static int bech32_charpos(char c) {
    const char *p = strchr(bech32_alpha, c);
    if (!p) return -1;
    return (int)(p - bech32_alpha);
}

static uint32_t bech32_polymod(const uint8_t *values, size_t n) {
    static const uint32_t G[5] = {
        0x3b6a57b2, 0x26508e6d, 0x1ea119fa, 0x3d4233dd, 0x2a1462b3
    };
    uint32_t chk = 1;
    for (size_t i = 0; i < n; i++) {
        uint8_t b = (uint8_t)(chk >> 25);
        chk = ((chk & 0x1ffffff) << 5) ^ values[i];
        for (int j = 0; j < 5; j++) {
            if ((b >> j) & 1) chk ^= G[j];
        }
    }
    return chk;
}

static void bech32_hrp_expand(const char *hrp, uint8_t *out, size_t *outlen) {
    size_t n = strlen(hrp);
    for (size_t i = 0; i < n; i++) out[i] = (uint8_t)(hrp[i] >> 5);
    out[n] = 0;
    for (size_t i = 0; i < n; i++) out[n + 1 + i] = (uint8_t)(hrp[i] & 31);
    *outlen = 2 * n + 1;
}

/* Decode bech32 / bech32m. Returns 0 ok, negative on error.
 * out_data receives the 5-bit groups (after HRP and separator, excluding checksum).
 * encoding_out: 1 = bech32, 2 = bech32m.
 */
static int bech32_decode(const char *addr, char *hrp_out, size_t hrp_cap,
                         uint8_t *data_out, size_t *data_len, int *encoding_out) {
    size_t alen = strlen(addr);
    if (alen < 8 || alen > 90) return -1;

    /* Find separator '1' (last one). */
    ssize_t sep = -1;
    for (ssize_t i = (ssize_t)alen - 1; i >= 0; i--) {
        if (addr[i] == '1') { sep = i; break; }
    }
    if (sep < 1 || (size_t)(sep + 7) > alen) return -1;

    /* HRP. */
    size_t hrp_len = (size_t)sep;
    if (hrp_len + 1 > hrp_cap) return -1;
    int has_lower = 0, has_upper = 0;
    for (size_t i = 0; i < hrp_len; i++) {
        char c = addr[i];
        if (c < 33 || c > 126) return -1;
        if (c >= 'a' && c <= 'z') has_lower = 1;
        if (c >= 'A' && c <= 'Z') { has_upper = 1; hrp_out[i] = (char)(c - 'A' + 'a'); }
        else hrp_out[i] = c;
    }
    hrp_out[hrp_len] = '\0';

    /* Data part. */
    size_t data_n = alen - hrp_len - 1;
    if (data_n > 90) return -1;
    uint8_t values[90];
    for (size_t i = 0; i < data_n; i++) {
        char c = addr[hrp_len + 1 + i];
        if (c >= 'A' && c <= 'Z') { has_upper = 1; c = (char)(c - 'A' + 'a'); }
        else if (c >= 'a' && c <= 'z') has_lower = 1;
        int v = bech32_charpos(c);
        if (v < 0) return -1;
        values[i] = (uint8_t)v;
    }
    if (has_lower && has_upper) return -1;

    /* Compute polymod over hrp_expand || values. */
    uint8_t buf[200];
    size_t buflen = 0;
    bech32_hrp_expand(hrp_out, buf, &buflen);
    if (buflen + data_n > sizeof buf) return -1;
    memcpy(buf + buflen, values, data_n);
    uint32_t chk = bech32_polymod(buf, buflen + data_n);
    if (chk == 1) *encoding_out = 1;
    else if (chk == 0x2bc830a3) *encoding_out = 2;
    else return -1;

    if (*data_len < data_n - 6) return -1;
    memcpy(data_out, values, data_n - 6);
    *data_len = data_n - 6;
    return 0;
}

/* Convert from bits-per-element `from` to bits-per-element `to`. */
static int convertbits(const uint8_t *in, size_t in_len, int from, int to, int pad,
                       uint8_t *out, size_t *out_len) {
    uint32_t acc = 0;
    int bits = 0;
    size_t off = 0;
    uint32_t maxv = (1u << to) - 1;
    for (size_t i = 0; i < in_len; i++) {
        uint8_t v = in[i];
        if (v >> from) return -1;
        acc = (acc << from) | v;
        bits += from;
        while (bits >= to) {
            bits -= to;
            if (off >= *out_len) return -1;
            out[off++] = (uint8_t)((acc >> bits) & maxv);
        }
    }
    if (pad) {
        if (bits > 0) {
            if (off >= *out_len) return -1;
            out[off++] = (uint8_t)((acc << (to - bits)) & maxv);
        }
    } else if (bits >= from || ((acc << (to - bits)) & maxv) != 0) {
        return -1;
    }
    *out_len = off;
    return 0;
}

/* ---------- address -> script ---------- */

int coinbase_address_to_script(const char *addr,
                               uint8_t *out, size_t cap, size_t *out_len,
                               char *errbuf, size_t errlen) {
    /* Try bech32 first if it has '1' separator and a known HRP prefix. */
    if (strncmp(addr, "bc1",   3) == 0 ||
        strncmp(addr, "tb1",   3) == 0 ||
        strncmp(addr, "bcrt1", 5) == 0 ||
        strncmp(addr, "BC1",   3) == 0 ||
        strncmp(addr, "TB1",   3) == 0 ||
        strncmp(addr, "BCRT1", 5) == 0) {
        char hrp[16];
        uint8_t data[90];
        size_t data_len = sizeof data;
        int enc = 0;
        if (bech32_decode(addr, hrp, sizeof hrp, data, &data_len, &enc) < 0) {
            set_err(errbuf, errlen, "bech32 decode failed for '%s'", addr);
            return -1;
        }
        if (data_len < 1) {
            set_err(errbuf, errlen, "bech32 data too short");
            return -1;
        }
        uint8_t witver = data[0];
        if (witver > 16) {
            set_err(errbuf, errlen, "bad witness version %u", witver);
            return -1;
        }
        /* v0 must use bech32; v1+ must use bech32m. */
        if ((witver == 0 && enc != 1) || (witver != 0 && enc != 2)) {
            set_err(errbuf, errlen, "bech32 encoding/version mismatch");
            return -1;
        }
        uint8_t prog[64];
        size_t prog_len = sizeof prog;
        if (convertbits(data + 1, data_len - 1, 5, 8, 0, prog, &prog_len) < 0) {
            set_err(errbuf, errlen, "bech32 5->8 convert failed");
            return -1;
        }
        if (witver == 0 && prog_len != 20) {
            set_err(errbuf, errlen, "only P2WPKH (20-byte) v0 supported, got %zu", prog_len);
            return -1;
        }
        if (witver == 0 && prog_len == 20) {
            /* OP_0 <20-byte push>. */
            if (cap < 22) return -1;
            out[0] = 0x00;
            out[1] = 0x14;
            memcpy(out + 2, prog, 20);
            *out_len = 22;
            return 0;
        }
        set_err(errbuf, errlen, "unsupported segwit version/program length");
        return -1;
    }

    /* Base58check: P2PKH or P2SH. */
    uint8_t dec[64];
    size_t dec_len = 0;
    if (b58_decode(addr, dec, sizeof dec, &dec_len) < 0 || dec_len < 5) {
        set_err(errbuf, errlen, "base58 decode failed for '%s'", addr);
        return -1;
    }
    /* Verify checksum. */
    uint8_t hash1[32], hash2[32];
    sha256(dec, dec_len - 4, hash1);
    sha256(hash1, 32, hash2);
    if (memcmp(hash2, dec + dec_len - 4, 4) != 0) {
        set_err(errbuf, errlen, "base58 checksum mismatch for '%s'", addr);
        return -1;
    }
    if (dec_len != 25) {
        set_err(errbuf, errlen, "unexpected base58 length %zu", dec_len);
        return -1;
    }
    uint8_t ver = dec[0];
    /* P2PKH: 0x00 (mainnet), 0x6f (testnet/regtest). */
    if (ver == 0x00 || ver == 0x6f) {
        if (cap < 25) return -1;
        out[0] = 0x76; /* OP_DUP */
        out[1] = 0xa9; /* OP_HASH160 */
        out[2] = 0x14; /* push 20 */
        memcpy(out + 3, dec + 1, 20);
        out[23] = 0x88; /* OP_EQUALVERIFY */
        out[24] = 0xac; /* OP_CHECKSIG */
        *out_len = 25;
        return 0;
    }
    /* P2SH: 0x05 (mainnet), 0xc4 (testnet/regtest). */
    if (ver == 0x05 || ver == 0xc4) {
        if (cap < 23) return -1;
        out[0] = 0xa9; /* OP_HASH160 */
        out[1] = 0x14; /* push 20 */
        memcpy(out + 2, dec + 1, 20);
        out[22] = 0x87; /* OP_EQUAL */
        *out_len = 23;
        return 0;
    }
    set_err(errbuf, errlen, "unsupported base58 version byte 0x%02x", ver);
    return -1;
}

/* ---------- main builder ---------- */

void coinbase_parts_free(coinbase_parts_t *p) {
    if (!p) return;
    free(p->cb1); p->cb1 = NULL; p->cb1_len = 0;
    free(p->cb2); p->cb2 = NULL; p->cb2_len = 0;
}

int coinbase_build(uint32_t height, int64_t value_sats,
                   const char *payout_address,
                   const char *witness_commitment_hex,
                   const char *coinbase_tag,
                   size_t extranonce1_size, size_t extranonce2_size,
                   coinbase_parts_t *out, char *errbuf, size_t errlen) {
    return coinbase_build_split(height, value_sats,
                                payout_address, NULL, 0,
                                witness_commitment_hex, coinbase_tag,
                                extranonce1_size, extranonce2_size,
                                out, NULL, NULL, errbuf, errlen);
}

int coinbase_build_split(uint32_t height, int64_t value_sats,
                         const char *miner_address,
                         const char *operator_address,
                         int fee_bps,
                         const char *witness_commitment_hex,
                         const char *coinbase_tag,
                         size_t extranonce1_size, size_t extranonce2_size,
                         coinbase_parts_t *out,
                         int64_t *out_miner_sats, int64_t *out_fee_sats,
                         char *errbuf, size_t errlen) {
    if (!out || !miner_address) {
        set_err(errbuf, errlen, "null arg");
        return -1;
    }
    out->cb1 = NULL; out->cb1_len = 0;
    out->cb2 = NULL; out->cb2_len = 0;
    if (out_miner_sats) *out_miner_sats = value_sats;
    if (out_fee_sats)   *out_fee_sats   = 0;

    /* Resolve scriptPubKey for miner. */
    uint8_t miner_spk[64];
    size_t  miner_spk_len = 0;
    if (coinbase_address_to_script(miner_address, miner_spk, sizeof miner_spk,
                                   &miner_spk_len, errbuf, errlen) < 0) {
        return -1;
    }

    /* Compute fee split. The fee output is omitted entirely if it would be
     * zero, below dust, or the operator address is missing. */
    int64_t fee_sats   = 0;
    int64_t miner_sats = value_sats;
    uint8_t operator_spk[64];
    size_t  operator_spk_len = 0;
    int     has_operator = 0;
    if (operator_address && operator_address[0] &&
        fee_bps > 0 && value_sats > 0) {
        fee_sats = (value_sats * (int64_t)fee_bps) / 10000;
        if (fee_sats >= COINBASE_DUST_SATS) {
            if (coinbase_address_to_script(operator_address, operator_spk,
                                           sizeof operator_spk,
                                           &operator_spk_len,
                                           errbuf, errlen) < 0) {
                return -1;
            }
            miner_sats = value_sats - fee_sats;
            has_operator = 1;
        } else {
            fee_sats = 0;
        }
    }
    if (out_miner_sats) *out_miner_sats = miner_sats;
    if (out_fee_sats)   *out_fee_sats   = fee_sats;

    /* Decode witness commitment if any. */
    uint8_t wc_buf[256];
    size_t wc_len = 0;
    int has_wc = 0;
    if (witness_commitment_hex && *witness_commitment_hex) {
        if (hex_decode(witness_commitment_hex, wc_buf, sizeof wc_buf, &wc_len) < 0) {
            set_err(errbuf, errlen, "bad witness commitment hex");
            return -1;
        }
        has_wc = 1;
    }

    /* Height push. */
    uint8_t height_push[8];
    size_t height_push_len = bip34_height_push(height, height_push);

    /* Tag push. */
    uint8_t tag_push[80];
    size_t tag_push_len = 0;
    if (coinbase_tag && *coinbase_tag) {
        size_t tlen = strlen(coinbase_tag);
        if (tlen > 75) tlen = 75;
        tag_push[0] = (uint8_t)tlen;
        memcpy(tag_push + 1, coinbase_tag, tlen);
        tag_push_len = tlen + 1;
    }

    size_t en_total = extranonce1_size + extranonce2_size;
    size_t script_sig_len = height_push_len + tag_push_len + en_total;

    /* Consensus caps a coinbase scriptSig at 2..100 bytes. The template-based
     * builders check this; this one did not, purely by omission. Today the max
     * is ~94 (6 height + 76 tag + 12 extranonce) so it cannot trip — but a
     * longer extranonce would silently start producing blocks the network
     * rejects, which is the worst possible way to find out. */
    if (script_sig_len < 2 || script_sig_len > 100) {
        set_err(errbuf, errlen,
                "coinbase scriptSig length %zu out of range (2..100)",
                script_sig_len);
        return -1;
    }

    /* Build outputs blob: miner payout, [operator fee], [witness commitment]. */
    bbuf_t outs;
    bbuf_init(&outs);
    uint64_t n_outputs = 1;

    if (bbuf_push_u64_le(&outs, (uint64_t)miner_sats) < 0) goto oom;
    if (bbuf_push_varint(&outs, miner_spk_len) < 0) goto oom;
    if (bbuf_push(&outs, miner_spk, miner_spk_len) < 0) goto oom;

    if (has_operator) {
        if (bbuf_push_u64_le(&outs, (uint64_t)fee_sats) < 0) goto oom;
        if (bbuf_push_varint(&outs, operator_spk_len) < 0) goto oom;
        if (bbuf_push(&outs, operator_spk, operator_spk_len) < 0) goto oom;
        n_outputs++;
    }

    if (has_wc) {
        if (bbuf_push_u64_le(&outs, 0) < 0) goto oom;
        if (bbuf_push_varint(&outs, wc_len) < 0) goto oom;
        if (bbuf_push(&outs, wc_buf, wc_len) < 0) goto oom;
        n_outputs++;
    }

    /* Build c1. */
    bbuf_t c1;
    bbuf_init(&c1);
    if (bbuf_push_i32_le(&c1, 1) < 0) goto oom2;
    if (bbuf_push_varint(&c1, 1) < 0) goto oom2;
    static const uint8_t zero32[32] = {0};
    if (bbuf_push(&c1, zero32, 32) < 0) goto oom2;
    if (bbuf_push_u32_le(&c1, 0xffffffff) < 0) goto oom2;
    if (bbuf_push_varint(&c1, script_sig_len) < 0) goto oom2;
    if (bbuf_push(&c1, height_push, height_push_len) < 0) goto oom2;
    if (tag_push_len && bbuf_push(&c1, tag_push, tag_push_len) < 0) goto oom2;

    /* Build c2. */
    bbuf_t c2;
    bbuf_init(&c2);
    if (bbuf_push_u32_le(&c2, 0xffffffff) < 0) goto oom3;
    if (bbuf_push_varint(&c2, n_outputs) < 0) goto oom3;
    if (bbuf_push(&c2, outs.data, outs.len) < 0) goto oom3;
    if (bbuf_push_u32_le(&c2, 0) < 0) goto oom3;

    bbuf_free(&outs);

    out->cb1 = c1.data;
    out->cb1_len = c1.len;
    out->cb2 = c2.data;
    out->cb2_len = c2.len;
    return 0;

oom3:
    bbuf_free(&c2);
oom2:
    bbuf_free(&c1);
oom:
    bbuf_free(&outs);
    set_err(errbuf, errlen, "out of memory");
    return -1;
}

/* ---------- coinbasetxn (server-provided coinbase) ---------- */

/* Read a little-endian Bitcoin varint from buf[*off..len). Advances *off.
 *
 * NOTE for every caller: the value returned is attacker-shaped, up to 2^64-1,
 * and comes from whatever the block-template backend sent. Bounds-check it as
 * `val > len - *off`, NEVER as `*off + val > len` — the latter wraps, passes,
 * and hands a multi-exabyte length to the memcpy that follows. */
static int rd_varint(const uint8_t *buf, size_t len, size_t *off, uint64_t *val) {
    if (*off >= len) return -1;
    uint8_t b = buf[(*off)++];
    if (b < 0xfd) { *val = b; return 0; }
    size_t n = (b == 0xfd) ? 2 : (b == 0xfe) ? 4 : 8;
    if (*off + n > len) return -1;
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) v |= (uint64_t)buf[*off + i] << (8 * i);
    *off += n;
    *val = v;
    return 0;
}

static int rd_u32(const uint8_t *buf, size_t len, size_t *off, uint32_t *val) {
    if (*off + 4 > len) return -1;
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= (uint32_t)buf[*off + i] << (8 * i);
    *off += 4;
    *val = v;
    return 0;
}

static int rd_u64(const uint8_t *buf, size_t len, size_t *off, uint64_t *val) {
    if (*off + 8 > len) return -1;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)buf[*off + i] << (8 * i);
    *off += 8;
    *val = v;
    return 0;
}

int coinbase_build_from_template(const char *coinbase_tx_hex,
                                 const char *miner_address,
                                 const char *operator_address,
                                 int fee_bps,
                                 const char *coinbase_tag,
                                 size_t extranonce1_size,
                                 size_t extranonce2_size,
                                 coinbase_parts_t *out,
                                 int *out_has_witness,
                                 int64_t *out_miner_sats,
                                 int64_t *out_fee_sats,
                                 char *errbuf, size_t errlen) {
    if (!out || !coinbase_tx_hex || !miner_address) {
        set_err(errbuf, errlen, "null arg");
        return -1;
    }
    out->cb1 = NULL; out->cb1_len = 0;
    out->cb2 = NULL; out->cb2_len = 0;
    if (out_has_witness) *out_has_witness = 0;
    if (out_miner_sats)  *out_miner_sats  = 0;
    if (out_fee_sats)    *out_fee_sats    = 0;

    struct cb_out { uint64_t value; size_t spk_off; size_t spk_len; int op_return; };

    uint8_t *tx = NULL;
    struct cb_out *outs = NULL;
    bbuf_t c1, ob, c2;
    bbuf_init(&c1); bbuf_init(&ob); bbuf_init(&c2);
    int ret = -1;

    /* Decode the coinbase tx hex into bytes. */
    size_t hexlen = strlen(coinbase_tx_hex);
    if (hexlen % 2 != 0) { set_err(errbuf, errlen, "odd coinbasetxn hex"); goto done; }
    size_t txlen = hexlen / 2;
    tx = (uint8_t *)malloc(txlen ? txlen : 1);
    if (!tx) { set_err(errbuf, errlen, "oom"); goto done; }
    size_t dl = 0;
    if (hex_decode(coinbase_tx_hex, tx, txlen, &dl) < 0 || dl != txlen) {
        set_err(errbuf, errlen, "bad coinbasetxn hex");
        goto done;
    }

    /* Parse: version | [marker flag] | vin | input | vout | outputs |
     *        [witness] | locktime. */
    size_t off = 0;
    uint32_t version;
    if (rd_u32(tx, txlen, &off, &version) < 0) { set_err(errbuf, errlen, "truncated coinbasetxn"); goto done; }

    int has_witness = 0;
    if (off + 2 <= txlen && tx[off] == 0x00 && tx[off + 1] != 0x00) {
        has_witness = 1;     /* segwit marker (0x00) + flag (nonzero) */
        off += 2;
    }

    uint64_t vin = 0;
    if (rd_varint(tx, txlen, &off, &vin) < 0) { set_err(errbuf, errlen, "truncated coinbasetxn"); goto done; }
    if (vin != 1) { set_err(errbuf, errlen, "coinbasetxn input count %llu != 1", (unsigned long long)vin); goto done; }

    size_t prevout_off = off;            /* 32-byte hash + 4-byte index */
    if (off + 36 > txlen) { set_err(errbuf, errlen, "truncated coinbasetxn input"); goto done; }
    off += 36;
    uint64_t ss_len = 0;
    if (rd_varint(tx, txlen, &off, &ss_len) < 0) { set_err(errbuf, errlen, "truncated scriptSig"); goto done; }
    size_t ss_off = off;
    if (ss_len > (uint64_t)(txlen - off)) { set_err(errbuf, errlen, "truncated scriptSig"); goto done; }
    off += ss_len;
    uint32_t sequence;
    if (rd_u32(tx, txlen, &off, &sequence) < 0) { set_err(errbuf, errlen, "truncated sequence"); goto done; }

    uint64_t vout = 0;
    if (rd_varint(tx, txlen, &off, &vout) < 0) { set_err(errbuf, errlen, "truncated output count"); goto done; }
    if (vout == 0) { set_err(errbuf, errlen, "coinbasetxn has no outputs"); goto done; }

    outs = (struct cb_out *)calloc((size_t)vout, sizeof(*outs));
    if (!outs) { set_err(errbuf, errlen, "oom"); goto done; }

    int64_t reward_idx = -1;
    int reward_count = 0;
    for (uint64_t i = 0; i < vout; i++) {
        uint64_t val = 0, spk_len = 0;
        if (rd_u64(tx, txlen, &off, &val) < 0) { set_err(errbuf, errlen, "truncated output value"); goto done; }
        if (rd_varint(tx, txlen, &off, &spk_len) < 0) { set_err(errbuf, errlen, "truncated scriptPubKey"); goto done; }
        if (spk_len > (uint64_t)(txlen - off)) { set_err(errbuf, errlen, "truncated scriptPubKey"); goto done; }
        outs[i].value = val;
        outs[i].spk_off = off;
        outs[i].spk_len = (size_t)spk_len;
        outs[i].op_return = (spk_len >= 1 && tx[off] == 0x6a); /* OP_RETURN */
        off += spk_len;
        if (!outs[i].op_return) { reward_idx = (int64_t)i; reward_count++; }
    }

    /* Skip the input witness (single input) to reach the locktime. */
    if (has_witness) {
        uint64_t stack = 0;
        if (rd_varint(tx, txlen, &off, &stack) < 0) { set_err(errbuf, errlen, "truncated witness"); goto done; }
        for (uint64_t i = 0; i < stack; i++) {
            uint64_t il = 0;
            if (rd_varint(tx, txlen, &off, &il) < 0) { set_err(errbuf, errlen, "truncated witness item"); goto done; }
            if (il > (uint64_t)(txlen - off)) { set_err(errbuf, errlen, "truncated witness item"); goto done; }
            off += il;
        }
    }

    uint32_t locktime;
    if (rd_u32(tx, txlen, &off, &locktime) < 0) { set_err(errbuf, errlen, "truncated locktime"); goto done; }
    if (off != txlen) { set_err(errbuf, errlen, "coinbasetxn trailing bytes"); goto done; }

    /* The server pays the reward to exactly one spendable output; we hand
     * that value to the miner. Refuse to guess if that assumption breaks. */
    if (reward_count != 1) {
        set_err(errbuf, errlen, "coinbasetxn has %d spendable outputs (expected 1)", reward_count);
        goto done;
    }
    int64_t reward = (int64_t)outs[reward_idx].value;

    /* Resolve miner scriptPubKey + fee split (mirrors coinbase_build_split). */
    uint8_t miner_spk[64]; size_t miner_spk_len = 0;
    if (coinbase_address_to_script(miner_address, miner_spk, sizeof miner_spk,
                                   &miner_spk_len, errbuf, errlen) < 0) goto done;

    int64_t fee_sats = 0, miner_sats = reward;
    uint8_t operator_spk[64]; size_t operator_spk_len = 0;
    int has_operator = 0;
    if (operator_address && operator_address[0] && fee_bps > 0 && reward > 0) {
        fee_sats = (reward * (int64_t)fee_bps) / 10000;
        if (fee_sats >= COINBASE_DUST_SATS) {
            if (coinbase_address_to_script(operator_address, operator_spk,
                                           sizeof operator_spk, &operator_spk_len,
                                           errbuf, errlen) < 0) goto done;
            miner_sats = reward - fee_sats;
            has_operator = 1;
        } else {
            fee_sats = 0;
        }
    }

    /* Optional coinbase tag, appended into the scriptSig. */
    uint8_t tag_push[80]; size_t tag_push_len = 0;
    if (coinbase_tag && *coinbase_tag) {
        size_t tlen = strlen(coinbase_tag);
        if (tlen > 75) tlen = 75;
        tag_push[0] = (uint8_t)tlen;
        memcpy(tag_push + 1, coinbase_tag, tlen);
        tag_push_len = tlen + 1;
    }

    /* New scriptSig = server scriptSig (BIP34 height + any server data) +
     * tag + extranonce placeholder. Coinbase scriptSig is capped at 100. */
    size_t en_total = extranonce1_size + extranonce2_size;
    uint64_t new_ss_len = ss_len + tag_push_len + en_total;
    if (new_ss_len < 2 || new_ss_len > 100) {
        set_err(errbuf, errlen, "coinbase scriptSig length %llu out of range",
                (unsigned long long)new_ss_len);
        goto done;
    }

    /* cb1: version | vin(1) | prevout(36) | varint(scriptSig_len) |
     *      server scriptSig | tag   (extranonce slots follow). */
    if (bbuf_push_u32_le(&c1, version) < 0) { set_err(errbuf, errlen, "oom"); goto done; }
    if (bbuf_push_varint(&c1, 1) < 0) { set_err(errbuf, errlen, "oom"); goto done; }
    if (bbuf_push(&c1, tx + prevout_off, 36) < 0) { set_err(errbuf, errlen, "oom"); goto done; }
    if (bbuf_push_varint(&c1, new_ss_len) < 0) { set_err(errbuf, errlen, "oom"); goto done; }
    if (ss_len && bbuf_push(&c1, tx + ss_off, (size_t)ss_len) < 0) { set_err(errbuf, errlen, "oom"); goto done; }
    if (tag_push_len && bbuf_push(&c1, tag_push, tag_push_len) < 0) { set_err(errbuf, errlen, "oom"); goto done; }

    /* Outputs: replace the reward output, preserve everything else in order. */
    uint64_t new_vout = vout + (has_operator ? 1u : 0u);
    for (uint64_t i = 0; i < vout; i++) {
        if ((int64_t)i == reward_idx) {
            if (bbuf_push_u64_le(&ob, (uint64_t)miner_sats) < 0) { set_err(errbuf, errlen, "oom"); goto done; }
            if (bbuf_push_varint(&ob, miner_spk_len) < 0) { set_err(errbuf, errlen, "oom"); goto done; }
            if (bbuf_push(&ob, miner_spk, miner_spk_len) < 0) { set_err(errbuf, errlen, "oom"); goto done; }
            if (has_operator) {
                if (bbuf_push_u64_le(&ob, (uint64_t)fee_sats) < 0) { set_err(errbuf, errlen, "oom"); goto done; }
                if (bbuf_push_varint(&ob, operator_spk_len) < 0) { set_err(errbuf, errlen, "oom"); goto done; }
                if (bbuf_push(&ob, operator_spk, operator_spk_len) < 0) { set_err(errbuf, errlen, "oom"); goto done; }
            }
        } else {
            if (bbuf_push_u64_le(&ob, outs[i].value) < 0) { set_err(errbuf, errlen, "oom"); goto done; }
            if (bbuf_push_varint(&ob, outs[i].spk_len) < 0) { set_err(errbuf, errlen, "oom"); goto done; }
            if (bbuf_push(&ob, tx + outs[i].spk_off, outs[i].spk_len) < 0) { set_err(errbuf, errlen, "oom"); goto done; }
        }
    }

    /* cb2: sequence | varint(out_count) | outputs | locktime. */
    if (bbuf_push_u32_le(&c2, sequence) < 0) { set_err(errbuf, errlen, "oom"); goto done; }
    if (bbuf_push_varint(&c2, new_vout) < 0) { set_err(errbuf, errlen, "oom"); goto done; }
    if (bbuf_push(&c2, ob.data, ob.len) < 0) { set_err(errbuf, errlen, "oom"); goto done; }
    if (bbuf_push_u32_le(&c2, locktime) < 0) { set_err(errbuf, errlen, "oom"); goto done; }

    /* Success — hand ownership of c1/c2 buffers to the caller. */
    out->cb1 = c1.data; out->cb1_len = c1.len; c1.data = NULL;
    out->cb2 = c2.data; out->cb2_len = c2.len; c2.data = NULL;
    if (out_has_witness) *out_has_witness = has_witness;
    if (out_miner_sats)  *out_miner_sats  = miner_sats;
    if (out_fee_sats)    *out_fee_sats    = fee_sats;
    ret = 0;

done:
    bbuf_free(&c1);
    bbuf_free(&ob);
    bbuf_free(&c2);
    free(outs);
    free(tx);
    return ret;
}

int coinbase_build_from_template_multi(const char *coinbase_tx_hex,
                                       const coinbase_payout_t *payouts,
                                       size_t n_payouts,
                                       const char *operator_address,
                                       int fee_bps,
                                       const char *coinbase_tag,
                                       size_t extranonce1_size,
                                       size_t extranonce2_size,
                                       coinbase_parts_t *out,
                                       int *out_has_witness,
                                       int64_t *out_total_payout_sats,
                                       int64_t *out_fee_sats,
                                       char *errbuf, size_t errlen) {
    if (!out || !coinbase_tx_hex || !payouts || n_payouts == 0) {
        set_err(errbuf, errlen, "null arg or no payouts");
        return -1;
    }
    out->cb1 = NULL; out->cb1_len = 0;
    out->cb2 = NULL; out->cb2_len = 0;
    if (out_has_witness) *out_has_witness = 0;
    if (out_total_payout_sats) *out_total_payout_sats = 0;
    if (out_fee_sats) *out_fee_sats = 0;

    struct cb_out { uint64_t value; size_t spk_off; size_t spk_len; int op_return; };

    uint8_t *tx = NULL;
    struct cb_out *outs = NULL;
    uint8_t *payout_spk = NULL;
    size_t *payout_spk_len = NULL;
    bbuf_t c1, ob, c2;
    bbuf_init(&c1); bbuf_init(&ob); bbuf_init(&c2);
    int ret = -1;

    size_t hexlen = strlen(coinbase_tx_hex);
    if (hexlen % 2 != 0) { set_err(errbuf, errlen, "odd coinbasetxn hex"); goto done; }
    size_t txlen = hexlen / 2;
    tx = (uint8_t *)malloc(txlen ? txlen : 1);
    if (!tx) { set_err(errbuf, errlen, "oom"); goto done; }
    size_t dl = 0;
    if (hex_decode(coinbase_tx_hex, tx, txlen, &dl) < 0 || dl != txlen) {
        set_err(errbuf, errlen, "bad coinbasetxn hex");
        goto done;
    }

    size_t off = 0;
    uint32_t version;
    if (rd_u32(tx, txlen, &off, &version) < 0) { set_err(errbuf, errlen, "truncated coinbasetxn"); goto done; }

    int has_witness = 0;
    if (off + 2 <= txlen && tx[off] == 0x00 && tx[off + 1] != 0x00) {
        has_witness = 1;
        off += 2;
    }

    uint64_t vin = 0;
    if (rd_varint(tx, txlen, &off, &vin) < 0) { set_err(errbuf, errlen, "truncated coinbasetxn"); goto done; }
    if (vin != 1) { set_err(errbuf, errlen, "coinbasetxn input count %llu != 1", (unsigned long long)vin); goto done; }

    size_t prevout_off = off;
    if (off + 36 > txlen) { set_err(errbuf, errlen, "truncated coinbasetxn input"); goto done; }
    off += 36;
    uint64_t ss_len = 0;
    if (rd_varint(tx, txlen, &off, &ss_len) < 0) { set_err(errbuf, errlen, "truncated scriptSig"); goto done; }
    size_t ss_off = off;
    if (ss_len > (uint64_t)(txlen - off)) { set_err(errbuf, errlen, "truncated scriptSig"); goto done; }
    off += ss_len;
    uint32_t sequence;
    if (rd_u32(tx, txlen, &off, &sequence) < 0) { set_err(errbuf, errlen, "truncated sequence"); goto done; }

    uint64_t vout = 0;
    if (rd_varint(tx, txlen, &off, &vout) < 0) { set_err(errbuf, errlen, "truncated output count"); goto done; }
    if (vout == 0) { set_err(errbuf, errlen, "coinbasetxn has no outputs"); goto done; }

    outs = (struct cb_out *)calloc((size_t)vout, sizeof(*outs));
    if (!outs) { set_err(errbuf, errlen, "oom"); goto done; }

    int64_t reward_idx = -1;
    int reward_count = 0;
    for (uint64_t i = 0; i < vout; i++) {
        uint64_t val = 0, spk_len = 0;
        if (rd_u64(tx, txlen, &off, &val) < 0) { set_err(errbuf, errlen, "truncated output value"); goto done; }
        if (rd_varint(tx, txlen, &off, &spk_len) < 0) { set_err(errbuf, errlen, "truncated scriptPubKey"); goto done; }
        if (spk_len > (uint64_t)(txlen - off)) { set_err(errbuf, errlen, "truncated scriptPubKey"); goto done; }
        outs[i].value = val;
        outs[i].spk_off = off;
        outs[i].spk_len = (size_t)spk_len;
        outs[i].op_return = (spk_len >= 1 && tx[off] == 0x6a);
        off += spk_len;
        if (!outs[i].op_return) { reward_idx = (int64_t)i; reward_count++; }
    }

    if (has_witness) {
        uint64_t stack = 0;
        if (rd_varint(tx, txlen, &off, &stack) < 0) { set_err(errbuf, errlen, "truncated witness"); goto done; }
        for (uint64_t i = 0; i < stack; i++) {
            uint64_t il = 0;
            if (rd_varint(tx, txlen, &off, &il) < 0) { set_err(errbuf, errlen, "truncated witness item"); goto done; }
            if (il > (uint64_t)(txlen - off)) { set_err(errbuf, errlen, "truncated witness item"); goto done; }
            off += il;
        }
    }

    uint32_t locktime;
    if (rd_u32(tx, txlen, &off, &locktime) < 0) { set_err(errbuf, errlen, "truncated locktime"); goto done; }
    if (off != txlen) { set_err(errbuf, errlen, "coinbasetxn trailing bytes"); goto done; }

    if (reward_count != 1) {
        set_err(errbuf, errlen, "coinbasetxn has %d spendable outputs (expected 1)", reward_count);
        goto done;
    }
    int64_t reward = (int64_t)outs[reward_idx].value;

    /* Compute fee and verify payouts cover the remainder exactly. */
    int64_t fee_sats = 0;
    int has_operator = 0;
    uint8_t operator_spk[64]; size_t operator_spk_len = 0;
    if (operator_address && operator_address[0] && fee_bps > 0 && reward > 0) {
        fee_sats = (reward * (int64_t)fee_bps) / 10000;
        if (fee_sats >= COINBASE_DUST_SATS) {
            if (coinbase_address_to_script(operator_address, operator_spk,
                                           sizeof operator_spk, &operator_spk_len,
                                           errbuf, errlen) < 0) goto done;
            has_operator = 1;
        } else {
            fee_sats = 0;
        }
    }

    int64_t total_payout = 0;
    for (size_t i = 0; i < n_payouts; i++) {
        if (!payouts[i].address || payouts[i].address[0] == '\0') {
            set_err(errbuf, errlen, "payout %zu has empty address", i);
            goto done;
        }
        if (payouts[i].sats < COINBASE_DUST_SATS) {
            set_err(errbuf, errlen, "payout %zu is below dust (%lld < %d)",
                    i, (long long)payouts[i].sats, COINBASE_DUST_SATS);
            goto done;
        }
        total_payout += payouts[i].sats;
    }
    if (total_payout + fee_sats != reward) {
        set_err(errbuf, errlen,
                "payout total %lld + fee %lld != reward %lld",
                (long long)total_payout, (long long)fee_sats, (long long)reward);
        goto done;
    }

    /* Pre-resolve every payout scriptPubKey. */
    payout_spk = (uint8_t *)calloc(n_payouts, 64);
    payout_spk_len = (size_t *)calloc(n_payouts, sizeof(*payout_spk_len));
    if (!payout_spk || !payout_spk_len) { set_err(errbuf, errlen, "oom"); goto done; }
    for (size_t i = 0; i < n_payouts; i++) {
        if (coinbase_address_to_script(payouts[i].address, payout_spk + i * 64,
                                       64, &payout_spk_len[i],
                                       errbuf, errlen) < 0) goto done;
    }

    /* Optional coinbase tag, appended into the scriptSig. */
    uint8_t tag_push[80]; size_t tag_push_len = 0;
    if (coinbase_tag && *coinbase_tag) {
        size_t tlen = strlen(coinbase_tag);
        if (tlen > 75) tlen = 75;
        tag_push[0] = (uint8_t)tlen;
        memcpy(tag_push + 1, coinbase_tag, tlen);
        tag_push_len = tlen + 1;
    }

    size_t en_total = extranonce1_size + extranonce2_size;
    uint64_t new_ss_len = ss_len + tag_push_len + en_total;
    if (new_ss_len < 2 || new_ss_len > 100) {
        set_err(errbuf, errlen, "coinbase scriptSig length %llu out of range",
                (unsigned long long)new_ss_len);
        goto done;
    }

    /* cb1: version | vin(1) | prevout(36) | varint(scriptSig_len) |
     *      server scriptSig | tag. */
    if (bbuf_push_u32_le(&c1, version) < 0) { set_err(errbuf, errlen, "oom"); goto done; }
    if (bbuf_push_varint(&c1, 1) < 0) { set_err(errbuf, errlen, "oom"); goto done; }
    if (bbuf_push(&c1, tx + prevout_off, 36) < 0) { set_err(errbuf, errlen, "oom"); goto done; }
    if (bbuf_push_varint(&c1, new_ss_len) < 0) { set_err(errbuf, errlen, "oom"); goto done; }
    if (ss_len && bbuf_push(&c1, tx + ss_off, (size_t)ss_len) < 0) { set_err(errbuf, errlen, "oom"); goto done; }
    if (tag_push_len && bbuf_push(&c1, tag_push, tag_push_len) < 0) { set_err(errbuf, errlen, "oom"); goto done; }

    /* Outputs: replace the reward output with N payout outputs + operator. */
    uint64_t new_vout = vout - 1 + n_payouts + (has_operator ? 1u : 0u);
    for (uint64_t i = 0; i < vout; i++) {
        if ((int64_t)i == reward_idx) {
            for (size_t k = 0; k < n_payouts; k++) {
                if (bbuf_push_u64_le(&ob, (uint64_t)payouts[k].sats) < 0) { set_err(errbuf, errlen, "oom"); goto done; }
                if (bbuf_push_varint(&ob, payout_spk_len[k]) < 0) { set_err(errbuf, errlen, "oom"); goto done; }
                if (bbuf_push(&ob, payout_spk + k * 64, payout_spk_len[k]) < 0) { set_err(errbuf, errlen, "oom"); goto done; }
            }
            if (has_operator) {
                if (bbuf_push_u64_le(&ob, (uint64_t)fee_sats) < 0) { set_err(errbuf, errlen, "oom"); goto done; }
                if (bbuf_push_varint(&ob, operator_spk_len) < 0) { set_err(errbuf, errlen, "oom"); goto done; }
                if (bbuf_push(&ob, operator_spk, operator_spk_len) < 0) { set_err(errbuf, errlen, "oom"); goto done; }
            }
        } else {
            if (bbuf_push_u64_le(&ob, outs[i].value) < 0) { set_err(errbuf, errlen, "oom"); goto done; }
            if (bbuf_push_varint(&ob, outs[i].spk_len) < 0) { set_err(errbuf, errlen, "oom"); goto done; }
            if (bbuf_push(&ob, tx + outs[i].spk_off, outs[i].spk_len) < 0) { set_err(errbuf, errlen, "oom"); goto done; }
        }
    }

    /* cb2: sequence | varint(out_count) | outputs | locktime. */
    if (bbuf_push_u32_le(&c2, sequence) < 0) { set_err(errbuf, errlen, "oom"); goto done; }
    if (bbuf_push_varint(&c2, new_vout) < 0) { set_err(errbuf, errlen, "oom"); goto done; }
    if (bbuf_push(&c2, ob.data, ob.len) < 0) { set_err(errbuf, errlen, "oom"); goto done; }
    if (bbuf_push_u32_le(&c2, locktime) < 0) { set_err(errbuf, errlen, "oom"); goto done; }

    out->cb1 = c1.data; out->cb1_len = c1.len; c1.data = NULL;
    out->cb2 = c2.data; out->cb2_len = c2.len; c2.data = NULL;
    if (out_has_witness) *out_has_witness = has_witness;
    if (out_total_payout_sats) *out_total_payout_sats = total_payout;
    if (out_fee_sats) *out_fee_sats = fee_sats;
    ret = 0;

 done:
    bbuf_free(&c1);
    bbuf_free(&ob);
    bbuf_free(&c2);
    free(payout_spk);
    free(payout_spk_len);
    free(outs);
    free(tx);
    return ret;
}


/* Count the outputs of a serialized coinbase, split into spendable and
 * OP_RETURN.
 *
 * The OP_RETURN count is the interesting one: a coinbase built from
 * "coinbasevalue" carries exactly one (the witness commitment), while one
 * dictated by the CUSF enforcer carries the mandatory BIP300/301 commitments
 * alongside it. Surfacing the number is how an observer can tell, from the
 * outside, whether the blocks this pool produces can have a sidechain
 * merge-mined into them at all.
 *
 * Parses only far enough to walk the output list. Returns 0 on success,
 * negative on malformed input; counts are untouched on failure. */
size_t coinbase_max_payout_outputs(int64_t weight_limit,
                                   int64_t tx_weight_total,
                                   size_t template_coinbase_bytes,
                                   size_t scriptsig_growth_bytes,
                                   int fee_output,
                                   size_t ceiling,
                                   int64_t *out_headroom_wu) {
    if (out_headroom_wu) *out_headroom_wu = -1;
    if (ceiling < 1) ceiling = 1;
    /* No weightlimit from the server: nothing to measure against. */
    if (weight_limit <= 0) return ceiling;

    int64_t headroom = weight_limit
                     - tx_weight_total
                     - (int64_t)template_coinbase_bytes * 4
                     - (int64_t)scriptsig_growth_bytes * 4
                     - COINBASE_WEIGHT_SAFETY_WU;
    if (out_headroom_wu) *out_headroom_wu = headroom;
    if (headroom < 0) return 1;

    /* The template already budgeted one spendable output, so the first payout
     * costs nothing extra; the operator fee output takes one of the slots. */
    int64_t allowed = 1 + headroom / COINBASE_PAYOUT_TXOUT_WU - (fee_output ? 1 : 0);
    if (allowed < 1) allowed = 1;
    return ((uint64_t)allowed < (uint64_t)ceiling) ? (size_t)allowed : ceiling;
}

int coinbase_template_reward(const char *coinbase_tx_hex, int64_t *out_reward,
                             char *errbuf, size_t errlen) {
    if (!coinbase_tx_hex || !out_reward) {
        set_err(errbuf, errlen, "null arg");
        return -1;
    }
    *out_reward = 0;

    size_t hexlen = strlen(coinbase_tx_hex);
    if (hexlen % 2 != 0) { set_err(errbuf, errlen, "odd coinbasetxn hex"); return -1; }
    size_t txlen = hexlen / 2;
    uint8_t *tx = (uint8_t *)malloc(txlen ? txlen : 1);
    if (!tx) { set_err(errbuf, errlen, "oom"); return -1; }
    size_t dl = 0;
    int ret = -1;
    if (hex_decode(coinbase_tx_hex, tx, txlen, &dl) < 0 || dl != txlen) {
        set_err(errbuf, errlen, "bad coinbasetxn hex");
        goto done;
    }

    size_t off = 0;
    uint32_t version;
    if (rd_u32(tx, txlen, &off, &version) < 0) { set_err(errbuf, errlen, "truncated coinbasetxn"); goto done; }
    if (off + 2 <= txlen && tx[off] == 0x00 && tx[off + 1] != 0x00) off += 2; /* segwit marker */

    uint64_t vin = 0;
    if (rd_varint(tx, txlen, &off, &vin) < 0) { set_err(errbuf, errlen, "truncated coinbasetxn"); goto done; }
    if (vin != 1) { set_err(errbuf, errlen, "coinbasetxn input count %llu != 1", (unsigned long long)vin); goto done; }
    if (off + 36 > txlen) { set_err(errbuf, errlen, "truncated coinbasetxn input"); goto done; }
    off += 36;
    uint64_t ss_len = 0;
    if (rd_varint(tx, txlen, &off, &ss_len) < 0) { set_err(errbuf, errlen, "truncated scriptSig"); goto done; }
    if (ss_len > (uint64_t)(txlen - off)) { set_err(errbuf, errlen, "truncated scriptSig"); goto done; }
    off += ss_len;
    uint32_t sequence;
    if (rd_u32(tx, txlen, &off, &sequence) < 0) { set_err(errbuf, errlen, "truncated sequence"); goto done; }

    uint64_t vout = 0;
    if (rd_varint(tx, txlen, &off, &vout) < 0) { set_err(errbuf, errlen, "truncated output count"); goto done; }
    if (vout == 0) { set_err(errbuf, errlen, "coinbasetxn has no outputs"); goto done; }

    int spendable = 0;
    uint64_t reward = 0;
    for (uint64_t i = 0; i < vout; i++) {
        uint64_t val = 0, spk_len = 0;
        if (rd_u64(tx, txlen, &off, &val) < 0) { set_err(errbuf, errlen, "truncated output value"); goto done; }
        if (rd_varint(tx, txlen, &off, &spk_len) < 0) { set_err(errbuf, errlen, "truncated scriptPubKey"); goto done; }
        if (spk_len > (uint64_t)(txlen - off)) { set_err(errbuf, errlen, "truncated scriptPubKey"); goto done; }
        int op_return = (spk_len >= 1 && tx[off] == 0x6a);
        off += spk_len;
        if (!op_return) { spendable++; reward = val; }
    }
    if (spendable != 1) {
        set_err(errbuf, errlen, "coinbasetxn has %d spendable outputs (expected 1)", spendable);
        goto done;
    }
    if (reward > (uint64_t)INT64_MAX) { set_err(errbuf, errlen, "reward overflows int64"); goto done; }

    *out_reward = (int64_t)reward;
    ret = 0;
done:
    free(tx);
    return ret;
}

int coinbase_count_outputs(const char *tx_hex, int *spendable_out,
                           int *op_return_out) {
    if (!tx_hex) return -1;
    size_t hexlen = strlen(tx_hex);
    if (hexlen % 2 != 0) return -1;
    size_t txlen = hexlen / 2;
    uint8_t *tx = (uint8_t *)malloc(txlen ? txlen : 1);
    if (!tx) return -1;

    int ret = -1;
    size_t dl = 0;
    if (hex_decode(tx_hex, tx, txlen, &dl) < 0 || dl != txlen) goto done;

    size_t off = 0;
    uint32_t version;
    if (rd_u32(tx, txlen, &off, &version) < 0) goto done;
    /* Optional segwit marker+flag. */
    if (off + 2 <= txlen && tx[off] == 0x00 && tx[off + 1] != 0x00) off += 2;

    uint64_t vin = 0;
    if (rd_varint(tx, txlen, &off, &vin) < 0) goto done;
    for (uint64_t i = 0; i < vin; i++) {
        if (off + 36 > txlen) goto done;
        off += 36;
        uint64_t ss = 0;
        if (rd_varint(tx, txlen, &off, &ss) < 0) goto done;
        if (txlen - off < 4 || ss > (uint64_t)(txlen - off - 4)) goto done;
        off += (size_t)ss + 4;   /* scriptSig + sequence */
    }

    uint64_t vout = 0;
    if (rd_varint(tx, txlen, &off, &vout) < 0) goto done;
    int spendable = 0, op_returns = 0;
    for (uint64_t i = 0; i < vout; i++) {
        uint64_t val = 0, spk_len = 0;
        if (rd_u64(tx, txlen, &off, &val) < 0) goto done;
        if (rd_varint(tx, txlen, &off, &spk_len) < 0) goto done;
        if (spk_len > (uint64_t)(txlen - off)) goto done;
        if (spk_len >= 1 && tx[off] == 0x6a) op_returns++;   /* OP_RETURN */
        else spendable++;
        off += (size_t)spk_len;
    }

    if (spendable_out)  *spendable_out  = spendable;
    if (op_return_out)  *op_return_out  = op_returns;
    ret = 0;
done:
    free(tx);
    return ret;
}
