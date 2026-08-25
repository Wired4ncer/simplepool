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
    test_build_from_template_multi();
    test_build_from_template_multi_fee();
    test_build_from_template_multi_sum_check();
    test_count_outputs();
    test_address_network();
    test_scriptsig_length_matches_advertised_extranonce();
    test_wrong_width_extranonce_desyncs_the_parse();
    test_scriptsig_over_100_is_rejected();
    printf("test_coinbase: all tests passed\n");
    return 0;
}
