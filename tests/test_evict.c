/* test_evict.c — the tail-eviction module (src/evict.c).
 *
 * Vectors were produced by an independent Python implementation of BIP141
 * (sha256d, Bitcoin merkle with odd-level duplication, zero reserved value)
 * on 2026-09-19. The same Python reproduced the enforcer's LIVE
 * default_witness_commitment for a 3,893-tx betanet template at height
 * 969,700 byte-for-byte, which is what makes these constants trustworthy.
 * The coinbase hex below IS that template's coinbasetxn (181 bytes, one
 * P2TR spendable output of 318,107,917 sats + the commitment OP_RETURN). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "evict.h"
#include "coinbase.h"

static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static const char *LIVE_CB =
"020000000001010000000000000000000000000000000000000000000000000000000000000000ffffffff0403e4cb0effffffff02"
"0df1f512000000002251203a55383afa0f75dcb4993bdd6edbd513d157a2c96b6077621d14f74a3fbf5b6a"
"0000000000000000266a24aa21a9ede853e3cca3e7ed426b45c7ba55f86d908a6bade34d7b3d81a6580f5915e1a1cf"
"0120000000000000000000000000000000000000000000000000000000000000000000000000";
static const char *C5 = "16b975e692a9c33ebcb1a5867e410414aa0059d10e39709ed5fa4f493d4387d2";
static const char *C4 = "d04eb37af7711e0e42abdd23f10c9ffed796721970945f5275945d03653ca008";
static const char *C3 = "ab683e86094729475c542e2f99bde55c99961ac27f888366e7130f51ad917ae4";
static const char *C0 = "e2f61c3f71d1defd3fa999dfa36953755c690689799962b48bebd836974e8cf9";

static void hex32(const uint8_t b[32], char out[65]) {
    static const char d[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { out[2*i] = d[b[i] >> 4]; out[2*i+1] = d[b[i] & 15]; }
    out[64] = 0;
}

/* tx i = byte (i+1) repeated 10+i+1 times, i.e. bytes([i])*(10+i) for i=1..5
 * in the Python that made the vectors. weight/fee are made up but distinct. */
static bitcoind_template_t *mk(size_t n, const char *dwc, int64_t weight_limit) {
    bitcoind_template_t *t = (bitcoind_template_t *)calloc(1, sizeof *t);
    t->height = 969700;
    t->weight_limit = weight_limit;
    t->coinbasetxn_hex = strdup(LIVE_CB);
    t->coinbase_value_sats = 318107917;
    if (dwc) {
        t->default_witness_commitment = (char *)malloc(77);
        snprintf(t->default_witness_commitment, 77, "6a24aa21a9ed%s", dwc);
        /* The enforcer embeds the same commitment in the coinbase's OP_RETURN;
         * the gate requires the two to agree, so the fixture writes it there too. */
        char *e = strstr(t->coinbasetxn_hex, "6a24aa21a9ed"); memcpy(e + 12, dwc, 64);
    }
    t->txs = (bitcoind_template_tx_t *)calloc(n ? n : 1, sizeof *t->txs);
    t->tx_count = n;
    for (size_t i = 0; i < n; i++) {
        size_t len = 10 + (i + 1);
        char *h = (char *)malloc(2 * len + 1);
        for (size_t j = 0; j < len; j++) snprintf(h + 2*j, 3, "%02x", (unsigned)(i + 1));
        t->txs[i].data_hex = h;
        t->txs[i].weight = 1000 * (int)(i + 1);     /* 1000,2000,3000,4000,5000 */
        t->txs[i].fee    = 100 * (int64_t)(i + 1);  /* 100..500 */
    }
    return t;
}
static void fr(bitcoind_template_t *t) {
    for (size_t i = 0; i < t->tx_count; i++) free(t->txs[i].data_hex);
    free(t->txs); free(t->coinbasetxn_hex); free(t->default_witness_commitment); free(t);
}

static void test_commitment_vectors(void) {
    uint8_t c[32]; char h[65]; char err[128];
    bitcoind_template_t *t = mk(5, NULL, 4000000);
    CHECK(evict_witness_commitment(t, c, err, sizeof err) == 0); hex32(c, h); CHECK(strcmp(h, C5) == 0);
    t->tx_count = 4; CHECK(evict_witness_commitment(t, c, err, sizeof err) == 0); hex32(c, h); CHECK(strcmp(h, C4) == 0);
    t->tx_count = 3; CHECK(evict_witness_commitment(t, c, err, sizeof err) == 0); hex32(c, h); CHECK(strcmp(h, C3) == 0);
    t->tx_count = 0; CHECK(evict_witness_commitment(t, c, err, sizeof err) == 0); hex32(c, h); CHECK(strcmp(h, C0) == 0);
    t->tx_count = 5; fr(t);
}

static void test_reward_adjust(void) {
    char err[128];
    char *cb = strdup(LIVE_CB);
    CHECK(evict_adjust_template_reward(cb, -700, err, sizeof err) == 0);
    /* 318107917 - 700 = 318107217 = 0x12F5EE51 -> LE bytes 51 ee f5 12 00 00 00 00 at the same offset */
    CHECK(strstr(cb, "51eef5120000000022512") != NULL);
    CHECK(strstr(cb, "0df1f5120000000022512") == NULL);
    CHECK(strlen(cb) == strlen(LIVE_CB));
    /* Never below zero. */
    CHECK(evict_adjust_template_reward(cb, -400000000, err, sizeof err) < 0);
    /* A coinbase with two spendable outputs is refused. */
    char *two = strdup(LIVE_CB);
    /* turn the OP_RETURN output into a spendable one by changing 6a -> 51 (OP_1) at its spk start */
    char *p = strstr(two, "266a24aa21a9ed"); CHECK(p != NULL); if (p) { p[2] = '5'; p[3] = '1'; }
    CHECK(evict_adjust_template_reward(two, -1, err, sizeof err) < 0);
    free(two); free(cb);
}

/* need_wu such that 5 txs (15,000 WU) do not fit but 3 (6,000 WU) do:
 * limit 20,000, need 9,500 -> budget 10,500 -> drop tx5 (5,000) -> 10,000 fits. */
static void test_evict_drops_tail_and_recomputes(void) {
    char err[128] = {0}; int64_t wu = -1, fee = -1;
    bitcoind_template_t *t = mk(5, C5, 20000);
    int n = evict_tail_for_coinbase(t, 9500, &wu, &fee, err, sizeof err);
    CHECK(n == 1);
    CHECK(t->tx_count == 4);
    CHECK(wu == 5000 && fee == 500);
    CHECK(strcmp(t->default_witness_commitment + 12, C4) == 0);
    CHECK(strncmp(t->default_witness_commitment, "6a24aa21a9ed", 12) == 0);
    /* ⛔ The copy that reaches a block is the one INSIDE the coinbase hex
     * (coinbase.c copies the OP_RETURN verbatim). Round-1 review of 3708a1a:
     * this was rewritten in the field only, and every evicted block would
     * have failed bad-witness-merkle-match. */
    { char *e = strstr(t->coinbasetxn_hex, "6a24aa21a9ed"); CHECK(e != NULL);
      CHECK(e && strncmp(e + 12, C4, 64) == 0);
      CHECK(strstr(t->coinbasetxn_hex, C5) == NULL); }
    CHECK(t->coinbase_value_sats == 318107917 - 500);
    /* 318107917-500 = 318107417 = 0x12F5EF19 -> 19 ef f5 12 */
    CHECK(strstr(t->coinbasetxn_hex, "19eff5120000000022512") != NULL);
    /* The dropped entry was freed and the survivors kept their bytes. */
    CHECK(t->txs[4].data_hex == NULL);
    CHECK(t->txs[3].data_hex != NULL && strncmp(t->txs[3].data_hex, "04040404", 8) == 0);
    /* Already fits -> 0, untouched. */
    CHECK(evict_tail_for_coinbase(t, 9500, &wu, &fee, err, sizeof err) == 0);
    CHECK(t->tx_count == 4 && wu == 0 && fee == 0);
    /* Two more: need 16,500 -> budget 3,500 -> keep tx1+tx2 (3,000). */
    n = evict_tail_for_coinbase(t, 16500, &wu, &fee, err, sizeof err);
    CHECK(n == 2 && t->tx_count == 2 && wu == 7000 && fee == 700);
    fr(t);
}

/* Exact-fit boundaries (mutant `>` -> `>=` in the drop loop survived the
 * first version of this file). limit 20,000: 5 txs = 15,000 WU. */
static void test_exact_fit_boundaries(void) {
    char err[128] = {0}; int64_t wu = -1, fee = -1;
    /* need 5,000 -> budget 15,000 == tx weight: must evict NOTHING. */
    bitcoind_template_t *t = mk(5, C5, 20000);
    CHECK(evict_tail_for_coinbase(t, 5000, &wu, &fee, err, sizeof err) == 0);
    CHECK(t->tx_count == 5 && wu == 0);
    CHECK(strstr(t->coinbasetxn_hex, C5) != NULL);
    /* need 10,000 -> budget 10,000: dropping tx5 lands EXACTLY on the budget -> 1 evicted, not 2. */
    CHECK(evict_tail_for_coinbase(t, 10000, &wu, &fee, err, sizeof err) == 1);
    CHECK(t->tx_count == 4 && wu == 5000);
    fr(t);
}

static void test_gate_needs_embedded_commitment(void) {
    char err[128] = {0}; int64_t wu = -1, fee = -1;
    /* Field says C5 (correct) but the coinbase embeds something else: refuse. */
    bitcoind_template_t *t = mk(5, C5, 20000);
    char *e = strstr(t->coinbasetxn_hex, "6a24aa21a9ed"); memcpy(e + 12, C4, 64);
    CHECK(evict_tail_for_coinbase(t, 9500, &wu, &fee, err, sizeof err) == -1);
    CHECK(strstr(err, "embedded") != NULL);
    CHECK(t->tx_count == 5);
    fr(t);
    /* No commitment output in the coinbase at all: refuse. */
    t = mk(5, C5, 20000);
    e = strstr(t->coinbasetxn_hex, "6a24aa21a9ed"); memcpy(e, "6a24bb21a9ed", 12);
    CHECK(evict_tail_for_coinbase(t, 9500, &wu, &fee, err, sizeof err) == -1);
    CHECK(t->tx_count == 5);
    fr(t);
}

static void test_safety_gate_refuses(void) {
    char err[128] = {0}; int64_t wu = -1, fee = -1;
    /* Enforcer's commitment says C4 but the template holds 5 txs: our
     * arithmetic does not reproduce theirs -> refuse, touch nothing. */
    bitcoind_template_t *t = mk(5, C4, 20000);
    CHECK(evict_tail_for_coinbase(t, 9500, &wu, &fee, err, sizeof err) == -1);
    CHECK(t->tx_count == 5 && wu == 0 && fee == 0);
    CHECK(strstr(err, "self-check FAILED") != NULL);
    CHECK(t->coinbase_value_sats == 318107917);
    fr(t);
    /* Malformed commitment field -> refuse. */
    t = mk(5, NULL, 20000); t->default_witness_commitment = strdup("6a24deadbeef");
    CHECK(evict_tail_for_coinbase(t, 9500, &wu, &fee, err, sizeof err) == -1);
    CHECK(t->tx_count == 5);
    fr(t);
    /* No weight limit -> refuse (the pool then assumes nothing about headroom). */
    t = mk(5, C5, 0);
    CHECK(evict_tail_for_coinbase(t, 9500, &wu, &fee, err, sizeof err) == -1);
    fr(t);
    /* Coinbase alone larger than the block -> refuse rather than evict everything. */
    t = mk(5, C5, 20000);
    CHECK(evict_tail_for_coinbase(t, 20001, &wu, &fee, err, sizeof err) == -1);
    CHECK(t->tx_count == 5);
    fr(t);
}

/* The contract the whole module rests on: coinbase.c copies the template's
 * OP_RETURN commitment output VERBATIM into what it renders, so the commitment
 * the block carries is the one INSIDE coinbasetxn_hex. A future builder that
 * sourced it elsewhere (e.g. job->wc_hex) would break eviction with every
 * other test still green — this one would not stay green. */
static void test_builder_emits_the_new_commitment(void) {
    char err[256] = {0}; int64_t wu = 0, fee = 0;
    bitcoind_template_t *t = mk(5, C5, 20000);
    CHECK(evict_tail_for_coinbase(t, 9500, &wu, &fee, err, sizeof err) == 1);
    int64_t reward = 0;
    CHECK(coinbase_template_reward(t->coinbasetxn_hex, &reward, err, sizeof err) == 0);
    CHECK(reward == 318107917 - 500);
    /* one payout taking the whole reward-after-fee; the builder checks the sum */
    const char *op = "bc1p8f2nswh6pa6aedye80wkak74z0g40gkfdds8wcsaznm550altd4qptz0dk";
    int64_t fee_sats = reward * 50 / 10000;
    coinbase_payout_t p = { "bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4", reward - fee_sats };
    coinbase_parts_t parts = {0}; int has_wit = 0;
    CHECK(coinbase_build_from_template_multi(t->coinbasetxn_hex, &p, 1, op, 50, "/ecpool.tech/",
                                             4, 8, &parts, &has_wit, NULL, NULL, err, sizeof err) == 0);
    CHECK(has_wit == 1);
    /* find the 38-byte OP_RETURN in cb2 and compare its 32 bytes with the field */
    uint8_t want[32]; for (int i = 0; i < 32; i++) { unsigned b; sscanf(t->default_witness_commitment + 12 + 2*i, "%2x", &b); want[i] = (uint8_t)b; }
    int found = 0, old_found = 0;
    uint8_t old[32]; for (int i = 0; i < 32; i++) { unsigned b; sscanf(C5 + 2*i, "%2x", &b); old[i] = (uint8_t)b; }
    for (size_t i = 0; i + 38 <= parts.cb2_len; i++) {
        if (memcmp(parts.cb2 + i, "\x6a\x24\xaa\x21\xa9\xed", 6) == 0) {
            if (memcmp(parts.cb2 + i + 6, want, 32) == 0) found++;
            if (memcmp(parts.cb2 + i + 6, old, 32) == 0) old_found++;
        }
    }
    CHECK(found == 1);
    CHECK(old_found == 0);
    free(parts.cb1); free(parts.cb2);
    fr(t);
}

int main(void) {
    test_commitment_vectors();
    test_reward_adjust();
    test_evict_drops_tail_and_recomputes();
    test_safety_gate_refuses();
    test_exact_fit_boundaries();
    test_gate_needs_embedded_commitment();
    test_builder_emits_the_new_commitment();
    if (fails) { fprintf(stderr, "test_evict: %d of %d checks FAILED\n", fails, checks); return 1; }
    printf("test_evict: %d checks passed\n", checks);
    return 0;
}
