/* evict.c — see evict.h for why. */
#include "evict.h"
#include "share.h"      /* dsha256 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

static void set_err(char *errbuf, size_t errlen, const char *fmt, ...) {
    if (!errbuf || errlen == 0) return;
    va_list ap; va_start(ap, fmt);
    vsnprintf(errbuf, errlen, fmt, ap);
    va_end(ap);
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decode hex into a fresh buffer. Returns NULL on odd length / bad digit. */
static uint8_t *hex_alloc(const char *hex, size_t *out_len) {
    size_t n = strlen(hex);
    if (n % 2) return NULL;
    uint8_t *b = (uint8_t *)malloc(n / 2 ? n / 2 : 1);
    if (!b) return NULL;
    for (size_t i = 0; i < n / 2; i++) {
        int hi = hex_nibble(hex[2 * i]), lo = hex_nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) { free(b); return NULL; }
        b[i] = (uint8_t)((hi << 4) | lo);
    }
    *out_len = n / 2;
    return b;
}

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

/* Merkle root over `n` 32-byte leaves, Bitcoin rules (odd level: duplicate
 * the last node). leaves is modified in place as scratch. */
static void merkle_root_inplace(uint8_t (*leaves)[32], size_t n, uint8_t out[32]) {
    if (n == 0) { memset(out, 0, 32); return; }
    while (n > 1) {
        size_t m = (n + 1) / 2;
        for (size_t i = 0; i < m; i++) {
            uint8_t pair[64];
            memcpy(pair, leaves[2 * i], 32);
            memcpy(pair + 32, (2 * i + 1 < n) ? leaves[2 * i + 1] : leaves[2 * i], 32);
            dsha256(pair, 64, leaves[i]);
        }
        n = m;
    }
    memcpy(out, leaves[0], 32);
}

int evict_witness_commitment(const bitcoind_template_t *t, uint8_t out[32],
                             char *errbuf, size_t errlen) {
    if (!t || !out) { set_err(errbuf, errlen, "null arg"); return -1; }
    /* Leaf 0 is the coinbase's wtxid, defined as 32 zero bytes (BIP141). */
    uint8_t (*leaves)[32] = (uint8_t (*)[32])calloc(t->tx_count + 1, 32);
    if (!leaves) { set_err(errbuf, errlen, "oom"); return -1; }
    for (size_t i = 0; i < t->tx_count; i++) {
        size_t n = 0;
        uint8_t *raw = t->txs[i].data_hex ? hex_alloc(t->txs[i].data_hex, &n) : NULL;
        if (!raw || n == 0) {
            free(raw); free(leaves);
            set_err(errbuf, errlen, "tx %zu: bad data hex", i);
            return -1;
        }
        /* wtxid = sha256d of the FULL serialization (witness included when
         * present). For a non-segwit tx that is the txid. The template hands us
         * the full serialization in `data`, so no re-encoding is needed. */
        dsha256(raw, n, leaves[i + 1]);
        free(raw);
    }
    uint8_t root[32];
    merkle_root_inplace(leaves, t->tx_count + 1, root);
    free(leaves);
    /* Reserved value: 32 zero bytes — the value stratum.c writes into the
     * submitted coinbase's witness, and what the enforcer's own commitment
     * assumes. The safety gate in evict_tail_for_coinbase proves that. */
    uint8_t pre[64];
    memcpy(pre, root, 32);
    memset(pre + 32, 0, 32);
    dsha256(pre, 64, out);
    return 0;
}

/* The 32 commitment bytes inside a default_witness_commitment scriptPubKey
 * hex ("6a24aa21a9ed" + 64 hex). Returns 0 ok, -1 if it is not that shape. */
static int commitment_from_spk_hex(const char *spk_hex, uint8_t out[32]) {
    if (!spk_hex || strlen(spk_hex) != 12 + 64) return -1;
    if (strncmp(spk_hex, "6a24aa21a9ed", 12) != 0) return -1;
    for (int i = 0; i < 32; i++) {
        int hi = hex_nibble(spk_hex[12 + 2 * i]), lo = hex_nibble(spk_hex[12 + 2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

/* Walk a template coinbase once. Finds the byte offset of the single
 * spendable output's VALUE and of the witness-commitment output's
 * scriptPubKey (the 38-byte 6a24aa21a9ed<32> one). Either may be absent
 * (offset 0, count reported). Returns 0 ok, -1 malformed. */
static int cb_locate(const uint8_t *tx, size_t len,
                     size_t *value_off, int *spendable,
                     size_t *wc_spk_off, int *wc_count,
                     char *errbuf, size_t errlen) {
    *value_off = 0; *spendable = 0; *wc_spk_off = 0; *wc_count = 0;
    size_t off = 4;                                   /* version */
    if (off + 2 <= len && tx[off] == 0x00 && tx[off + 1] != 0x00) off += 2; /* segwit marker+flag */
    uint64_t vin = 0, vout = 0;
    if (rd_varint(tx, len, &off, &vin) < 0 || vin != 1) { set_err(errbuf, errlen, "coinbase vin != 1"); return -1; }
    off += 36;                                        /* prevout */
    uint64_t sl = 0;
    if (rd_varint(tx, len, &off, &sl) < 0 || off + sl + 4 > len) { set_err(errbuf, errlen, "truncated scriptsig"); return -1; }
    off += sl + 4;                                    /* scriptsig + sequence */
    if (rd_varint(tx, len, &off, &vout) < 0) { set_err(errbuf, errlen, "truncated vout"); return -1; }
    for (uint64_t i = 0; i < vout; i++) {
        size_t voff = off;
        if (off + 8 > len) { set_err(errbuf, errlen, "truncated output"); return -1; }
        off += 8;
        uint64_t spk = 0;
        if (rd_varint(tx, len, &off, &spk) < 0 || off + spk > len) { set_err(errbuf, errlen, "truncated spk"); return -1; }
        int op_return = (spk >= 1 && tx[off] == 0x6a);
        if (!op_return) { (*spendable)++; *value_off = voff; }
        if (spk == 38 && memcmp(tx + off, "\x6a\x24\xaa\x21\xa9\xed", 6) == 0) { (*wc_count)++; *wc_spk_off = off; }
        off += spk;
    }
    return 0;
}

/* Overwrite the 32 commitment bytes inside the coinbase's own OP_RETURN
 * output (hex in place). This is the output the pool copies VERBATIM into
 * every coinbase it renders (coinbase.c copies non-reward outputs as-is), so
 * the field default_witness_commitment alone would never reach a block —
 * the round-1 review of 3708a1a found exactly that. Returns 0 ok, -1 when
 * the output is absent or present more than once. */
static int cb_rewrite_commitment(char *hex, const uint8_t wc[32], char *errbuf, size_t errlen) {
    size_t len = 0; uint8_t *tx = hex_alloc(hex, &len);
    if (!tx) { set_err(errbuf, errlen, "bad coinbase hex"); return -1; }
    size_t voff, woff; int sp, wn;
    int rc = cb_locate(tx, len, &voff, &sp, &woff, &wn, errbuf, errlen);
    free(tx);
    if (rc < 0) return -1;
    if (wn != 1) { set_err(errbuf, errlen, "coinbase carries %d 38-byte witness-commitment (6a24aa21a9ed) outputs (expected exactly 1)", wn); return -1; }
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        hex[2 * (woff + 6 + i)]     = digits[wc[i] >> 4];
        hex[2 * (woff + 6 + i) + 1] = digits[wc[i] & 0x0f];
    }
    return 0;
}

/* The 32 commitment bytes embedded in the coinbase's OP_RETURN, or -1. */
static int cb_embedded_commitment(const char *hex, uint8_t out[32], char *errbuf, size_t errlen) {
    size_t len = 0; uint8_t *tx = hex_alloc(hex, &len);
    if (!tx) { set_err(errbuf, errlen, "bad coinbase hex"); return -1; }
    size_t voff, woff; int sp, wn;
    int rc = cb_locate(tx, len, &voff, &sp, &woff, &wn, errbuf, errlen);
    if (rc == 0 && wn != 1) { set_err(errbuf, errlen, "coinbase carries %d 38-byte witness-commitment (6a24aa21a9ed) outputs (expected exactly 1)", wn); rc = -1; }
    if (rc == 0) memcpy(out, tx + woff + 6, 32);
    free(tx);
    return rc;
}

int evict_adjust_template_reward(char *hex, int64_t delta_sats,
                                 char *errbuf, size_t errlen) {
    if (!hex) { set_err(errbuf, errlen, "null coinbase"); return -1; }
    size_t len = 0;
    uint8_t *tx = hex_alloc(hex, &len);
    if (!tx) { set_err(errbuf, errlen, "bad coinbase hex"); return -1; }
    int ret = -1;
    size_t off = 4;                                   /* version */
    if (off + 2 <= len && tx[off] == 0x00 && tx[off + 1] != 0x00) off += 2; /* segwit marker+flag */
    uint64_t vin = 0, vout = 0;
    if (rd_varint(tx, len, &off, &vin) < 0 || vin != 1) { set_err(errbuf, errlen, "coinbase vin != 1"); goto done; }
    off += 36;                                        /* prevout */
    uint64_t sl = 0;
    if (rd_varint(tx, len, &off, &sl) < 0 || off + sl + 4 > len) { set_err(errbuf, errlen, "truncated scriptsig"); goto done; }
    off += sl + 4;                                    /* scriptsig + sequence */
    if (rd_varint(tx, len, &off, &vout) < 0) { set_err(errbuf, errlen, "truncated vout"); goto done; }
    size_t value_off = 0; int spendable = 0;
    for (uint64_t i = 0; i < vout; i++) {
        size_t voff = off;
        if (off + 8 > len) { set_err(errbuf, errlen, "truncated output"); goto done; }
        off += 8;
        uint64_t spk = 0;
        if (rd_varint(tx, len, &off, &spk) < 0 || off + spk > len) { set_err(errbuf, errlen, "truncated spk"); goto done; }
        int op_return = (spk >= 1 && tx[off] == 0x6a);
        if (!op_return) { spendable++; value_off = voff; }
        off += spk;
    }
    if (spendable != 1) { set_err(errbuf, errlen, "coinbase has %d spendable outputs (expected 1)", spendable); goto done; }
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)tx[value_off + i] << (8 * i);
    if (delta_sats < 0 && (uint64_t)(-delta_sats) > v) { set_err(errbuf, errlen, "reward would go negative"); goto done; }
    v = (uint64_t)((int64_t)v + delta_sats);
    /* Rewrite the 8 little-endian bytes in the hex, same length. */
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 8; i++) {
        uint8_t b = (uint8_t)(v >> (8 * i));
        hex[2 * (value_off + i)]     = digits[b >> 4];
        hex[2 * (value_off + i) + 1] = digits[b & 0x0f];
    }
    ret = 0;
done:
    free(tx);
    return ret;
}

int evict_tail_for_coinbase(bitcoind_template_t *t, int64_t need_wu,
                            int64_t *out_evicted_wu, int64_t *out_evicted_fee,
                            char *errbuf, size_t errlen) {
    if (out_evicted_wu)  *out_evicted_wu  = 0;
    if (out_evicted_fee) *out_evicted_fee = 0;
    if (!t) { set_err(errbuf, errlen, "null template"); return -1; }
    if (!t->coinbasetxn_hex || !t->coinbasetxn_hex[0]) { set_err(errbuf, errlen, "no coinbasetxn"); return -1; }
    if (t->weight_limit <= 0) { set_err(errbuf, errlen, "no weightlimit"); return -1; }
    if (!t->default_witness_commitment) { set_err(errbuf, errlen, "no default_witness_commitment"); return -1; }

    int64_t tx_weight = 0;
    for (size_t i = 0; i < t->tx_count; i++) tx_weight += t->txs[i].weight;
    int64_t budget = t->weight_limit - need_wu;
    if (tx_weight <= budget) return 0;                /* already fits */

    /* ⛔ Safety gate: our commitment for the UNCHANGED template must equal the
     * enforcer's. If it does not, our arithmetic is not its arithmetic and we
     * must not touch the template. */
    uint8_t ours[32], theirs[32];
    if (evict_witness_commitment(t, ours, errbuf, errlen) < 0) return -1;
    if (commitment_from_spk_hex(t->default_witness_commitment, theirs) < 0) {
        set_err(errbuf, errlen, "default_witness_commitment is not 6a24aa21a9ed+32 bytes");
        return -1;
    }
    if (memcmp(ours, theirs, 32) != 0) {
        set_err(errbuf, errlen, "witness commitment self-check FAILED for the full template — refusing to evict");
        return -1;
    }
    uint8_t embedded[32];
    if (cb_embedded_commitment(t->coinbasetxn_hex, embedded, errbuf, errlen) < 0) return -1;
    if (memcmp(embedded, theirs, 32) != 0) {
        set_err(errbuf, errlen, "coinbasetxn's embedded commitment differs from default_witness_commitment — refusing to evict");
        return -1;
    }

    /* Drop from the tail until the txs fit inside the budget. */
    size_t keep = t->tx_count;
    int64_t dropped_wu = 0, dropped_fee = 0;
    while (keep > 0 && tx_weight - dropped_wu > budget) {
        keep--;
        dropped_wu  += t->txs[keep].weight;
        dropped_fee += t->txs[keep].fee;
    }
    if (tx_weight - dropped_wu > budget) {           /* cannot happen unless need_wu > limit */
        set_err(errbuf, errlen, "coinbase needs %lld WU, more than the block allows", (long long)need_wu);
        return -1;
    }
    int evicted = (int)(t->tx_count - keep);

    /* Recompute on the shortened list BEFORE freeing anything, so a failure
     * leaves the template untouched. */
    size_t saved = t->tx_count;
    t->tx_count = keep;
    uint8_t wc[32];
    if (evict_witness_commitment(t, wc, errbuf, errlen) < 0) { t->tx_count = saved; return -1; }
    char *spk = (char *)malloc(12 + 64 + 1);
    if (!spk) { t->tx_count = saved; set_err(errbuf, errlen, "oom"); return -1; }
    memcpy(spk, "6a24aa21a9ed", 12);
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { spk[12 + 2 * i] = digits[wc[i] >> 4]; spk[12 + 2 * i + 1] = digits[wc[i] & 0x0f]; }
    spk[76] = '\0';

    /* Reward: the template coinbase paid subsidy + ALL fees; the dropped fees
     * come off. Done on a copy so a parse failure leaves the original intact. */
    char *cb = strdup(t->coinbasetxn_hex);
    if (!cb) { free(spk); t->tx_count = saved; set_err(errbuf, errlen, "oom"); return -1; }
    /* The commitment the block will actually carry lives INSIDE this hex. */
    if (cb_rewrite_commitment(cb, wc, errbuf, errlen) < 0) {
        free(cb); free(spk); t->tx_count = saved; return -1;
    }
    if (dropped_fee > 0 && evict_adjust_template_reward(cb, -dropped_fee, errbuf, errlen) < 0) {
        free(cb); free(spk); t->tx_count = saved; return -1;
    }

    /* Commit: free the dropped entries, swap in the recomputed fields. */
    for (size_t i = keep; i < saved; i++) { free(t->txs[i].data_hex); t->txs[i].data_hex = NULL; }
    free(t->default_witness_commitment); t->default_witness_commitment = spk;
    free(t->coinbasetxn_hex);            t->coinbasetxn_hex = cb;
    t->coinbase_value_sats -= dropped_fee;
    if (out_evicted_wu)  *out_evicted_wu  = dropped_wu;
    if (out_evicted_fee) *out_evicted_fee = dropped_fee;
    return evicted;
}
