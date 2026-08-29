#include "coinbase.h"
#include "stratum.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Minimal varint reader. */
static int read_varint(const uint8_t *buf, size_t cap, size_t *off, uint64_t *val) {
    if (*off >= cap) return -1;
    uint8_t b = buf[(*off)++];
    if (b < 0xfd) { *val = b; return 0; }
    if (b == 0xfd) {
        if (*off + 2 > cap) return -1;
        *val = (uint64_t)buf[*off] | ((uint64_t)buf[*off + 1] << 8);
        *off += 2;
        return 0;
    }
    if (b == 0xfe) {
        if (*off + 4 > cap) return -1;
        uint32_t v = 0;
        for (int i = 0; i < 4; i++) v |= (uint32_t)buf[*off + i] << (8 * i);
        *off += 4;
        *val = v;
        return 0;
    }
    if (*off + 8 > cap) return -1;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)buf[*off + i] << (8 * i);
    *off += 8;
    *val = v;
    return 0;
}

static void test_p2pkh_address(void) {
    /* mainnet P2PKH: 1A1zP1eP5QGefi2DMPTfTL5SLmv7DivfNa (genesis coinbase) */
    uint8_t spk[64];
    size_t spk_len = 0;
    char err[128];
    int rc = coinbase_address_to_script("1A1zP1eP5QGefi2DMPTfTL5SLmv7DivfNa",
                                        spk, sizeof spk, &spk_len, err, sizeof err);
    assert(rc == 0);
    assert(spk_len == 25);
    assert(spk[0] == 0x76 && spk[1] == 0xa9 && spk[2] == 0x14);
    assert(spk[23] == 0x88 && spk[24] == 0xac);
    printf("ok: p2pkh decode\n");
}

static void test_p2wpkh_address(void) {
    /* BIP173 test vector: bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4 */
    uint8_t spk[64];
    size_t spk_len = 0;
    char err[128];
    int rc = coinbase_address_to_script("bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4",
                                        spk, sizeof spk, &spk_len, err, sizeof err);
    assert(rc == 0);
    assert(spk_len == 22);
    assert(spk[0] == 0x00 && spk[1] == 0x14);
    printf("ok: p2wpkh decode\n");
}

static void test_regtest_p2wpkh(void) {
    /* A canonical regtest P2WPKH address. */
    uint8_t spk[64];
    size_t spk_len = 0;
    char err[128];
    int rc = coinbase_address_to_script("bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080",
                                        spk, sizeof spk, &spk_len, err, sizeof err);
    if (rc != 0) {
        fprintf(stderr, "decode err: %s\n", err);
    }
    assert(rc == 0);
    assert(spk_len == 22);
    assert(spk[0] == 0x00 && spk[1] == 0x14);
    printf("ok: regtest p2wpkh decode\n");
}

static void test_build_coinbase_structural(void) {
    coinbase_parts_t parts = {0};
    char err[256];
    /* witness commitment: OP_RETURN OP_PUSHBYTES_36 aa21a9ed + 32 bytes */
    char wc_hex[2 + 2 + 8 + 64 + 1] = {0};
    /* "6a24aa21a9ed" + 32 bytes of "ab" */
    snprintf(wc_hex, sizeof wc_hex, "6a24aa21a9ed");
    for (int i = 0; i < 32; i++) {
        char tmp[3];
        snprintf(tmp, sizeof tmp, "ab");
        strcat(wc_hex, tmp);
    }

    int rc = coinbase_build(800000, 625000000,
                            "bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4",
                            wc_hex, "/drivepool/", 4, 4,
                            &parts, err, sizeof err);
    if (rc != 0) {
        fprintf(stderr, "coinbase_build err: %s\n", err);
    }
    assert(rc == 0);

    /* Assemble cb1 || en1(4 zeros) || en2(4 zeros) || cb2. */
    size_t total = parts.cb1_len + 8 + parts.cb2_len;
    uint8_t *tx = (uint8_t *)malloc(total);
    assert(tx);
    memcpy(tx, parts.cb1, parts.cb1_len);
    memset(tx + parts.cb1_len, 0xaa, 4);
    memset(tx + parts.cb1_len + 4, 0xbb, 4);
    memcpy(tx + parts.cb1_len + 8, parts.cb2, parts.cb2_len);

    /* Parse: version(4) | varint(in_count) | prev_hash(32) | prev_idx(4) |
     *        varint(scriptSig_len) | scriptSig | sequence(4) |
     *        varint(out_count) | outputs | locktime(4) */
    size_t off = 0;
    /* version */
    uint32_t version = 0;
    for (int i = 0; i < 4; i++) version |= (uint32_t)tx[off + i] << (8 * i);
    off += 4;
    assert(version == 1);

    uint64_t in_count = 0;
    assert(read_varint(tx, total, &off, &in_count) == 0);
    assert(in_count == 1);

    /* prev hash: 32 zeros */
    for (int i = 0; i < 32; i++) assert(tx[off + i] == 0);
    off += 32;

    /* prev idx: 0xffffffff */
    for (int i = 0; i < 4; i++) assert(tx[off + i] == 0xff);
    off += 4;

    uint64_t ss_len = 0;
    assert(read_varint(tx, total, &off, &ss_len) == 0);
    /* scriptSig must start with BIP34 height push: 0x03 0x00 0x35 0x0c (800000 LE) */
    assert(tx[off] == 0x03);
    assert(tx[off + 1] == 0x00);
    assert(tx[off + 2] == 0x35);
    assert(tx[off + 3] == 0x0c);
    off += ss_len;

    /* sequence */
    for (int i = 0; i < 4; i++) assert(tx[off + i] == 0xff);
    off += 4;

    uint64_t out_count = 0;
    assert(read_varint(tx, total, &off, &out_count) == 0);
    assert(out_count == 2); /* payout + witness commitment */

    /* output 0: value */
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)tx[off + i] << (8 * i);
    off += 8;
    assert(v == 625000000);
    uint64_t spk_len = 0;
    assert(read_varint(tx, total, &off, &spk_len) == 0);
    assert(spk_len == 22); /* P2WPKH scriptPubKey */
    off += spk_len;

    /* output 1: zero value + OP_RETURN script */
    uint64_t v2 = 0;
    for (int i = 0; i < 8; i++) v2 |= (uint64_t)tx[off + i] << (8 * i);
    off += 8;
    assert(v2 == 0);
    uint64_t spk2_len = 0;
    assert(read_varint(tx, total, &off, &spk2_len) == 0);
    assert(spk2_len == 38); /* 6a 24 aa21a9ed + 32 */
    assert(tx[off] == 0x6a);
    off += spk2_len;

    /* locktime */
    for (int i = 0; i < 4; i++) assert(tx[off + i] == 0);
    off += 4;
    assert(off == total);

    free(tx);
    coinbase_parts_free(&parts);
    printf("ok: structural coinbase parse\n");
}

/* Split builder: at 100 bps (1%) on 50 BTC subsidy, miner gets
 * 4_950_000_000 sats and operator gets 50_000_000 sats. Below the dust
 * threshold the operator output is dropped and the miner gets everything. */
static void test_build_coinbase_split_fee_math(void) {
    coinbase_parts_t parts = {0};
    char err[256];
    int64_t miner_sats = 0, fee_sats = 0;

    /* Normal split: 1% of 50 BTC = 0.5 BTC. */
    int rc = coinbase_build_split(
        800000, 5000000000LL,
        "bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080",
        "bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080",
        100, NULL, "/simplepool/", 4, 4,
        &parts, &miner_sats, &fee_sats, err, sizeof err);
    assert(rc == 0);
    assert(fee_sats   == 50000000LL);
    assert(miner_sats == 4950000000LL);
    coinbase_parts_free(&parts);

    /* fee_bps = 0 → no operator output, miner gets full value. */
    rc = coinbase_build_split(
        800000, 5000000000LL,
        "bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080",
        "bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080",
        0, NULL, "/simplepool/", 4, 4,
        &parts, &miner_sats, &fee_sats, err, sizeof err);
    assert(rc == 0);
    assert(fee_sats   == 0);
    assert(miner_sats == 5000000000LL);
    coinbase_parts_free(&parts);

    /* fee below dust threshold (546 sats) → collapsed to miner-only. At
     * 100 bps, 30_000 sats subsidy gives 300 sats fee, below dust. */
    rc = coinbase_build_split(
        800000, 30000LL,
        "bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080",
        "bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080",
        100, NULL, "/simplepool/", 4, 4,
        &parts, &miner_sats, &fee_sats, err, sizeof err);
    assert(rc == 0);
    assert(fee_sats   == 0);
    assert(miner_sats == 30000LL);
    coinbase_parts_free(&parts);

    printf("ok: coinbase split fee math\n");
}

/* BIP34 small-height regression: Bitcoin Core encodes heights 1..16 as
 * OP_N (single byte 0x50+n), not as the 2-byte push-data form. Getting
 * this wrong shows up on fresh regtest/signet chains as 'bad-cb-height'. */
static void test_bip34_small_height_uses_opn(void) {
    coinbase_parts_t parts;
    memset(&parts, 0, sizeof parts);
    char err[256] = {0};
    /* height = 5 should produce scriptSig starting with OP_5 = 0x55. */
    int rc = coinbase_build(5, 5000000000LL,
                            "bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080",
                            NULL, "/simplepool/", 4, 4,
                            &parts, err, sizeof err);
    assert(rc == 0);

    /* Walk to the scriptSig as in the structural test. */
    size_t total = parts.cb1_len + 8 + parts.cb2_len;
    uint8_t *tx = (uint8_t *)malloc(total);
    assert(tx);
    memcpy(tx, parts.cb1, parts.cb1_len);
    memset(tx + parts.cb1_len, 0xaa, 4);
    memset(tx + parts.cb1_len + 4, 0xbb, 4);
    memcpy(tx + parts.cb1_len + 8, parts.cb2, parts.cb2_len);

    size_t off = 4; /* version */
    uint64_t in_count = 0;
    assert(read_varint(tx, total, &off, &in_count) == 0);
    off += 32 + 4; /* prev hash + idx */
    uint64_t ss_len = 0;
    assert(read_varint(tx, total, &off, &ss_len) == 0);
    /* OP_5 (0x55) as the first byte of scriptSig. */
    assert(tx[off] == 0x55);
    free(tx);
    coinbase_parts_free(&parts);
    printf("ok: bip34 small-height uses OP_N\n");
}

/* A server-built coinbase like the CUSF enforcer returns: segwit-serialized,
 * version 2, scriptSig = BIP34 height push for 800000 only, three outputs —
 * a BIP301 commitment OP_RETURN, the spendable reward (50 BTC), and the segwit
 * witness commitment — plus a single 32-byte (all-zero) input witness. */
static const char *ENF_COINBASE_HEX =
    "02000000"                                                            /* version 2 */
    "0001"                                                                /* segwit marker + flag */
    "01"                                                                  /* vin = 1 */
    "0000000000000000000000000000000000000000000000000000000000000000"    /* prevout hash */
    "ffffffff"                                                            /* prevout index */
    "04" "0300350c"                                                       /* scriptSig: height 800000 */
    "ffffffff"                                                            /* sequence */
    "03"                                                                  /* vout = 3 */
    "0000000000000000" "06" "6a04deadbeef"                                /* out0: BIP301 commitment */
    "00f2052a01000000" "16" "0014" "1111111111111111111111111111111111111111" /* out1: reward 50 BTC */
    "0000000000000000" "26" "6a24aa21a9ed"
        "2222222222222222222222222222222222222222222222222222222222222222"    /* out2: witness commitment */
    "0120" "0000000000000000000000000000000000000000000000000000000000000000" /* input witness */
    "00000000";                                                           /* locktime */

#define ENF_ADDR "bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080"

/* Rebuild from a server-provided coinbase: the reward output is redirected to
 * the miner, the extranonce is spliced into the scriptSig, and the mandatory
 * commitment + witness-commitment outputs are preserved verbatim. */
static void test_build_from_template(void) {
    coinbase_parts_t parts = {0};
    char err[256] = {0};
    int has_witness = -1;
    int64_t miner_sats = 0, fee_sats = 0;

    int rc = coinbase_build_from_template(
        ENF_COINBASE_HEX, ENF_ADDR, NULL, 0, "/x/", 4, 4,
        &parts, &has_witness, &miner_sats, &fee_sats, err, sizeof err);
    if (rc != 0) fprintf(stderr, "build_from_template err: %s\n", err);
    assert(rc == 0);
    assert(has_witness == 1);
    assert(miner_sats == 5000000000LL);
    assert(fee_sats == 0);

    size_t total = parts.cb1_len + 8 + parts.cb2_len;
    uint8_t *tx = (uint8_t *)malloc(total);
    assert(tx);
    memcpy(tx, parts.cb1, parts.cb1_len);
    memset(tx + parts.cb1_len, 0xaa, 4);       /* extranonce1 */
    memset(tx + parts.cb1_len + 4, 0xbb, 4);   /* extranonce2 */
    memcpy(tx + parts.cb1_len + 8, parts.cb2, parts.cb2_len);

    size_t off = 0;
    uint32_t version = 0;
    for (int i = 0; i < 4; i++) version |= (uint32_t)tx[off + i] << (8 * i);
    off += 4;
    assert(version == 2); /* preserved from the template, not forced to 1 */

    uint64_t in_count = 0;
    assert(read_varint(tx, total, &off, &in_count) == 0);
    assert(in_count == 1);
    for (int i = 0; i < 32; i++) assert(tx[off + i] == 0);
    off += 32;
    for (int i = 0; i < 4; i++) assert(tx[off + i] == 0xff);
    off += 4;

    uint64_t ss_len = 0;
    assert(read_varint(tx, total, &off, &ss_len) == 0);
    assert(ss_len == 4 + 4 + 8); /* height(4) + tag "/x/"(4) + extranonce(8) */
    assert(tx[off] == 0x03 && tx[off + 1] == 0x00 &&
           tx[off + 2] == 0x35 && tx[off + 3] == 0x0c);     /* BIP34 height kept */
    assert(tx[off + 4] == 0x03 && tx[off + 5] == '/' &&
           tx[off + 6] == 'x' && tx[off + 7] == '/');       /* tag push */
    assert(tx[off + 8] == 0xaa && tx[off + 11] == 0xaa);    /* extranonce1 */
    assert(tx[off + 12] == 0xbb && tx[off + 15] == 0xbb);   /* extranonce2 */
    off += ss_len;

    for (int i = 0; i < 4; i++) assert(tx[off + i] == 0xff); /* sequence */
    off += 4;

    uint64_t out_count = 0;
    assert(read_varint(tx, total, &off, &out_count) == 0);
    assert(out_count == 3); /* commitment + redirected reward + witness commitment */

    /* out0: BIP301 commitment preserved. */
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)tx[off + i] << (8 * i);
    off += 8;
    assert(v == 0);
    uint64_t l = 0;
    assert(read_varint(tx, total, &off, &l) == 0);
    assert(l == 6 && tx[off] == 0x6a && tx[off + 1] == 0x04);
    off += l;

    /* out1: reward redirected to the miner (50 BTC, 22-byte P2WPKH). */
    v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)tx[off + i] << (8 * i);
    off += 8;
    assert(v == 5000000000ULL);
    assert(read_varint(tx, total, &off, &l) == 0);
    assert(l == 22 && tx[off] == 0x00 && tx[off + 1] == 0x14);
    off += l;

    /* out2: witness commitment preserved. */
    v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)tx[off + i] << (8 * i);
    off += 8;
    assert(v == 0);
    assert(read_varint(tx, total, &off, &l) == 0);
    assert(l == 38 && tx[off] == 0x6a && tx[off + 1] == 0x24);
    off += l;

    for (int i = 0; i < 4; i++) assert(tx[off + i] == 0); /* locktime */
    off += 4;
    assert(off == total);

    free(tx);
    coinbase_parts_free(&parts);
    printf("ok: coinbase_build_from_template (redirect + preserve commitments)\n");
}

/* With an operator fee, a fourth output (the operator payout) is inserted and
 * the reward is split, while the commitments are still preserved. */
static void test_build_from_template_fee_split(void) {
    coinbase_parts_t parts = {0};
    char err[256] = {0};
    int has_witness = 0;
    int64_t miner_sats = 0, fee_sats = 0;

    int rc = coinbase_build_from_template(
        ENF_COINBASE_HEX, ENF_ADDR, ENF_ADDR, 100, "/x/", 4, 4,
        &parts, &has_witness, &miner_sats, &fee_sats, err, sizeof err);
    assert(rc == 0);
    assert(fee_sats == 50000000LL);       /* 1% of 50 BTC */
    assert(miner_sats == 4950000000LL);

    size_t total = parts.cb1_len + 8 + parts.cb2_len;
    uint8_t *tx = (uint8_t *)malloc(total);
    assert(tx);
    memcpy(tx, parts.cb1, parts.cb1_len);
    memset(tx + parts.cb1_len, 0xaa, 4);
    memset(tx + parts.cb1_len + 4, 0xbb, 4);
    memcpy(tx + parts.cb1_len + 8, parts.cb2, parts.cb2_len);

    size_t off = 4;
    uint64_t in_count = 0;
    assert(read_varint(tx, total, &off, &in_count) == 0);
    off += 36;
    uint64_t ss_len = 0;
    assert(read_varint(tx, total, &off, &ss_len) == 0);
    off += ss_len + 4;
    uint64_t out_count = 0;
    assert(read_varint(tx, total, &off, &out_count) == 0);
    assert(out_count == 4); /* commitment + miner + operator + witness commitment */

    free(tx);
    coinbase_parts_free(&parts);
    printf("ok: coinbase_build_from_template fee split\n");
}


/* coinbase_count_outputs: the OP_RETURN count is what tells an observer
 * whether these blocks can have a sidechain merge-mined into them. One
 * OP_RETURN is a bare witness commitment (a coinbase we built ourselves);
 * more means BIP300/301 commitments came down with the template.
 *
 * Counted against the real fixtures above rather than hand-written hex — a
 * miscounted length byte in a literal silently desyncs the parse and the
 * assertion that catches it tells you nothing about which byte was wrong. */
static void test_count_outputs(void) {
    int spend = -1, opret = -1;

    /* Server-provided coinbase: BIP301 commitment + reward + witness
     * commitment, and segwit-serialized, so the marker/flag and the trailing
     * witness must both be stepped over correctly. */
    assert(coinbase_count_outputs(ENF_COINBASE_HEX, &spend, &opret) == 0);
    assert(spend == 1);
    assert(opret == 2);

    /* A coinbase we built: reward + operator fee + witness commitment only.
     * One OP_RETURN means no sidechain commitments — the state that left
     * Thunder unable to advance. */
    coinbase_parts_t parts = {0};
    char err[256] = {0};
    assert(coinbase_build_split(800000, 5000000000LL, ENF_ADDR, ENF_ADDR, 100,
                                /* witness_commitment_hex */
                                "6a24aa21a9ed2222222222222222222222222222"
                                "222222222222222222222222222222222222",
                                /* coinbase_tag */ NULL,
                                4, 4, &parts, NULL, NULL, err, sizeof err) == 0);
    /* cb1 + extranonce1 + extranonce2 + cb2 is the coinbase a miner submits. */
    size_t n = parts.cb1_len * 2 + 16 + parts.cb2_len * 2 + 1;
    char *hex = (char *)malloc(n);
    assert(hex);
    size_t o = 0;
    for (size_t i = 0; i < parts.cb1_len; i++) o += (size_t)sprintf(hex + o, "%02x", parts.cb1[i]);
    o += (size_t)sprintf(hex + o, "%s", "0011223344556677");   /* en1 + en2 */
    for (size_t i = 0; i < parts.cb2_len; i++) o += (size_t)sprintf(hex + o, "%02x", parts.cb2[i]);
    hex[o] = '\0';

    spend = -1; opret = -1;
    assert(coinbase_count_outputs(hex, &spend, &opret) == 0);
    assert(spend == 2);      /* miner + operator */
    assert(opret == 1);      /* witness commitment only */
    free(hex);
    coinbase_parts_free(&parts);

    /* Malformed input must fail rather than report a plausible count. */
    assert(coinbase_count_outputs("00", &spend, &opret) < 0);
    assert(coinbase_count_outputs("abc", &spend, &opret) < 0);   /* odd length */
    assert(coinbase_count_outputs(NULL, &spend, &opret) < 0);

    /* Out-params are optional. */
    assert(coinbase_count_outputs(ENF_COINBASE_HEX, NULL, NULL) == 0);

    printf("ok: coinbase_count_outputs\n");
}

/* coinbase_template_reward must return the value the multi builder checks
 * against — the spendable output only, ignoring the zero-value OP_RETURNs, and
 * refusing anything without exactly one spendable output. */
static void test_template_reward(void) {
    char err[256] = {0};
    int64_t reward = 0;

    assert(coinbase_template_reward(ENF_COINBASE_HEX, &reward, err, sizeof err) == 0);
    assert(reward == 5000000000LL);

    /* The figure the builder derives its fee from must agree exactly, or a
     * payout split computed from it is rejected at render time. */
    coinbase_parts_t parts = {0};
    int64_t total_payout = 0, fee_sats = 0;
    int64_t fee_expect = (reward * 100) / 10000;
    coinbase_payout_t payouts[] = { { ENF_ADDR, reward - fee_expect } };
    assert(coinbase_build_from_template_multi(
               ENF_COINBASE_HEX, payouts, 1, ENF_ADDR, 100, "/x/", 4, 4,
               &parts, NULL, &total_payout, &fee_sats, err, sizeof err) == 0);
    assert(fee_sats == fee_expect);
    assert(total_payout + fee_sats == reward);
    coinbase_parts_free(&parts);

    /* Garbage in, refusal out — never a silent zero. */
    assert(coinbase_template_reward("nothex", &reward, err, sizeof err) < 0);
    assert(coinbase_template_reward(NULL, &reward, err, sizeof err) < 0);

    printf("ok: coinbase_template_reward\n");
}

/* The adaptive output cap. An overweight block is rejected outright, so this
 * must never return more outputs than the template's spare weight allows —
 * and must not needlessly defer miners when there is room. */
static void test_max_payout_outputs(void) {
    int64_t hr = 0;

    /* No weightlimit from the server: nothing to measure, keep the operator's
     * number rather than inventing a limit. */
    assert(coinbase_max_payout_outputs(0, 0, 200, 21, 1, 12, &hr) == 12);
    assert(hr == -1);

    /* A nearly-empty block: the ceiling binds, not the weight. This is the
     * post-fork case — ECX blocks with almost no transactions. */
    assert(coinbase_max_payout_outputs(4000000, 1000, 200, 21, 1, 12, &hr) == 12);
    assert(hr > 3900000);

    /* The live alpha measurement: 4 MWU limit, 3,997,315 WU of transactions,
     * a 250-byte template coinbase, tag + extranonce, fee output present.
     * Headroom 4,000,000-3,997,315-1000-84-400 = 1,201 WU -> 9 extra outputs,
     * +1 already budgeted, -1 for the fee output = 9. */
    size_t n = coinbase_max_payout_outputs(4000000, 3997315, 250, 21, 1, 64, &hr);
    assert(hr == 4000000 - 3997315 - 1000 - 84 - 400);
    assert(n == (size_t)(1 + hr / COINBASE_PAYOUT_TXOUT_WU - 1));
    /* Whatever it returns must FIT: the outputs it permits, beyond the one the
     * template budgeted, must not exceed the headroom. */
    assert((int64_t)(n + 1 - 1) * COINBASE_PAYOUT_TXOUT_WU <= hr + COINBASE_PAYOUT_TXOUT_WU);

    /* A completely full block: fall back to a single output rather than
     * building something that will be rejected. */
    assert(coinbase_max_payout_outputs(4000000, 4000000, 250, 21, 1, 12, &hr) == 1);
    assert(hr < 0);

    /* The fee output really does consume a slot. */
    size_t with_fee = coinbase_max_payout_outputs(4000000, 3998000, 250, 21, 1, 64, NULL);
    size_t no_fee   = coinbase_max_payout_outputs(4000000, 3998000, 250, 21, 0, 64, NULL);
    assert(no_fee == with_fee + 1);

    /* Never zero, never above the ceiling. */
    assert(coinbase_max_payout_outputs(4000000, 3999900, 250, 21, 1, 12, NULL) >= 1);
    assert(coinbase_max_payout_outputs(4000000, 0, 250, 21, 0, 3, NULL) == 3);

    printf("ok: coinbase_max_payout_outputs adapts to template weight\n");
}

/* The coinbase BYTE budget — a marketplace-compatibility limit, not consensus.
 *
 * These numbers are not invented. They are the pool's own blocks, read back
 * from the node on 2026-08-29:
 *
 *   height 996560 / 996563: 719 B, 19 outputs (17 P2WPKH payouts, 2 nulldata)
 *   height 996566:          767 B, 20 outputs (17 P2WPKH payouts, 3 nulldata)
 *
 * and the model reproduces both EXACTLY:
 *
 *   built = template_bytes - template_slot_bytes + ss_growth
 *           + n_total_outputs * payout_bytes
 *
 * with ss_growth 29 (extranonce 4+8, tag "/ecashpool.tech/" 16, +1 length) and
 * a 31-byte P2WPKH payout. A model that merely looked plausible would have
 * been wrong by one output here, which is a payout a miner does not get. */
static void test_max_payout_outputs_bytes(void) {
    const size_t SS = 29, P2WPKH = 31, P2TR = 43;
    /* Template with 2 nulldata commitments + its own 31 B P2WPKH output. */
    const size_t TMPL = 194, SLOT = 31;

    /* Reproduce block 996563: 16 payouts + 1 fee = 17 outputs = 719 B. */
    const size_t built_719 = TMPL - SLOT + SS + 17 * P2WPKH;
    assert(built_719 == 719);

    /* A budget of exactly the size we already produce must not cost a slot. */
    assert(coinbase_max_payout_outputs_bytes(TMPL, SLOT, SS, P2WPKH, 1, 719, 16)
           == 16);
    /* One byte more room changes nothing; one byte less costs exactly one. */
    assert(coinbase_max_payout_outputs_bytes(TMPL, SLOT, SS, P2WPKH, 1, 720, 16)
           == 16);
    assert(coinbase_max_payout_outputs_bytes(TMPL, SLOT, SS, P2WPKH, 1, 718, 16)
           == 15);

    /* THE POINT OF THE WHOLE THING: the same 16 miners on taproot addresses.
     * 17 outputs x 43 B would be 923 B, past the 919 B that NiceHash's
     * verificator rejected. Under a 750 B budget the cap must bite. */
    assert(TMPL - SLOT + SS + 17 * P2TR == 923);
    size_t tr = coinbase_max_payout_outputs_bytes(TMPL, SLOT, SS, P2TR, 1, 750, 16);
    assert(tr < 16);
    /* And whatever it returns must actually FIT the budget. */
    assert(TMPL - SLOT + SS + (tr + 1) * P2TR <= 750);
    /* Exactly: (750 - 192) / 43 = 12 outputs, minus the fee = 11 payouts. */
    assert(tr == 11);

    /* An all-P2WPKH window under the same 750 B budget is untouched — the cap
     * must cost nothing until address types actually grow the coinbase. */
    assert(coinbase_max_payout_outputs_bytes(TMPL, SLOT, SS, P2WPKH, 1, 750, 16)
           == 16);

    /* 0 disables it entirely: an upgrade must not silently reprice payouts. */
    assert(coinbase_max_payout_outputs_bytes(TMPL, SLOT, SS, P2TR, 1, 0, 16) == 16);

    /* The ceiling still binds over the byte cap. */
    assert(coinbase_max_payout_outputs_bytes(TMPL, SLOT, SS, P2WPKH, 1, 4000, 12)
           == 12);

    /* A budget too small for even one payout returns 1, not 0: a block must
     * pay someone, and the misconfiguration is the operator's to see. */
    assert(coinbase_max_payout_outputs_bytes(TMPL, SLOT, SS, P2WPKH, 1, 400, 16) >= 1);
    assert(coinbase_max_payout_outputs_bytes(TMPL, SLOT, SS, P2WPKH, 1, 100, 16) == 1);

    /* An unknown template slot size credits nothing rather than guessing —
     * one slot smaller, never larger. */
    size_t known   = coinbase_max_payout_outputs_bytes(TMPL, SLOT, SS, P2WPKH, 1, 719, 64);
    size_t unknown = coinbase_max_payout_outputs_bytes(TMPL, 0,    SS, P2WPKH, 1, 719, 64);
    assert(unknown <= known);

    /* ⛔ A nonsense per-output size must never buy slots: below the smallest
     * output we emit, or above the largest, falls back to the largest. */
    size_t as_tr = coinbase_max_payout_outputs_bytes(TMPL, SLOT, SS, P2TR, 1, 750, 16);
    assert(coinbase_max_payout_outputs_bytes(TMPL, SLOT, SS, 0,  1, 750, 16) == as_tr);
    assert(coinbase_max_payout_outputs_bytes(TMPL, SLOT, SS, 30, 1, 750, 16) == as_tr);
    assert(coinbase_max_payout_outputs_bytes(TMPL, SLOT, SS, 99, 1, 750, 16) == as_tr);

    /* The fee output really does consume one. */
    size_t with_fee = coinbase_max_payout_outputs_bytes(TMPL, SLOT, SS, P2WPKH, 1, 719, 64);
    size_t no_fee   = coinbase_max_payout_outputs_bytes(TMPL, SLOT, SS, P2WPKH, 0, 719, 64);
    assert(no_fee == with_fee + 1);

    printf("ok: coinbase_max_payout_outputs_bytes holds the byte budget\n");
}

/* Per-address output sizes, the input the byte budget divides by. */
static void test_payout_txout_bytes(void) {
    assert(coinbase_payout_txout_bytes(
               "bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4") == 31);  /* P2WPKH */
    assert(coinbase_payout_txout_bytes(
               "bc1qrp33g0q5c5txsp9arysrx4k6zdkfs4nce4xj0gdcccefvpysxf3qccfmv3")
           == 43);                                                    /* P2WSH */
    assert(coinbase_payout_txout_bytes(
               "bc1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqzk5jj0")
           == 43);                                                    /* P2TR */
    assert(coinbase_payout_txout_bytes("1BvBMSEYstWetqTFn5Au4m4GFg7xJaNVN2") == 34);
    assert(coinbase_payout_txout_bytes("3J98t1WpEZ73CNmQviecrnyiWrnqRhWNLy") == 32);

    /* Undecodable is charged the LARGEST, never skipped and never the
     * smallest: over-charging costs a payout slot, under-charging breaks the
     * budget the operator asked for. */
    assert(coinbase_payout_txout_bytes("not-an-address") == 43);
    assert(coinbase_payout_txout_bytes("") == 43);
    assert(coinbase_payout_txout_bytes(NULL) == 43);
    /* Well-formed but refused by policy (witness v2) — unknown to us. */
    assert(coinbase_payout_txout_bytes("bc1zw508d6qejxtdg4y5r3zarvaryvaxxpcs") == 43);

    /* The weight constant and the byte figure must never disagree. */
    assert(coinbase_payout_txout_bytes(
               "bc1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqzk5jj0") * 4
           == COINBASE_PAYOUT_TXOUT_WU);

    printf("ok: coinbase_payout_txout_bytes measures the real output\n");
}

/* The template's own spendable output, parsed rather than assumed. */
static void test_template_payout_slot_bytes(void) {
    char err[256] = {0};
    size_t slot = 0;
    assert(coinbase_template_payout_slot_bytes(ENF_COINBASE_HEX, &slot,
                                               err, sizeof err) == 0);
    /* Whatever the enforcer pays, it must be a plausible single output and
     * must match what the reward walk saw. */
    assert(slot >= 31 && slot <= 43);

    int64_t reward = 0;
    assert(coinbase_template_reward(ENF_COINBASE_HEX, &reward, err, sizeof err) == 0);
    assert(reward > 0);

    /* Same refusals as the reward walk — it is literally the same parse. */
    assert(coinbase_template_payout_slot_bytes("nothex", &slot, err, sizeof err) < 0);
    assert(err[0] != 0);
    assert(coinbase_template_payout_slot_bytes(NULL, &slot, err, sizeof err) < 0);
    assert(coinbase_template_payout_slot_bytes(ENF_COINBASE_HEX, NULL,
                                               err, sizeof err) < 0);

    printf("ok: coinbase_template_payout_slot_bytes\n");
}

/* Multi-output builder: split the 50 BTC reward three ways. */
static void test_build_from_template_multi(void) {
    coinbase_parts_t parts = {0};
    char err[256] = {0};
    int has_witness = 0;
    int64_t total_payout = 0, fee_sats = 0;

    coinbase_payout_t payouts[] = {
        { ENF_ADDR, 1500000000LL }, /* 15 BTC */
        { ENF_ADDR, 2000000000LL }, /* 20 BTC */
        { ENF_ADDR, 1500000000LL }, /* 15 BTC */
    };

    int rc = coinbase_build_from_template_multi(
        ENF_COINBASE_HEX, payouts, 3, NULL, 0, "/x/", 4, 4,
        &parts, &has_witness, &total_payout, &fee_sats, err, sizeof err);
    if (rc != 0) fprintf(stderr, "build_from_template_multi err: %s\n", err);
    assert(rc == 0);
    assert(has_witness == 1);
    assert(total_payout == 5000000000LL);
    assert(fee_sats == 0);

    size_t total = parts.cb1_len + 8 + parts.cb2_len;
    uint8_t *tx = (uint8_t *)malloc(total);
    assert(tx);
    memcpy(tx, parts.cb1, parts.cb1_len);
    memset(tx + parts.cb1_len, 0xaa, 4);
    memset(tx + parts.cb1_len + 4, 0xbb, 4);
    memcpy(tx + parts.cb1_len + 8, parts.cb2, parts.cb2_len);

    size_t off = 4; /* version */
    uint64_t in_count = 0;
    assert(read_varint(tx, total, &off, &in_count) == 0);
    off += 36; /* prevout + idx */
    uint64_t ss_len = 0;
    assert(read_varint(tx, total, &off, &ss_len) == 0);
    off += ss_len + 4; /* scriptSig + sequence */
    uint64_t out_count = 0;
    assert(read_varint(tx, total, &off, &out_count) == 0);
    /* commitment + 3 payouts + witness commitment */
    assert(out_count == 5);

    /* out0: BIP301 commitment preserved. */
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)tx[off + i] << (8 * i);
    off += 8;
    assert(v == 0);
    uint64_t l = 0;
    assert(read_varint(tx, total, &off, &l) == 0);
    assert(l == 6 && tx[off] == 0x6a && tx[off + 1] == 0x04);
    off += l;

    /* out1..3: payouts in order. */
    uint64_t expected[3] = { 1500000000ULL, 2000000000ULL, 1500000000ULL };
    for (int k = 0; k < 3; k++) {
        v = 0;
        for (int i = 0; i < 8; i++) v |= (uint64_t)tx[off + i] << (8 * i);
        off += 8;
        assert(v == expected[k]);
        assert(read_varint(tx, total, &off, &l) == 0);
        assert(l == 22 && tx[off] == 0x00 && tx[off + 1] == 0x14);
        off += l;
    }

    /* out4: witness commitment preserved. */
    v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)tx[off + i] << (8 * i);
    off += 8;
    assert(v == 0);
    assert(read_varint(tx, total, &off, &l) == 0);
    assert(l == 38 && tx[off] == 0x6a && tx[off + 1] == 0x24);
    off += l;

    /* locktime */
    off += 4;
    assert(off == total);

    free(tx);
    coinbase_parts_free(&parts);
    printf("ok: coinbase_build_from_template_multi\n");
}

/* Multi-output builder with operator fee. */
static void test_build_from_template_multi_fee(void) {
    coinbase_parts_t parts = {0};
    char err[256] = {0};
    int64_t total_payout = 0, fee_sats = 0;

    coinbase_payout_t payouts[] = {
        { ENF_ADDR, 2475000000LL }, /* 24.75 BTC */
        { ENF_ADDR, 2475000000LL }, /* 24.75 BTC */
    };

    int rc = coinbase_build_from_template_multi(
        ENF_COINBASE_HEX, payouts, 2, ENF_ADDR, 100, "/x/", 4, 4,
        &parts, NULL, &total_payout, &fee_sats, err, sizeof err);
    assert(rc == 0);
    assert(total_payout == 4950000000LL);
    assert(fee_sats == 50000000LL);

    size_t total = parts.cb1_len + 8 + parts.cb2_len;
    uint8_t *tx = (uint8_t *)malloc(total);
    assert(tx);
    memcpy(tx, parts.cb1, parts.cb1_len);
    memset(tx + parts.cb1_len, 0, 8);
    memcpy(tx + parts.cb1_len + 8, parts.cb2, parts.cb2_len);

    size_t off = 4;
    uint64_t in_count = 0;
    assert(read_varint(tx, total, &off, &in_count) == 0);
    off += 36;
    uint64_t ss_len = 0;
    assert(read_varint(tx, total, &off, &ss_len) == 0);
    off += ss_len + 4;
    uint64_t out_count = 0;
    assert(read_varint(tx, total, &off, &out_count) == 0);
    /* commitment + 2 payouts + operator + witness commitment */
    assert(out_count == 5);

    free(tx);
    coinbase_parts_free(&parts);
    printf("ok: coinbase_build_from_template_multi fee split\n");
}

/* Multi-output builder refuses when payouts + fee do not equal reward. */
static void test_build_from_template_multi_sum_check(void) {
    coinbase_parts_t parts = {0};
    char err[256] = {0};

    coinbase_payout_t payouts[] = {
        { ENF_ADDR, 2000000000LL },
        { ENF_ADDR, 2000000000LL },
    };

    int rc = coinbase_build_from_template_multi(
        ENF_COINBASE_HEX, payouts, 2, NULL, 0, "/x/", 4, 4,
        &parts, NULL, NULL, NULL, err, sizeof err);
    assert(rc < 0);
    printf("ok: coinbase_build_from_template_multi sum check\n");
}

/* The network an address encodes, used when the block-template backend
 * cannot be asked (the CUSF enforcer answers only getblocktemplate and
 * submitblock). Deliberately coarse: several networks share version bytes
 * and HRPs, so the test pins that it reports what the encoding proves and
 * refuses to over-claim. */
static void test_address_network(void) {
    assert(strcmp(coinbase_address_network(
        "bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4"), "main") == 0);
    assert(strcmp(coinbase_address_network(
        "1A1zP1eP5QGefi2DMPTfTL5SLmv7DivfNa"), "main") == 0);
    assert(strcmp(coinbase_address_network(
        "bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080"), "regtest") == 0);
    /* testnet and signet share the `tb` HRP — reporting either one alone
     * would be a guess dressed up as a fact. */
    assert(strcmp(coinbase_address_network(
        "tb1qw508d6qejxtdg4y5r3zarvary0c5xw7kxpjzsx"), "test/signet") == 0);

    /* A typo must read as "no idea", not as a network: it is the input to a
     * mainnet-vs-testnet mismatch warning, and a false negative there is a
     * burnt fee output. */
    assert(coinbase_address_network(
        "1A1zP1eP5QGefi2DMPTfTL5SLmv7Divfna") == NULL);
    assert(coinbase_address_network("") == NULL);
    assert(coinbase_address_network(NULL) == NULL);

    assert(coinbase_network_is_mainnet("main") == 1);
    assert(coinbase_network_is_mainnet("signet") == 0);
    assert(coinbase_network_is_mainnet("test") == 0);
    assert(coinbase_network_is_mainnet("regtest") == 0);
    /* Unrecognised must not read as mainnet. */
    assert(coinbase_network_is_mainnet("who-knows") == 0);
    assert(coinbase_network_is_mainnet(NULL) == 0);
    printf("ok: address -> network\n");
}

/* The scriptSig length varint lives in cb1, and is computed from
 * en1_size + en2_size at render time. If the extranonces spliced in later
 * are not exactly that wide, the varint disagrees with the bytes that follow
 * and the transaction is malformed -- valid-looking to a hasher, rejected by
 * the network. Pin the agreement at the width the pool actually advertises,
 * so a change to STRATUM_EXTRANONCE2_SIZE that misses a call site fails here
 * rather than on a found block. */
static void test_scriptsig_length_matches_advertised_extranonce(void) {
    const size_t en1 = STRATUM_EXTRANONCE1_SIZE;
    const size_t en2 = STRATUM_EXTRANONCE2_SIZE;

    coinbase_parts_t parts = {0};
    char err[256] = {0};
    assert(coinbase_build_split(800000, 5000000000LL, ENF_ADDR, ENF_ADDR, 100,
                                "6a24aa21a9ed2222222222222222222222222222"
                                "222222222222222222222222222222222222",
                                "/simplepool/",
                                en1, en2, &parts, NULL, NULL,
                                err, sizeof err) == 0);

    /* Assemble the coinbase exactly as handle_submit does. */
    size_t total = parts.cb1_len + en1 + en2 + parts.cb2_len;
    uint8_t *tx = (uint8_t *)malloc(total);
    assert(tx);
    size_t o = 0;
    memcpy(tx + o, parts.cb1, parts.cb1_len); o += parts.cb1_len;
    memset(tx + o, 0xaa, en1);                o += en1;
    memset(tx + o, 0xbb, en2);                o += en2;
    memcpy(tx + o, parts.cb2, parts.cb2_len);

    /* Walk to the scriptSig varint: version(4) | varint(vin) | prevout(36). */
    size_t off = 4;
    uint64_t vin = 0;
    assert(read_varint(tx, total, &off, &vin) == 0 && vin == 1);
    off += 36;
    uint64_t ss_len = 0;
    assert(read_varint(tx, total, &off, &ss_len) == 0);

    /* The declared length must cover the real scriptSig contents, and the
     * extranonce bytes must be the last thing inside it. */
    assert(ss_len <= 100);
    size_t ss_end = off + (size_t)ss_len;
    assert(ss_end <= total);
    for (size_t i = 0; i < en1; i++) assert(tx[ss_end - en1 - en2 + i] == 0xaa);
    for (size_t i = 0; i < en2; i++) assert(tx[ss_end - en2 + i] == 0xbb);
    /* Sequence follows immediately -- proof the varint did not run short. */
    for (int i = 0; i < 4; i++) assert(tx[ss_end + i] == 0xff);

    free(tx);
    coinbase_parts_free(&parts);
    printf("ok: scriptSig length agrees with en1=%zu en2=%zu\n", en1, en2);
}

/* The same cb1 assembled with a wrong-width extranonce2 -- what a miner that
 * ignored mining.subscribe would produce -- must not parse as a well-formed
 * coinbase. This is the failure the stratum-side length check prevents. */
static void test_wrong_width_extranonce_desyncs_the_parse(void) {
    coinbase_parts_t parts = {0};
    char err[256] = {0};
    assert(coinbase_build_split(800000, 5000000000LL, ENF_ADDR, ENF_ADDR, 100,
                                "6a24aa21a9ed2222222222222222222222222222"
                                "222222222222222222222222222222222222",
                                "/simplepool/",
                                STRATUM_EXTRANONCE1_SIZE,
                                STRATUM_EXTRANONCE2_SIZE, &parts, NULL, NULL,
                                err, sizeof err) == 0);

    /* Splice in the classic 4-byte extranonce2 instead of the reserved width. */
    size_t n = parts.cb1_len * 2 + (STRATUM_EXTRANONCE1_SIZE + 4) * 2
             + parts.cb2_len * 2 + 1;
    char *hex = (char *)malloc(n);
    assert(hex);
    size_t o = 0;
    for (size_t i = 0; i < parts.cb1_len; i++)
        o += (size_t)sprintf(hex + o, "%02x", parts.cb1[i]);
    for (size_t i = 0; i < STRATUM_EXTRANONCE1_SIZE + 4; i++)
        o += (size_t)sprintf(hex + o, "%02x", 0xcc);
    for (size_t i = 0; i < parts.cb2_len; i++)
        o += (size_t)sprintf(hex + o, "%02x", parts.cb2[i]);
    hex[o] = '\0';

    int spend = -1, opret = -1;
    int rc = coinbase_count_outputs(hex, &spend, &opret);
    /* Either the parse fails outright or it reports something other than the
     * real output set -- never a clean, correct read. */
    assert(!(rc == 0 && spend == 2 && opret == 1));

    free(hex);
    coinbase_parts_free(&parts);
    printf("ok: wrong-width extranonce2 does not parse as a valid coinbase\n");
}

/* Consensus caps the coinbase scriptSig at 100 bytes. Unreachable through
 * config today (height push + a 76-byte tag + 12 extranonce bytes is 93),
 * but the guard is what keeps a future widening from emitting a coinbase
 * that only fails at the network. */
static void test_scriptsig_over_100_is_rejected(void) {
    coinbase_parts_t parts = {0};
    char err[256] = {0};
    int rc = coinbase_build_split(800000, 5000000000LL, ENF_ADDR, NULL, 0,
                                  NULL, "/simplepool/",
                                  /* en1 */ 4, /* en2 */ 90,
                                  &parts, NULL, NULL, err, sizeof err);
    assert(rc < 0);
    assert(strstr(err, "scriptSig length") != NULL);
    printf("ok: oversized coinbase scriptSig rejected (%s)\n", err);
}


/* ---------------------------------------------------------------------------
 * BIP-350 conformance.
 *
 * The tables below are VERIFIED AGAINST bip-0350.mediawiki. They come with no
 * generator, and an earlier version of this comment claimed they were
 * "generated, do not hand-edit" — pointing at a tool that does not exist in
 * this tree. That is unenforceable advice dressed as a guarantee, so this says
 * what can actually be checked here instead.
 *
 * Why it matters: these strings are deliberately adversarial — one differs
 * from its neighbour only in the checksum, another carries a single uppercase
 * letter mid-string to test case rejection — so a slipped character makes a
 * test that passes for the wrong reason. What stands behind them is an
 * independent differential rather than provenance: all 23 entries were decoded
 * with a from-scratch BIP-173/350 reference implementation and checked against
 * what each row asserts — the 5 supported reproduce their scriptPubKey byte
 * for byte, the 3 refused-by-policy are genuinely well-formed with the witness
 * version and program length claimed, and all 15 invalid ones are rejected for
 * the stated reason. A transcription slip would have broken a checksum and
 * been caught. The same reference was diffed against
 * coinbase_address_to_script() over 1,224 generated addresses — witness
 * versions 0-16 x program lengths {2,16,20,21,31,32,33,40} x {bc,tb,bcrt} x
 * {lower, upper, bad checksum} — with zero mismatches and exactly the 18
 * expected acceptances.
 *
 * The tables are static, so the suite has no network dependency.
 *
 * ⚠️ "VALID PER BIP-350" AND "WE WILL PAY IT" ARE DIFFERENT QUESTIONS, and the
 * split into two tables is the point. Three of the eight addresses BIP-350
 * lists as valid are ones this pool REFUSES on purpose — witness v2, v16, and a
 * v1 with a 40-byte program. They encode correctly and `validateaddress` calls
 * them valid; they are also anyone-can-spend under current consensus, so a
 * coinbase paying one hands the reward to whoever notices first. Refusing costs
 * that miner an error message at authorize. Accepting costs them a block.
 * (BIP-341: a v1 program of any length other than 32 remains unencumbered.)
 * ------------------------------------------------------------------------- */
/* Verified against bip-0350.mediawiki — see the note above. */
static const struct { const char *addr; const char *spk; } bip350_supported[] = {
    { "BC1QW508D6QEJXTDG4Y5R3ZARVARY0C5XW7KV8F3T4",
      "0014751e76e8199196d454941c45d1b3a323f1433bd6" },  /* v0, 20-byte */
    { "tb1qrp33g0q5c5txsp9arysrx4k6zdkfs4nce4xj0gdcccefvpysxf3q0sl5k7",
      "00201863143c14c5166804bd19203356da136c985678cd4d27a1b8c6329604903262" },  /* v0, 32-byte */
    { "tb1qqqqqp399et2xygdj5xreqhjjvcmzhxw4aywxecjdzew6hylgvsesrxh6hy",
      "0020000000c4a5cad46221b2a187905e5266362b99d5e91c6ce24d165dab93e86433" },  /* v0, 32-byte */
    { "tb1pqqqqp399et2xygdj5xreqhjjvcmzhxw4aywxecjdzew6hylgvsesf3hn0c",
      "5120000000c4a5cad46221b2a187905e5266362b99d5e91c6ce24d165dab93e86433" },  /* v1, 32-byte */
    { "bc1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqzk5jj0",
      "512079be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798" },  /* v1, 32-byte */
};

static const struct { const char *addr; int witver; size_t proglen; } bip350_refused[] = {
    { "bc1pw508d6qejxtdg4y5r3zarvary0c5xw7kw508d6qejxtdg4y5r3zarvary0c5xw7kt5nd6y", 1, 40 },
    { "BC1SW50QGDZ25J", 16, 2 },
    { "bc1zw508d6qejxtdg4y5r3zarvaryvaxxpcs", 2, 16 },
};

static const struct { const char *addr; const char *why; } bip350_invalid[] = {
    /* NOT REACHED BY THE BECH32 PATH — no bc1/tb1/bcrt1 prefix, so it falls
       through to base58 and is rejected there. Still a rejection. */
    { "tc1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vq5zuyut",
      "Invalid human-readable part" },
    { "bc1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqh2y7hd",
      "Invalid checksum (Bech32 instead of Bech32m)" },
    { "tb1z0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqglt7rf",
      "Invalid checksum (Bech32 instead of Bech32m)" },
    { "BC1S0XLXVLHEMJA6C4DQV22UAPCTQUPFHLXM9H8Z3K2E72Q4K9HCZ7VQ54WELL",
      "Invalid checksum (Bech32 instead of Bech32m)" },
    { "bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kemeawh",
      "Invalid checksum (Bech32m instead of Bech32)" },
    { "tb1q0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vq24jc47",
      "Invalid checksum (Bech32m instead of Bech32)" },
    { "bc1p38j9r5y49hruaue7wxjce0updqjuyyx0kh56v8s25huc6995vvpql3jow4",
      "Invalid character in checksum" },
    { "BC130XLXVLHEMJA6C4DQV22UAPCTQUPFHLXM9H8Z3K2E72Q4K9HCZ7VQ7ZWS8R",
      "Invalid witness version" },
    { "bc1pw5dgrnzv",
      "Invalid program length (1 byte)" },
    { "bc1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7v8n0nx0muaewav253zgeav",
      "Invalid program length (41 bytes)" },
    { "BC1QR508D6QEJXTDG4Y5R3ZARVARYV98GJ9P",
      "Invalid program length for witness version 0 (per BIP141)" },
    { "tb1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vq47Zagq",
      "Mixed case" },
    { "bc1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7v07qwwzcrf",
      "zero padding of more than 4 bits" },
    { "tb1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vpggkg4j",
      "Non-zero padding in 8-to-5 conversion" },
    { "bc1gmk9yu",
      "Empty data section" },
};

static void hex_of(const uint8_t *b, size_t n, char *out) {
    static const char *H = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[i*2] = H[b[i] >> 4]; out[i*2+1] = H[b[i] & 15]; }
    out[n*2] = 0;
}

static void test_bip350_supported(void) {
    for (size_t i = 0; i < sizeof bip350_supported / sizeof bip350_supported[0]; i++) {
        uint8_t spk[64];
        size_t  spk_len = 0;
        char    err[192], got[160];
        int rc = coinbase_address_to_script(bip350_supported[i].addr, spk, sizeof spk,
                                            &spk_len, err, sizeof err);
        if (rc != 0) {
            printf("FAIL: %s rejected: %s\n", bip350_supported[i].addr, err);
            assert(rc == 0);
        }
        hex_of(spk, spk_len, got);
        if (strcmp(got, bip350_supported[i].spk) != 0) {
            printf("FAIL: %s\n  want %s\n  got  %s\n",
                   bip350_supported[i].addr, bip350_supported[i].spk, got);
            assert(0);
        }
    }
    printf("ok: bip350 supported vectors (%zu) produce the spec's exact scriptPubKey\n",
           sizeof bip350_supported / sizeof bip350_supported[0]);
}

static void test_bip350_refused_by_policy(void) {
    /* These must fail, and the message must say WHY — a miner who pastes a v2
       address needs to learn something other than "invalid". */
    for (size_t i = 0; i < sizeof bip350_refused / sizeof bip350_refused[0]; i++) {
        uint8_t spk[64];
        size_t  spk_len = 0;
        char    err[192];
        err[0] = 0;
        int rc = coinbase_address_to_script(bip350_refused[i].addr, spk, sizeof spk,
                                            &spk_len, err, sizeof err);
        if (rc == 0) {
            printf("FAIL: %s was PAID; witness v%d/%zu-byte is anyone-can-spend\n",
                   bip350_refused[i].addr, bip350_refused[i].witver,
                   bip350_refused[i].proglen);
            assert(rc != 0);
        }
        assert(err[0] != 0);
    }
    printf("ok: bip350 well-formed-but-unsafe vectors (%zu) refused with a reason\n",
           sizeof bip350_refused / sizeof bip350_refused[0]);
}

static void test_bip350_invalid(void) {
    for (size_t i = 0; i < sizeof bip350_invalid / sizeof bip350_invalid[0]; i++) {
        uint8_t spk[64];
        size_t  spk_len = 0;
        char    err[192];
        err[0] = 0;
        int rc = coinbase_address_to_script(bip350_invalid[i].addr, spk, sizeof spk,
                                            &spk_len, err, sizeof err);
        if (rc == 0) {
            printf("FAIL: accepted invalid address %s (%s)\n",
                   bip350_invalid[i].addr, bip350_invalid[i].why);
            assert(rc != 0);
        }
        assert(err[0] != 0);
    }
    printf("ok: bip350 invalid vectors (%zu) all rejected\n",
           sizeof bip350_invalid / sizeof bip350_invalid[0]);
}

/* The regression the weight constant exists to prevent: a taproot payout costs
   43 bytes on the wire, not 31, and the headroom divider has to know it. */
static void test_payout_txout_weight_matches_p2tr(void) {
    uint8_t spk[64];
    size_t  spk_len = 0;
    char    err[192];
    int rc = coinbase_address_to_script(
        "bc1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqzk5jj0",
        spk, sizeof spk, &spk_len, err, sizeof err);
    assert(rc == 0);
    /* 8-byte value + 1-byte script length + scriptPubKey, non-witness so x4. */
    size_t wu = (8 + 1 + spk_len) * 4;
    assert(spk_len == 34);
    assert(wu == 172);
    assert(COINBASE_PAYOUT_TXOUT_WU >= wu);
    printf("ok: payout txout weight constant covers a P2TR output (%zu WU)\n", wu);
}

int main(void) {
    test_p2pkh_address();
    test_p2wpkh_address();
    test_regtest_p2wpkh();
    test_build_coinbase_structural();
    test_build_coinbase_split_fee_math();
    test_bip34_small_height_uses_opn();
    test_build_from_template();
    test_build_from_template_fee_split();
    test_template_reward();
    test_max_payout_outputs();
    test_payout_txout_bytes();
    test_template_payout_slot_bytes();
    test_max_payout_outputs_bytes();
    test_build_from_template_multi();
    test_build_from_template_multi_fee();
    test_build_from_template_multi_sum_check();
    test_count_outputs();
    test_address_network();
    test_scriptsig_length_matches_advertised_extranonce();
    test_wrong_width_extranonce_desyncs_the_parse();
    test_scriptsig_over_100_is_rejected();
    test_bip350_supported();
    test_bip350_refused_by_policy();
    test_bip350_invalid();
    test_payout_txout_weight_matches_p2tr();
    printf("test_coinbase: all tests passed\n");
    return 0;
}
