#include "../src/share.h"
#include "../src/sha256.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static int hex2nib(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

static void hex_decode(const char *hex, uint8_t *out, size_t outlen) {
    for (size_t i = 0; i < outlen; ++i) {
        out[i] = (uint8_t)((hex2nib(hex[2 * i]) << 4) | hex2nib(hex[2 * i + 1]));
    }
}

static int hex_eq(const uint8_t *bytes, size_t n, const char *hex) {
    for (size_t i = 0; i < n; ++i) {
        int hi = hex2nib(hex[2 * i]);
        int lo = hex2nib(hex[2 * i + 1]);
        if (((hi << 4) | lo) != bytes[i]) return 0;
    }
    return 1;
}

static void test_dsha256(void) {
    /* dsha256("hello") = 9595c9df90075148eb06860365df33584b75bff782a510c6cd4883a419833d50 */
    uint8_t out[32];
    dsha256((const uint8_t *)"hello", 5, out);
    CHECK(hex_eq(out, 32,
        "9595c9df90075148eb06860365df33584b75bff782a510c6cd4883a419833d50"));
}

static void test_nbits(void) {
    uint8_t t[32];
    nbits_to_target(0x1d00ffffu, t);
    CHECK(hex_eq(t, 32,
        "00000000ffff0000000000000000000000000000000000000000000000000000"));

    /* 0x1b0404cb -> bytes 04 04 cb at offset 5 */
    nbits_to_target(0x1b0404cbu, t);
    CHECK(t[5] == 0x04);
    CHECK(t[6] == 0x04);
    CHECK(t[7] == 0xcb);
    /* Everything else zero */
    for (int i = 0; i < 32; ++i) {
        if (i == 5 || i == 6 || i == 7) continue;
        CHECK(t[i] == 0);
    }
}

static void test_worker_diff(void) {
    uint8_t t1[32], t2[32];
    worker_diff_to_target(1.0, t1);
    /* diff-1 standard target */
    CHECK(hex_eq(t1, 32,
        "00000000ffff0000000000000000000000000000000000000000000000000000"));
    /* diff 2 strictly smaller */
    worker_diff_to_target(2.0, t2);
    CHECK(be32_cmp(t2, t1) < 0);
    /* invalid -> max */
    worker_diff_to_target(0.0, t2);
    for (int i = 0; i < 32; ++i) CHECK(t2[i] == 0xff);

    /* A difficulty small enough to overflow the 128-bit conversion saturates
     * to the easiest target, and does so IDENTICALLY at every optimisation
     * level. The old clamp (`scaled = 2^128 - 1.0`, a no-op in double) left the
     * conversion undefined: 1e-12 gave an all-zero target at -O1 — no share can
     * ever meet it, so every miner is rejected — and an all-ff target at -O2.
     * Anything below ~2.3e-10 is in that range, and initial_diff / vardiff_min
     * are operator-set with no floor.
     *
     * Note this is also why the "tiny diff so any hash passes" idiom used in
     * tests/test_stratum.c only worked at -O2: it was relying on the UB. */
    worker_diff_to_target(1e-12, t2);
    for (int i = 0; i < 32; ++i) CHECK(t2[i] == 0xff);
    worker_diff_to_target(1e-30, t2);
    for (int i = 0; i < 32; ++i) CHECK(t2[i] == 0xff);

    /* Just above the overflow threshold the result is still a real target:
     * neither saturated nor zero, and still easier than diff 1. */
    uint8_t t3[32];
    worker_diff_to_target(1e-9, t3);
    CHECK(be32_cmp(t3, t1) > 0);           /* easier than diff 1 */
    int all_ff = 1, all_00 = 1;
    for (int i = 0; i < 32; ++i) {
        if (t3[i] != 0xff) all_ff = 0;
        if (t3[i] != 0x00) all_00 = 0;
    }
    CHECK(!all_ff && !all_00);
}

static void test_merkle(void) {
    uint8_t a[32], b[32], buf[64], expected[32], root[32];
    memset(a, 1, 32);
    memset(b, 2, 32);
    memcpy(buf, a, 32);
    memcpy(buf + 32, b, 32);
    dsha256(buf, 64, expected);
    uint8_t branches[1][32];
    memcpy(branches[0], b, 32);
    merkle_root_from_branches(a, (const uint8_t(*)[32])branches, 1, root);
    CHECK(memcmp(root, expected, 32) == 0);

    /* No branches -> root == leaf */
    merkle_root_from_branches(a, NULL, 0, root);
    CHECK(memcmp(root, a, 32) == 0);
}

static void test_be_cmp(void) {
    uint8_t a[32] = {0}, b[32] = {0};
    CHECK(be32_cmp(a, b) == 0);
    b[31] = 1;
    CHECK(be32_cmp(a, b) < 0);
    CHECK(be32_cmp(b, a) > 0);
    a[0] = 0xff;
    CHECK(be32_cmp(a, b) > 0);
}

static void test_genesis(void) {
    /* Bitcoin genesis block header.
     * version = 1
     * prev_hash = all zeros
     * merkle_root display (BE) = 4a5e1e4baab89f3a32518a88c31bc87f618f76673e2cc77ab2127b7afdeda33b
     * ntime = 0x495fab29
     * nbits = 0x1d00ffff
     * nonce = 0x7c2bac1d
     * hash display (BE) = 000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f
     */
    uint8_t prev[32] = {0};
    uint8_t merkle_be[32], merkle_le[32];
    hex_decode("4a5e1e4baab89f3a32518a88c31bc87f618f76673e2cc77ab2127b7afdeda33b",
               merkle_be, 32);
    /* The "internal" / wire form is reverse of display. */
    for (int i = 0; i < 32; ++i) merkle_le[i] = merkle_be[31 - i];

    uint8_t header[80];
    build_header(1, prev, merkle_le, 0x495fab29u, 0x1d00ffffu, 0x7c2bac1du, header);

    uint8_t hash_be[32];
    hash_header(header, hash_be);
    CHECK(hex_eq(hash_be, 32,
        "000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f"));

    /* And it must be <= the network target for nbits=0x1d00ffff. */
    uint8_t target[32];
    nbits_to_target(0x1d00ffffu, target);
    CHECK(be32_cmp(hash_be, target) < 0);
}

/* The PPS rate must be fair value net of fee, and must track difficulty.
 *
 * Regression guard for the bug where the fee lived only in a hand-computed
 * pps_sats_per_diff: fee_bps had no effect on what miners were credited, and
 * the rate went stale as difficulty moved. */
static void test_pps_rate(void) {
    const int64_t subsidy = 312500000;      /* 3.125 BTC */

    /* gross = 312500000 / 111157.455 = 2811.33...; 1% fee -> 2783.2... */
    double r = pps_rate_from_template(subsidy, 111157.455354832, 100);
    CHECK(r > 2783.0 && r < 2783.5);

    /* fee_bps must actually move the payout. */
    double gross = pps_rate_from_template(subsidy, 111157.455354832, 0);
    CHECK(gross > 2811.0 && gross < 2811.7);
    CHECK(r < gross);
    /* 1% off gross, to within float noise. */
    double implied_fee = 1.0 - r / gross;
    CHECK(implied_fee > 0.0099 && implied_fee < 0.0101);

    /* A 3% fee must pay strictly less than a 1% fee. */
    double r3 = pps_rate_from_template(subsidy, 111157.455354832, 300);
    CHECK(r3 < r);
    CHECK(r3 > 2726.0 && r3 < 2728.0);

    /* Rate tracks difficulty: double the difficulty, halve the rate. This is
     * what a static configured rate could not do. */
    double easy = pps_rate_from_template(subsidy, 100000.0, 100);
    double hard = pps_rate_from_template(subsidy, 200000.0, 100);
    CHECK(hard > 0.0);
    CHECK(easy / hard > 1.999 && easy / hard < 2.001);

    /* Unpriceable templates disable accrual rather than guess. */
    CHECK(pps_rate_from_template(0,       111157.0, 100) == 0.0);
    CHECK(pps_rate_from_template(-1,      111157.0, 100) == 0.0);
    CHECK(pps_rate_from_template(subsidy, 0.0,      100) == 0.0);
    CHECK(pps_rate_from_template(subsidy, -5.0,     100) == 0.0);
    CHECK(pps_rate_from_template(subsidy, HUGE_VAL, 100) == 0.0);
    /* A fee of 100% or more would credit nothing; reject rather than
     * silently pay zero. */
    CHECK(pps_rate_from_template(subsidy, 111157.0, 10000) == 0.0);
    CHECK(pps_rate_from_template(subsidy, 111157.0, -1)    == 0.0);
}

/* A difficulty small enough to overflow the target conversion must saturate,
 * not produce whatever the compiler felt like.
 *
 * DIFF1_TARGET's top 16 bytes are ~7.9e28, so anything below ~2.3e-10 makes
 * the scaled target exceed 2^128. The old clamp tried to pin it to 2^128 - 1,
 * a value no double can hold, so the conversion was undefined — and the two
 * plausible outcomes are opposites: a zero target rejects every share, an
 * all-ones target accepts every share. UBSan flags the conversion; this pins
 * the answer. */
static void test_tiny_difficulty_saturates_the_target(void) {
    uint8_t target[32];
    for (double d = 1e-10; d > 1e-300; d /= 1e10) {
        worker_diff_to_target(d, target);
        int all_ff = 1, all_00 = 1;
        for (int i = 0; i < 32; ++i) {
            if (target[i] != 0xff) all_ff = 0;
            if (target[i] != 0x00) all_00 = 0;
        }
        /* An impossibly small difficulty means an impossibly easy target.
         * Never the reverse. */
        CHECK(!all_00);
        if (d <= 1e-11) CHECK(all_ff);
    }
    /* And the ordinary range is untouched. */
    worker_diff_to_target(1.0, target);
    CHECK(target[0] == 0x00 && target[4] == 0xff && target[5] == 0xff);
}

/* The PPS guards, checked against what the production pps pool actually did.
 *
 * avonpool ran at 40.15 TH/s — 9,349 difficulty/s — on a forknet that started
 * at difficulty 1 and retargeted upward. block_value/difficulty priced every
 * difficulty-1 share at 3.09 BTC, and in under four hours the pool accrued
 * 15,561,471 BTC of liability against 943.60 BTC it had actually mined. */
static void test_pps_min_safe_difficulty(void) {
    /* The boundary is where this pool alone finds one block per interval. */
    CHECK(fabs(pps_min_safe_difficulty(9349.2676, 600) - 5609560.56) < 1.0);
    /* No measurement is not a licence to proceed — callers must read 0 as
     * "unknown", never as "safe". */
    CHECK(pps_min_safe_difficulty(0.0, 600) == 0.0);
    CHECK(pps_min_safe_difficulty(-1.0, 600) == 0.0);
    CHECK(pps_min_safe_difficulty(100.0, 0) == 0.0);
    /* A chain the pool cannot outrun needs no protection. */
    CHECK(pps_min_safe_difficulty(9349.0, 600) < 127479855693691.0);
}

static void test_pps_issuance_ceiling(void) {
    const int64_t VALUE = 312500000;   /* 3.125 BTC, the forknet subsidy */
    const int T = 600;

    /* Difficulty 1, and the pool presenting 117 difficulty/s: fair value says
     * 3.09 BTC a share, the chain can mint 3.125 BTC per 600s. The ceiling
     * has to bring the rate down by roughly the ratio between them. */
    double fair = pps_rate_from_template(VALUE, 1.0, 100);
    double capped = pps_rate_apply_issuance_ceiling(fair, VALUE, 117.0, T);
    CHECK(fair > 309000000.0);
    CHECK(capped < fair / 50000.0);
    /* What the pool would accrue over an hour can no longer exceed what the
     * chain mints in an hour. */
    double accrued_per_hour = capped * 117.0 * 3600.0;
    double issued_per_hour  = (double)VALUE * 3600.0 / (double)T;
    CHECK(accrued_per_hour <= issued_per_hour * 1.001);

    /* On a properly calibrated chain the ceiling is inert: mainnet difficulty
     * against the same pool leaves fair value untouched. */
    double m_fair = pps_rate_from_template(VALUE, 127479855693691.0, 100);
    CHECK(pps_rate_apply_issuance_ceiling(m_fair, VALUE, 9349.0, T) == m_fair);

    /* With no measurement it cannot judge, so it must not silently clamp to
     * zero and stop paying honest miners. */
    CHECK(pps_rate_apply_issuance_ceiling(m_fair, VALUE, 0.0, T) == m_fair);
    CHECK(pps_rate_apply_issuance_ceiling(m_fair, VALUE, 9349.0, 0) == m_fair);
    /* Nothing to cap. */
    CHECK(pps_rate_apply_issuance_ceiling(0.0, VALUE, 117.0, T) == 0.0);
}

/* The concrete regression: replay the difficulty-1 era through the ceiling and
 * check the liability lands near what the chain could actually pay. */
static void test_ceiling_would_have_stopped_the_blowup(void) {
    const int64_t VALUE = 312500000;
    const int T = 600;
    const double SECONDS = 330.0;      /* ~5.5 min at difficulty 1 */
    const double SHARES  = 38509.0;    /* what the pool submitted */
    const double dps = SHARES / SECONDS;

    double fair = pps_rate_from_template(VALUE, 1.0, 100);
    double unguarded_btc = fair * SHARES / 1e8;
    CHECK(unguarded_btc > 100000.0);   /* the actual outcome: millions */

    double capped = pps_rate_apply_issuance_ceiling(fair, VALUE, dps, T);
    double guarded_btc = capped * SHARES / 1e8;
    /* At most what the chain minted in that window, which is 0.55 blocks. */
    double mintable_btc = (double)VALUE / 1e8 * SECONDS / (double)T;
    CHECK(guarded_btc <= mintable_btc * 1.001);
    CHECK(guarded_btc > 0.0);          /* miners still earn something */
}

int main(void) {
    test_pps_min_safe_difficulty();
    test_pps_issuance_ceiling();
    test_ceiling_would_have_stopped_the_blowup();
    test_tiny_difficulty_saturates_the_target();
    test_dsha256();
    test_nbits();
    test_worker_diff();
    test_merkle();
    test_be_cmp();
    test_genesis();
    test_pps_rate();
    printf("test_share: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
