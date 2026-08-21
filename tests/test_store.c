/* Standalone test for src/store.c. Builds with -lsqlite3 -lpthread. */

#include "store.h"
#include "log.h"
#include "pplns.h"
#include "coinbase.h"

#include <sqlite3.h>

#include <math.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static char g_db_paths[16][256];
static int  g_db_count = 0;

static const char *fresh_db_path(void) {
    char *p = g_db_paths[g_db_count++];
    snprintf(p, 256, "/tmp/store_test_%d_%d.db", (int)getpid(), g_db_count);
    unlink(p);
    /* WAL/SHM siblings */
    char wal[300], shm[300];
    snprintf(wal, sizeof(wal), "%s-wal", p);
    snprintf(shm, sizeof(shm), "%s-shm", p);
    unlink(wal);
    unlink(shm);
    return p;
}

static void cleanup_dbs(void) {
    for (int i = 0; i < g_db_count; ++i) {
        unlink(g_db_paths[i]);
        char wal[300], shm[300];
        snprintf(wal, sizeof(wal), "%.255s-wal", g_db_paths[i]);
        snprintf(shm, sizeof(shm), "%.255s-shm", g_db_paths[i]);
        unlink(wal);
        unlink(shm);
    }
}

static int64_t scalar_i64(sqlite3 *db, const char *sql) {
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    assert(rc == SQLITE_OK);
    rc = sqlite3_step(st);
    assert(rc == SQLITE_ROW);
    int64_t v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return v;
}

static double scalar_dbl(sqlite3 *db, const char *sql) {
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    assert(rc == SQLITE_OK);
    rc = sqlite3_step(st);
    assert(rc == SQLITE_ROW);
    double v = sqlite3_column_double(st, 0);
    sqlite3_finalize(st);
    return v;
}

static void test_basic(void) {
    const char *path = fresh_db_path();
    store_cfg_t cfg = {0};
    snprintf(cfg.path, sizeof(cfg.path), "%s", path);
    cfg.commit_window_ms = 20;
    cfg.commit_max_shares = 200;

    store_t *s = NULL;
    int rc = store_open(&cfg, &s);
    assert(rc == 0);
    assert(s != NULL);

    double expected_sum = 0.0;
    for (int i = 0; i < 1000; ++i) {
        char wname[32];
        snprintf(wname, sizeof(wname), "worker%d", i % 10);
        double diff = 1.0 + (double)(i % 7);
        expected_sum += diff;
        rc = store_record_share(s, wname, 1000ULL + (uint64_t)i, diff, 0, NULL);
        assert(rc == 0);
    }

    rc = store_flush(s);
    assert(rc == 0);

    sqlite3 *db = NULL;
    rc = sqlite3_open(path, &db);
    assert(rc == SQLITE_OK);

    int64_t nworkers = scalar_i64(db, "SELECT count(*) FROM workers");
    int64_t nshares  = scalar_i64(db, "SELECT count(*) FROM shares");
    double  sumd     = scalar_dbl(db, "SELECT sum(difficulty) FROM shares");
    assert(nworkers == 10);
    assert(nshares == 1000);
    assert(sumd > expected_sum - 0.001 && sumd < expected_sum + 0.001);

    /* Block path */
    rc = store_record_block(s, 9999, 12345, "abc123hash", "worker3",
                            "bcrt1qexampleaddr", 4950000000LL, 50000000LL);
    assert(rc == 0);
    rc = store_flush(s);
    assert(rc == 0);

    int64_t nblocks = scalar_i64(db, "SELECT count(*) FROM blocks_found");
    assert(nblocks == 1);

    sqlite3_stmt *st = NULL;
    rc = sqlite3_prepare_v2(db,
        "SELECT b.finder_id, w.id, b.finder_address, b.reward_sats, b.fee_sats "
        "FROM blocks_found b JOIN workers w ON w.name='worker3' LIMIT 1",
        -1, &st, NULL);
    assert(rc == SQLITE_OK);
    rc = sqlite3_step(st);
    assert(rc == SQLITE_ROW);
    int64_t finder = sqlite3_column_int64(st, 0);
    int64_t wid    = sqlite3_column_int64(st, 1);
    const unsigned char *addr_txt = sqlite3_column_text(st, 2);
    int64_t reward = sqlite3_column_int64(st, 3);
    int64_t fee    = sqlite3_column_int64(st, 4);
    assert(finder == wid);
    assert(addr_txt && strcmp((const char *)addr_txt, "bcrt1qexampleaddr") == 0);
    assert(reward == 4950000000LL);
    assert(fee == 50000000LL);
    sqlite3_finalize(st);

    sqlite3_close(db);
    store_close(s);
    printf("  ok test_basic\n");
}

static void test_rejects(void) {
    const char *path = fresh_db_path();
    store_cfg_t cfg = {0};
    snprintf(cfg.path, sizeof(cfg.path), "%s", path);
    cfg.commit_window_ms = 20;
    cfg.commit_max_shares = 100;

    store_t *s = NULL;
    int rc = store_open(&cfg, &s);
    assert(rc == 0);

    for (int i = 0; i < 50; ++i) {
        char wname[32];
        snprintf(wname, sizeof(wname), "rw%d", i);
        rc = store_record_reject(s, wname, 1000 + (uint64_t)i, "low-difficulty");
        assert(rc == 0);
    }
    rc = store_flush(s);
    assert(rc == 0);

    sqlite3 *db = NULL;
    rc = sqlite3_open(path, &db);
    assert(rc == SQLITE_OK);
    int64_t n = scalar_i64(db, "SELECT count(*) FROM rejects");
    assert(n == 50);
    sqlite3_close(db);
    store_close(s);
    printf("  ok test_rejects\n");
}

typedef struct {
    store_t *s;
    int      tid;
    int      n;
} thread_arg_t;

static void *thread_fn(void *arg) {
    thread_arg_t *t = (thread_arg_t *)arg;
    for (int i = 0; i < t->n; ++i) {
        char wname[32];
        snprintf(wname, sizeof(wname), "tw%d", t->tid);
        int rc = store_record_share(t->s, wname,
            10000ULL + (uint64_t)i, 2.5, 0, NULL);
        if (rc != 0) {
            /* retry briefly if queue saturated */
            nanosleep(&(struct timespec){.tv_sec = 0, .tv_nsec = 100000}, NULL);
            --i;
        }
    }
    return NULL;
}

static void test_concurrent(void) {
    const char *path = fresh_db_path();
    store_cfg_t cfg = {0};
    snprintf(cfg.path, sizeof(cfg.path), "%s", path);
    cfg.commit_window_ms = 10;
    cfg.commit_max_shares = 500;

    store_t *s = NULL;
    int rc = store_open(&cfg, &s);
    assert(rc == 0);

    pthread_t th[8];
    thread_arg_t args[8];
    for (int i = 0; i < 8; ++i) {
        args[i].s = s; args[i].tid = i; args[i].n = 1000;
        pthread_create(&th[i], NULL, thread_fn, &args[i]);
    }
    for (int i = 0; i < 8; ++i) pthread_join(th[i], NULL);

    rc = store_flush(s);
    assert(rc == 0);

    sqlite3 *db = NULL;
    rc = sqlite3_open(path, &db);
    assert(rc == SQLITE_OK);
    int64_t n = scalar_i64(db, "SELECT count(*) FROM shares");
    assert(n == 8000);
    int64_t nw = scalar_i64(db, "SELECT count(*) FROM workers");
    assert(nw == 8);
    sqlite3_close(db);
    store_close(s);
    printf("  ok test_concurrent (8000 shares across 8 threads)\n");
}

static void test_drop(void) {
    /* Tiny ring; throw way more than can fit. */
    store_test_set_ring_capacity(64);
    const char *path = fresh_db_path();
    store_cfg_t cfg = {0};
    snprintf(cfg.path, sizeof(cfg.path), "%s", path);
    cfg.commit_window_ms = 1000;   /* writer rarely wakes */
    cfg.commit_max_shares = 8;

    store_t *s = NULL;
    int rc = store_open(&cfg, &s);
    assert(rc == 0);

    int dropped_observed = 0;
    for (int i = 0; i < 200000; ++i) {
        rc = store_record_share(s, "ww", 1000, 1.0, 0, NULL);
        if (rc < 0) dropped_observed = 1;
    }
    store_stats_t st;
    store_get_stats(s, &st);
    assert(dropped_observed);
    assert(st.shares_dropped > 0);

    /* Don't bother flushing fully -- just close (which drains). */
    store_close(s);
    store_test_set_ring_capacity(0);
    printf("  ok test_drop (dropped=%llu)\n",
           (unsigned long long)st.shares_dropped);
}

/* credited_sats must be stored per share exactly as passed, and pool_meta
 * must be readable back.
 *
 * The audit sums shares.credited_sats instead of recomputing it from a rate,
 * because the rate is derived per template and moves with difficulty. That
 * only works if the column faithfully records what was credited — including
 * 0, which is what solo mode writes since it never accrues. */
static void test_credited_sats(void) {
    const char *path = fresh_db_path();
    store_cfg_t cfg = {0};
    snprintf(cfg.path, sizeof(cfg.path), "%s", path);
    cfg.commit_window_ms = 20;
    cfg.commit_max_shares = 200;

    store_t *s = NULL;
    assert(store_open(&cfg, &s) == 0);

    /* pps-classic-style: each share carries the sats it was credited and
     * the rate that produced them. difficulty=i at rate 7.0 gives i*7. */
    int64_t expected = 0;
    for (int i = 1; i <= 50; ++i) {
        int64_t credited = (int64_t)i * 7;
        expected += credited;
        assert(store_record_share_addr(s, "payer", "addr1",
                                       1000ULL + (uint64_t)i, (double)i,
                                       0, NULL, credited, 7.0) == 0);
    }
    /* solo-style: no accrual, so the column must record 0 — not be left
     * to a later recompute that would invent a credit. rate_used stays 0
     * too, which is what marks the row as "nothing to verify". */
    for (int i = 0; i < 25; ++i) {
        assert(store_record_share_addr(s, "solo", "addr2",
                                       9000ULL + (uint64_t)i, 3.0,
                                       0, NULL, 0, 0.0) == 0);
    }
    /* The legacy 6-arg helper must still work and store 0. */
    assert(store_record_share(s, "legacy", 9500, 2.0, 0, NULL) == 0);

    assert(store_record_pool_meta(s, "pps-classic", 100, "derived",
                                  2783.22, 2811.33, 100.4,
                                  111157.455, 312500000, 1700000000ULL) == 0);
    assert(store_flush(s) == 0);

    sqlite3 *db = NULL;
    assert(sqlite3_open(path, &db) == SQLITE_OK);

    int64_t total = scalar_i64(db,
        "SELECT sum(credited_sats) FROM shares "
        "WHERE worker_id = (SELECT id FROM workers WHERE name='payer')");
    assert(total == expected);

    int64_t solo = scalar_i64(db,
        "SELECT sum(credited_sats) FROM shares "
        "WHERE worker_id = (SELECT id FROM workers WHERE name='solo')");
    assert(solo == 0);

    int64_t legacy = scalar_i64(db,
        "SELECT sum(credited_sats) FROM shares "
        "WHERE worker_id = (SELECT id FROM workers WHERE name='legacy')");
    assert(legacy == 0);

    /* The audit invariant. Every credited share must re-derive exactly from
     * the pair stored on its own row, with no reference to any current rate.
     * This is the property that makes the ledger checkable rather than
     * merely recorded, so it is asserted as equality, not as a tolerance. */
    assert(scalar_i64(db,
        "SELECT count(*) FROM shares WHERE rate_used > 0 "
        "  AND credited_sats <> CAST(difficulty * rate_used AS INTEGER)") == 0);
    /* Non-accruing rows carry no multiplicand to check against. */
    assert(scalar_i64(db,
        "SELECT count(*) FROM shares WHERE rate_used != 0 "
        "  AND worker_id IN (SELECT id FROM workers "
        "                     WHERE name IN ('solo','legacy'))") == 0);

    /* pool_meta: single row, values round-tripped. */
    assert(scalar_i64(db, "SELECT count(*) FROM pool_meta") == 1);
    assert(scalar_i64(db, "SELECT fee_bps FROM pool_meta") == 100);
    double rate = scalar_dbl(db, "SELECT rate_sats_per_diff FROM pool_meta");
    assert(rate > 2783.0 && rate < 2783.5);

    /* credited_from is stamped once and must survive later updates, so an
     * audit can tell where credited_sats became trustworthy. */
    int64_t from1 = scalar_i64(db, "SELECT credited_from FROM pool_meta");
    assert(from1 == 1700000000);
    assert(store_record_pool_meta(s, "pps-classic", 200, "override",
                                  1000.0, 2811.33, 6443.0,
                                  111157.455, 312500000, 1700009999ULL) == 0);
    int64_t from2 = scalar_i64(db, "SELECT credited_from FROM pool_meta");
    assert(from2 == 1700000000);
    assert(scalar_i64(db, "SELECT fee_bps FROM pool_meta") == 200);

    sqlite3_close(db);
    store_close(s);
    printf("  ok test_credited_sats\n");
}

/* rate_history is the provenance half of the audit: it must append when the
 * rate moves, stay quiet when it doesn't, and hold rows that re-derive from
 * their own inputs. */
static void test_rate_history(void) {
    const char *path = fresh_db_path();

    store_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.path, sizeof(cfg.path), "%s", path);
    cfg.commit_window_ms = 20;
    cfg.commit_max_shares = 200;

    store_t *s = NULL;
    assert(store_open(&cfg, &s) == 0);

    const double  net_diff = 111157.455354832;
    const int64_t value    = 312500000;
    const int     fee_bps  = 100;
    double gross = (double)value / net_diff;
    double rate  = gross * (1.0 - (double)fee_bps / 10000.0);

    assert(store_record_rate(s, "derived", rate, gross, fee_bps,
                             net_diff, value, 1700000000ULL) == 0);

    sqlite3 *db = NULL;
    assert(sqlite3_open(path, &db) == SQLITE_OK);
    assert(scalar_i64(db, "SELECT count(*) FROM rate_history") == 1);

    /* Re-publishing an unchanged rate must not append — otherwise the table
     * grows once per template poll rather than once per actual change. */
    for (int i = 0; i < 10; ++i) {
        assert(store_record_rate(s, "derived", rate, gross, fee_bps,
                                 net_diff, value, 1700000100ULL + (uint64_t)i) == 0);
    }
    assert(scalar_i64(db, "SELECT count(*) FROM rate_history") == 1);

    /* A moved block value is a new rate and must append. */
    int64_t value2 = 312500141;
    double  gross2 = (double)value2 / net_diff;
    double  rate2  = gross2 * (1.0 - (double)fee_bps / 10000.0);
    assert(store_record_rate(s, "derived", rate2, gross2, fee_bps,
                             net_diff, value2, 1700000200ULL) == 0);
    assert(scalar_i64(db, "SELECT count(*) FROM rate_history") == 2);

    /* So is a changed source at an otherwise identical rate. */
    assert(store_record_rate(s, "override", rate2, gross2, fee_bps,
                             net_diff, value2, 1700000300ULL) == 0);
    assert(scalar_i64(db, "SELECT count(*) FROM rate_history") == 3);

    /* Every logged rate must follow from its own recorded inputs. */
    assert(scalar_i64(db,
        "SELECT count(*) FROM rate_history WHERE ABS(rate_sats_per_diff"
        "  - (block_value_sats*1.0/network_difficulty)"
        "    *(1-fee_bps/10000.0)) > 1e-9") == 0);

    /* And a share credited at one of those rates must be traceable to it —
     * exact equality, because it is the same double on both sides. */
    assert(store_record_share_addr(s, "w", "addr", 1700000400000ULL, 2.0,
                                   0, NULL, (int64_t)(2.0 * rate2), rate2) == 0);
    assert(store_flush(s) == 0);
    assert(scalar_i64(db,
        "SELECT count(*) FROM shares s WHERE s.rate_used > 0 AND NOT EXISTS ("
        "  SELECT 1 FROM rate_history r"
        "   WHERE r.rate_sats_per_diff = s.rate_used)") == 0);

    sqlite3_close(db);
    store_close(s);
    printf("  ok test_rate_history\n");
}

/* Template history: one row per materially distinct template. Repeat polls
 * fold into the row they match instead of appending — the block value moves
 * on nearly every poll, so keying on it grew the table by thousands of rows a
 * day, all of them fee churn at a height already recorded. */
static void test_template_history(void) {
    const char *path = fresh_db_path();
    store_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.path, sizeof(cfg.path), "%s", path);
    store_t *s = NULL;
    assert(store_open(&cfg, &s) == 0);

    store_template_t t;
    memset(&t, 0, sizeof t);
    t.ts_s = 1700000000; t.height = 977817;
    t.prev_hash = "00000000000000000000000000000000000000000000000000000000000000ab";
    t.bits = "1a3839e6"; t.network_difficulty = 298383.4976083073;
    t.coinbase_value_sats = 312500500; t.tx_count = 1; t.tx_fees_sats = 500;
    t.source = "enforcer"; t.cb_spendable = 1; t.cb_op_returns = 2;
    t.longpoll = 1; t.rate_sats_per_diff = 1036.8368;

    assert(store_record_template(s, &t) == 0);

    sqlite3 *db = NULL;
    assert(sqlite3_open(path, &db) == SQLITE_OK);
    assert(scalar_i64(db, "SELECT count(*) FROM templates") == 1);

    assert(scalar_i64(db, "SELECT polls FROM templates") == 1);
    assert(scalar_i64(db, "SELECT last_seen FROM templates") == 1700000000);

    /* Re-publishing the same work must not append, even as time moves on. */
    for (int i = 0; i < 10; ++i) {
        t.ts_s += 30;
        assert(store_record_template(s, &t) == 0);
    }
    assert(scalar_i64(db, "SELECT count(*) FROM templates") == 1);

    /* Folded, not discarded: ts stays first-seen, so the row spans the window
     * the template was actually mined over. */
    assert(scalar_i64(db, "SELECT polls FROM templates")     == 11);
    assert(scalar_i64(db, "SELECT ts FROM templates")        == 1700000000);
    assert(scalar_i64(db, "SELECT last_seen FROM templates") == 1700000300);

    /* Fee churn at the same tip is the common case and must not append — but
     * the row has to carry the latest numbers, not the first ones. */
    t.coinbase_value_sats = 312501999; t.tx_count = 42; t.tx_fees_sats = 1999;
    assert(store_record_template(s, &t) == 0);
    assert(scalar_i64(db, "SELECT count(*) FROM templates") == 1);
    assert(scalar_i64(db, "SELECT coinbase_value_sats FROM templates") == 312501999);
    assert(scalar_i64(db, "SELECT tx_count FROM templates")            == 42);
    assert(scalar_i64(db, "SELECT tx_fees_sats FROM templates")        == 1999);

    /* A new tip is new work. */
    t.height = 977818;
    t.prev_hash = "00000000000000000000000000000000000000000000000000000000000000cd";
    assert(store_record_template(s, &t) == 0);
    assert(scalar_i64(db, "SELECT count(*) FROM templates") == 2);

    /* Switching template source decides whether blocks can carry sidechain
     * commitments at all, so it opens a row even mid-height — folding that
     * transition away would hide the exact regression this table is for. */
    t.source = "bitcoind"; t.cb_spendable = 0; t.cb_op_returns = 0;
    assert(store_record_template(s, &t) == 0);
    assert(scalar_i64(db, "SELECT count(*) FROM templates") == 3);
    assert(scalar_i64(db,
        "SELECT cb_op_returns FROM templates ORDER BY id DESC LIMIT 1") == 0);
    assert(scalar_i64(db,
        "SELECT count(*) FROM templates WHERE source='enforcer'") == 2);

    /* Losing a sidechain commitment while still on the enforcer is the same
     * class of regression, and is invisible from every other column. */
    t.source = "enforcer"; t.cb_spendable = 1; t.cb_op_returns = 2;
    assert(store_record_template(s, &t) == 0);
    t.cb_op_returns = 1;
    assert(store_record_template(s, &t) == 0);
    assert(scalar_i64(db, "SELECT count(*) FROM templates") == 5);

    sqlite3_close(db);
    store_close(s);
    printf("  ok test_template_history\n");
}

/* A concurrent writer must not cost shares.
 *
 * This reproduces a real incident: a maintenance script took the write lock
 * for a moment, and because the connection had no busy_timeout the writer
 * thread got SQLITE_BUSY instantly. Its batch was already out of the ring, so
 * accepted shares — already acknowledged to the miner — were logged and
 * discarded. Holding the lock here for longer than one commit window forces
 * exactly that race. */
static void test_commit_survives_a_locked_db(void) {
    const char *path = fresh_db_path();
    store_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.path, sizeof(cfg.path), "%s", path);
    cfg.commit_window_ms  = 20;    /* wake often, so we hit the lock */
    cfg.commit_max_shares = 10;    /* several batches, not one */
    store_t *s = NULL;
    assert(store_open(&cfg, &s) == 0);

    /* A second connection grabs the write lock, as `sqlite3 < script.sql`
     * would. */
    sqlite3 *hog = NULL;
    assert(sqlite3_open(path, &hog) == SQLITE_OK);
    assert(sqlite3_exec(hog, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK);

    const int N = 40;
    for (int i = 0; i < N; ++i) {
        char w[32];
        snprintf(w, sizeof(w), "miner%d", i % 4);
        assert(store_record_share(s, w, 1700000000000ULL + i, 1.0, 0, NULL) == 0);
    }

    /* Hold it well past several commit windows, then let go. */
    struct timespec hold = { .tv_sec = 0, .tv_nsec = 300L * 1000000L };
    nanosleep(&hold, NULL);
    assert(sqlite3_exec(hog, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_close(hog);

    assert(store_flush(s) == 0);

    store_stats_t st;
    store_get_stats(s, &st);
    assert(st.events_lost == 0);
    assert(st.shares_dropped == 0);

    sqlite3 *db = NULL;
    assert(sqlite3_open(path, &db) == SQLITE_OK);
    /* Every accepted share is on the ledger. Before the fix this came back
     * short, with the shortfall visible only as an ERROR line. */
    assert(scalar_i64(db, "SELECT count(*) FROM shares") == N);

    sqlite3_close(db);
    store_close(s);
    printf("  ok test_commit_survives_a_locked_db\n");
}

/* Retention keeps the table bounded. Pruning runs when a new row is opened
 * and is driven off the template's own clock, so this is deterministic. */
static void test_template_retention(void) {
    const char *path = fresh_db_path();
    store_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.path, sizeof(cfg.path), "%s", path);
    cfg.templates_retention_days = 7;
    store_t *s = NULL;
    assert(store_open(&cfg, &s) == 0);

    store_template_t t;
    memset(&t, 0, sizeof t);
    t.prev_hash = "00000000000000000000000000000000000000000000000000000000000000ab";
    t.bits = "1a3839e6"; t.network_difficulty = 298383.4976083073;
    t.coinbase_value_sats = 312500500; t.tx_count = 1; t.tx_fees_sats = 500;
    t.source = "enforcer"; t.cb_spendable = 1; t.cb_op_returns = 2;
    t.longpoll = 1; t.rate_sats_per_diff = 1036.8368;

    /* 30 distinct templates, one day apart. */
    for (int i = 0; i < 30; ++i) {
        t.ts_s  = 1700000000 + (int64_t)i * 86400;
        t.height = 977817 + i;
        assert(store_record_template(s, &t) == 0);
    }

    sqlite3 *db = NULL;
    assert(sqlite3_open(path, &db) == SQLITE_OK);
    /* The 7-day window holds the row just written plus the seven inside the
     * cutoff; everything older is gone. */
    assert(scalar_i64(db, "SELECT count(*) FROM templates") == 8);
    assert(scalar_i64(db, "SELECT MIN(height) FROM templates") == 977817 + 22);
    assert(scalar_i64(db, "SELECT MAX(height) FROM templates") == 977817 + 29);

    /* 0 means keep everything. */
    store_close(s);
    store_cfg_t keep;
    memset(&keep, 0, sizeof(keep));
    snprintf(keep.path, sizeof(keep.path), "%s", fresh_db_path());
    keep.templates_retention_days = 0;
    store_t *s2 = NULL;
    assert(store_open(&keep, &s2) == 0);
    for (int i = 0; i < 30; ++i) {
        t.ts_s  = 1700000000 + (int64_t)i * 86400;
        t.height = 977817 + i;
        assert(store_record_template(s2, &t) == 0);
    }
    sqlite3 *db2 = NULL;
    assert(sqlite3_open(keep.path, &db2) == SQLITE_OK);
    assert(scalar_i64(db2, "SELECT count(*) FROM templates") == 30);

    sqlite3_close(db2);
    sqlite3_close(db);
    store_close(s2);
    printf("  ok test_template_retention\n");
}

/* The window floor. At a difficulty reset the work target is satisfied by a
 * couple of shares, which would leave the window a few seconds wide and squeeze
 * every miner but the most recent out of it. The floor must extend the window
 * regardless of how little work satisfies the target. */
static void test_proportional_window_floor(void) {
    const char *path = fresh_db_path();
    store_cfg_t cfg = {0};
    snprintf(cfg.path, sizeof(cfg.path), "%s", path);
    cfg.commit_window_ms = 20;
    cfg.commit_max_shares = 100;

    store_t *s = NULL;
    assert(store_open(&cfg, &s) == 0);

    /* Ten minutes of shares, one per minute, difficulty 1 — the shape of the
     * minimum-difficulty window right after a fork. */
    uint64_t now_ms_ = 1700000600000ULL;
    for (int i = 0; i < 10; i++) {
        uint64_t ts = now_ms_ - (uint64_t)i * 60000ULL;
        assert(store_record_share_addr(s, "w1", "bcrt1qaaa", ts, 1.0,
                                       0, NULL, 0, 0.0) == 0);
    }
    assert(store_flush(s) == 0);

    /* Difficulty 1 network, k=3: the work target is met by three shares, so
     * without a floor the window is ~3 minutes wide. */
    uint64_t start_no_floor = 0, start_floor = 0;
    double diff_no_floor = 0.0, diff_floor = 0.0;
    assert(store_prop_compute_window(s, 3.0, now_ms_, 0,
                                     &start_no_floor, &diff_no_floor, NULL) == 0);
    assert(diff_no_floor >= 3.0);
    assert(diff_no_floor < 5.0);            /* stopped early, as designed */

    /* With a 600-second floor it must reach back over the whole ten minutes. */
    assert(store_prop_compute_window(s, 3.0, now_ms_, 600,
                                     &start_floor, &diff_floor, NULL) == 0);
    assert(start_floor < start_no_floor);   /* the floor genuinely widened it */
    assert(diff_floor >= 9.0);              /* essentially every share */
    assert(now_ms_ - start_floor >= 540000ULL);

    /* The floor must never SHRINK a window that work alone made wider: a high
     * work target still wins. */
    uint64_t start_big = 0; double diff_big = 0.0;
    assert(store_prop_compute_window(s, 9.0, now_ms_, 60,
                                     &start_big, &diff_big, NULL) == 0);
    assert(diff_big >= 9.0);

    store_close(s);
    printf("  ok test_proportional_window_floor\n");
}

/* The regression that made PPLNS collapse into solo mining.
 *
 * vardiff clamps the share difficulty so it never exceeds the network
 * difficulty, and is_block is `hash <= network_target`. Once that clamp binds
 * -- the whole post-fork minimum-difficulty window -- share target equals
 * network target and EVERY accepted share is also a block. The window queries
 * used to filter `is_block = 0`, so they saw nothing, prop_build_plan found an
 * empty window, and every block paid its finder directly instead of the
 * window. Observed across 24 consecutive regtest blocks.
 *
 * Regtest cannot produce this state on purpose (its difficulty is ~2e9x below
 * stratum diff-1, so the stack sits in this regime permanently and an earlier
 * proof had to hand-seed synthetic is_block=0 rows). So the fixture builds it
 * directly -- and asserts there is genuinely not one ordinary share in the DB,
 * because a fixture that quietly leaks one would pass for the wrong reason. */
static void test_proportional_window_when_every_share_is_a_block(void) {
    const char *path = fresh_db_path();
    store_cfg_t cfg = {0};
    snprintf(cfg.path, sizeof(cfg.path), "%s", path);
    cfg.commit_window_ms = 20;
    cfg.commit_max_shares = 100;

    store_t *s = NULL;
    assert(store_open(&cfg, &s) == 0);

    /* Two miners, 60/40 of the work, every share flagged is_block=1. */
    uint64_t now_ms_ = 1700000600000ULL;
    for (int i = 0; i < 60; i++)
        assert(store_record_share_addr(s, "w.a", "bcrt1qaaa",
                                       now_ms_ - (uint64_t)i * 1000ULL, 1.0,
                                       1 /* is_block */, "deadbeef", 0, 0.0) == 0);
    for (int i = 0; i < 40; i++)
        assert(store_record_share_addr(s, "w.b", "bcrt1qbbb",
                                       now_ms_ - (uint64_t)i * 1000ULL, 1.0,
                                       1 /* is_block */, "deadbeef", 0, 0.0) == 0);
    assert(store_flush(s) == 0);

    /* Precondition, asserted rather than assumed: no ordinary share exists, so
     * anything the window returns had to come from block-shares. */
    sqlite3 *db = NULL;
    assert(sqlite3_open(path, &db) == SQLITE_OK);
    assert(scalar_i64(db, "SELECT count(*) FROM shares") == 100);
    assert(scalar_i64(db, "SELECT count(*) FROM shares WHERE is_block = 0") == 0);
    sqlite3_close(db);

    /* The aggregate must see all 100 difficulty, not zero. */
    uint64_t start_ms = 0; double actual_diff = 0.0; int truncated = 0;
    int wrc = store_prop_compute_window(s, 3.0, now_ms_, 600,
                                        &start_ms, &actual_diff, &truncated);
    if (wrc != 0 || actual_diff <= 0.0) {
        fprintf(stderr, "window is empty with 100 block-shares present (rc=%d, "
                        "diff=%.2f) — the is_block filter is back and PPLNS is "
                        "running as solo\n", wrc, actual_diff);
        abort();
    }
    assert(actual_diff >= 100.0);

    /* And the payable-address split must be the real 60/40, so a plan can be
     * built at all -- an empty address list is the other route to the
     * pay-the-finder fallback. */
    pplns_addr_t *addrs = NULL; size_t n_addrs = 0;
    assert(store_prop_window_addrs(s, start_ms, now_ms_, &addrs, &n_addrs) == 0);
    assert(n_addrs == 2);
    double a = 0.0, b = 0.0;
    for (size_t i = 0; i < n_addrs; i++) {
        if (strcmp(addrs[i].address, "bcrt1qaaa") == 0) a = addrs[i].total_difficulty;
        if (strcmp(addrs[i].address, "bcrt1qbbb") == 0) b = addrs[i].total_difficulty;
    }
    assert(a > 0.0 && b > 0.0);
    assert(a > b);                              /* 60 beats 40 */
    assert(fabs(a / (a + b) - 0.6) < 1e-9);
    assert(fabs(b / (a + b) - 0.4) < 1e-9);
    free(addrs);

    /* The paged slow path must agree: force it by asking for more work than the
     * time floor covers, with a page size small enough to need several reads. */
    store_test_set_window_limits(7, 100000);
    uint64_t start_paged = 0; double diff_paged = 0.0;
    assert(store_prop_compute_window(s, 90.0, now_ms_, 0,
                                     &start_paged, &diff_paged, NULL) == 0);
    assert(diff_paged >= 90.0);
    store_test_set_window_limits(0, 0);         /* restore the defaults */

    /* Same root cause, separate query: the hashrate median filtered block-shares
     * too, so every worker read as 0 in this regime -- blanking the dashboard
     * and resetting reconnecting miners to initial_diff.
     *
     * This one is keyed on wall-clock time (it binds time(NULL) - lookback), so
     * it needs its own shares at NOW rather than the fixed epoch above. */
    uint64_t now_s = (uint64_t)time(NULL);
    for (int i = 0; i < 24; i++)
        assert(store_record_share_addr(s, "w.now", "bcrt1qccc",
                                       (now_s - 100 + (uint64_t)i) * 1000ULL,
                                       512.0, 1 /* is_block */, "deadbeef",
                                       0, 0.0) == 0);
    assert(store_flush(s) == 0);
    double d = store_worker_recent_difficulty(s, "w.now", 3600);
    if (d <= 0.0) {
        fprintf(stderr, "recent difficulty is 0 with 24 block-shares present — "
                        "the dashboard reads empty and vardiff resets\n");
        abort();
    }
    assert(d == 512.0);

    store_close(s);
    printf("  ok test_proportional_window_when_every_share_is_a_block\n");
}

/* A reconnecting worker must resume near the difficulty it was actually
 * running at. Restarting every miner at initial_diff makes a multi-TH/s ASIC
 * climb again at 4x per vardiff window, flooding the pool and shedding shares
 * at every step -- observed live on the alpha pool after a restart. */
static void test_worker_recent_difficulty(void) {
    const char *path = fresh_db_path();
    store_cfg_t cfg = {0};
    snprintf(cfg.path, sizeof(cfg.path), "%s", path);
    cfg.commit_window_ms = 20;
    cfg.commit_max_shares = 100;
    store_t *s = NULL;
    assert(store_open(&cfg, &s) == 0);

    uint64_t now_s = (uint64_t)time(NULL);
    /* A vardiff ramp: a tail of tiny difficulties from the climb, then the
     * converged value. The median must ignore the ramp; a mean would not. */
    double ramp[] = { 1, 4, 16, 64, 256 };
    for (int i = 0; i < 5; i++)
        assert(store_record_share_addr(s, "w.ramp", "bcrt1qaaa",
                                       (now_s - 300 + (uint64_t)i) * 1000ULL,
                                       ramp[i], 0, NULL, 0, 0.0) == 0);
    for (int i = 0; i < 20; i++)
        assert(store_record_share_addr(s, "w.ramp", "bcrt1qaaa",
                                       (now_s - 200 + (uint64_t)i) * 1000ULL,
                                       4096.0, 0, NULL, 0, 0.0) == 0);
    assert(store_flush(s) == 0);

    double d = store_worker_recent_difficulty(s, "w.ramp", 3600);
    assert(d == 4096.0);                       /* the converged value, not the ramp */

    /* The live failure: a long tail of low-difficulty ramp shares from repeated
     * reconnects, then a converged value. A median over the whole lookback
     * returns the ramp; the median of the most RECENT shares must not. */
    for (int i = 0; i < 200; i++)
        assert(store_record_share_addr(s, "w.churn", "bcrt1qccc",
                                       (now_s - 3000 + (uint64_t)i) * 1000ULL,
                                       (i % 4) + 1.0, 0, NULL, 0, 0.0) == 0);
    for (int i = 0; i < 24; i++)
        assert(store_record_share_addr(s, "w.churn", "bcrt1qccc",
                                       (now_s - 120 + (uint64_t)i) * 1000ULL,
                                       13680.0, 0, NULL, 0, 0.0) == 0);
    assert(store_flush(s) == 0);
    double churn = store_worker_recent_difficulty(s, "w.churn", 3600);
    if (churn != 13680.0) {
        fprintf(stderr, "expected 13680 (converged), got %.1f — the ramp tail won\n", churn);
        abort();
    }

    /* Too little history is not worth trusting: fall back to initial_diff. */
    for (int i = 0; i < 3; i++)
        assert(store_record_share_addr(s, "w.new", "bcrt1qbbb",
                                       (now_s - 10 + (uint64_t)i) * 1000ULL,
                                       999.0, 0, NULL, 0, 0.0) == 0);
    assert(store_flush(s) == 0);
    assert(store_worker_recent_difficulty(s, "w.new", 3600) == 0.0);

    /* Unknown worker, and stale history, both yield nothing. */
    assert(store_worker_recent_difficulty(s, "nobody", 3600) == 0.0);
    assert(store_worker_recent_difficulty(s, "w.ramp", 1) == 0.0);

    store_close(s);
    printf("  ok test_worker_recent_difficulty\n");
}

/* The window walk must page rather than stop at a fixed row limit.
 *
 * Regression: the walk was a single `LIMIT 100000`, so once the share rate was
 * high enough that the configured window held more than that, the window
 * silently shrank. At fork share rates a 600 s window would have held ~813,000
 * shares and quietly become ~74 s — the pool still paying, just on a window
 * nobody chose. */
static void test_proportional_window_pages(void) {
    const char *path = fresh_db_path();
    store_cfg_t cfg = {0};
    snprintf(cfg.path, sizeof(cfg.path), "%s", path);
    cfg.commit_window_ms = 20;
    cfg.commit_max_shares = 500;
    store_t *s = NULL;
    assert(store_open(&cfg, &s) == 0);

    /* 600 shares of difficulty 1, one per second. */
    uint64_t now_s = 1700100000ULL;
    for (int i = 0; i < 600; i++)
        assert(store_record_share_addr(s, "w.page", "bcrt1qaaa",
                                       (now_s - 600 + (uint64_t)i) * 1000ULL,
                                       1.0, 0, NULL, 0, 0.0) == 0);
    assert(store_flush(s) == 0);
    uint64_t before_ms = now_s * 1000ULL;

    /* Baseline: one big page, work target 500 -> needs 500 shares. */
    store_test_set_window_limits(100000, 5000000);
    uint64_t start_big = 0; double diff_big = 0.0; int trunc_big = -1;
    assert(store_prop_compute_window(s, 500.0, before_ms, 0,
                                     &start_big, &diff_big, &trunc_big) == 0);
    assert(diff_big >= 500.0);
    assert(trunc_big == 0);

    /* Same query, page size 7. Paging must reach the identical answer — under
     * the old single-LIMIT walk a small limit silently returned less work. */
    store_test_set_window_limits(7, 5000000);
    uint64_t start_pg = 0; double diff_pg = 0.0; int trunc_pg = -1;
    assert(store_prop_compute_window(s, 500.0, before_ms, 0,
                                     &start_pg, &diff_pg, &trunc_pg) == 0);
    assert(trunc_pg == 0);
    if (start_pg != start_big || diff_pg != diff_big) {
        fprintf(stderr, "paging changed the window: %llu/%.1f vs %llu/%.1f\n",
                (unsigned long long)start_pg, diff_pg,
                (unsigned long long)start_big, diff_big);
        abort();
    }

    /* Cap below what the target needs: the window IS short, and that must be
     * reported rather than passed off as the configured window. */
    store_test_set_window_limits(7, 21);
    uint64_t start_t = 0; double diff_t = 0.0; int trunc = 0;
    assert(store_prop_compute_window(s, 500.0, before_ms, 0,
                                     &start_t, &diff_t, &trunc) == 0);
    assert(trunc == 1);
    assert(diff_t < 500.0);

    store_test_set_window_limits(0, 0);   /* restore defaults */
    store_close(s);
    printf("  ok test_proportional_window_pages\n");
}

static void test_proportional(void) {
    const char *path = fresh_db_path();
    store_cfg_t cfg = {0};
    snprintf(cfg.path, sizeof(cfg.path), "%s", path);
    cfg.commit_window_ms = 20;
    cfg.commit_max_shares = 100;

    store_t *s = NULL;
    assert(store_open(&cfg, &s) == 0);

    /* Three miners with different share rates. */
    struct { const char *worker; const char *addr; double diff; int count; } miners[] = {
        { "m1", "bcrt1qaaa", 1.0, 5 },
        { "m2", "bcrt1qbbb", 2.0, 3 },
        { "m3", "bcrt1qccc", 0.5, 4 },
    };
    uint64_t base_ts = 1700000000000ULL;
    for (size_t m = 0; m < 3; m++) {
        for (int i = 0; i < miners[m].count; i++) {
            int rc = store_record_share_addr(s, miners[m].worker, miners[m].addr,
                base_ts + (uint64_t)(m * 10 + i), miners[m].diff, 0, NULL, 0, 0.0);
            assert(rc == 0);
        }
    }
    assert(store_flush(s) == 0);

    pplns_claim_t *ledger = NULL;
    size_t n_ledger = 0;
    assert(store_prop_get_ledger(s, &ledger, &n_ledger) == 0);
    assert(n_ledger == 0);
    free(ledger);

    uint64_t start_ms = 0;
    double actual_diff = 0.0;
    assert(store_prop_compute_window(s, 8.0, base_ts + 100, 0, &start_ms, &actual_diff, NULL) == 0);
    assert(actual_diff >= 8.0);

    pplns_addr_t *addrs = NULL;
    size_t n_addrs = 0;
    assert(store_prop_window_addrs(s, start_ms, base_ts + 100, &addrs, &n_addrs) == 0);
    assert(n_addrs == 3);
    double sum_diff = 0.0;
    for (size_t i = 0; i < n_addrs; i++) sum_diff += addrs[i].total_difficulty;
    assert(sum_diff > actual_diff - 0.001 && sum_diff < actual_diff + 0.001);

    /* A min_payout high enough that the smallest miner is deferred. */
    pplns_payout_t payouts[12] = {0};
    size_t n_payouts = 0;
    pplns_claim_t ledger_buf[16] = {0};
    size_t n_ledger_out = 0;
    int64_t reward = 1000000LL;
    int rc = pplns_compute_payouts(reward, actual_diff, addrs, n_addrs,
                                   ledger_buf, 16, 0, &n_ledger_out,
                                   300000LL, 12, payouts, &n_payouts);
    assert(rc == 0);
    assert(n_payouts >= 1 && n_payouts < n_addrs);

    /* The coinbase pays the whole reward — that is the point of the model. */
    int64_t paid = 0;
    for (size_t i = 0; i < n_payouts; i++) paid += payouts[i].sats;
    assert(paid == reward);

    /* And the claims it leaves behind cancel out. */
    double claim_sum = 0.0;
    for (size_t i = 0; i < n_ledger_out; i++) claim_sum += ledger_buf[i].claim_fraction;
    assert(claim_sum < 1e-9 && claim_sum > -1e-9);
    assert(n_ledger_out > 0);   /* somebody was deferred at this threshold */

    /* Settle the block and verify the ledger round-trips through SQLite. */
    /* The real block row is written by store_record_block(), exactly as
     * on_block_found_cb() does it for every block, pooled or solo. */
    assert(store_record_block(s, (base_ts + 200) * 1000ULL, 100, "blockhash1",
                              "worker1", addrs[0].address, reward, 0) == 0);

    rc = store_prop_settle_block(s, base_ts + 200, ledger_buf, n_ledger_out);
    assert(rc == 0);
    store_flush(s);

    sqlite3 *db = NULL;
    assert(sqlite3_open(path, &db) == SQLITE_OK);
    /* REGRESSION: settling must not write a second blocks_found row. It used to,
     * with finder NULL and reward 0, so every pooled block was counted twice —
     * 27 rows for 26 blocks on regtest, 2026-08-20. store_record_block() is the
     * sole writer; if this reads 2, the duplicate insert is back. */
    int64_t nblocks = scalar_i64(db, "SELECT count(*) FROM blocks_found");
    assert(nblocks == 1);
    assert(scalar_i64(db, "SELECT count(*) FROM blocks_found "
                          "WHERE finder_id IS NULL OR reward_sats IS NULL "
                          "   OR reward_sats = 0") == 0);

    pplns_claim_t *stored = NULL;
    size_t n_stored = 0;
    assert(store_prop_get_ledger(s, &stored, &n_stored) == 0);
    assert(n_stored == n_ledger_out);
    double stored_sum = 0.0;
    for (size_t i = 0; i < n_stored; i++) stored_sum += stored[i].claim_fraction;
    assert(stored_sum < 1e-9 && stored_sum > -1e-9);
    /* Every claim survived with its sign and magnitude intact. */
    for (size_t i = 0; i < n_ledger_out; i++) {
        int seen = 0;
        for (size_t j = 0; j < n_stored; j++) {
            if (strcmp(stored[j].address, ledger_buf[i].address) != 0) continue;
            double d = stored[j].claim_fraction - ledger_buf[i].claim_fraction;
            assert(d < 1e-12 && d > -1e-12);
            seen = 1;
        }
        assert(seen);
    }

    /* Settling a second block must REPLACE the ledger, not accumulate stale
     * rows — the ledger is whole-state, not a delta. */
    size_t n_second = 0;
    rc = pplns_compute_payouts(reward, actual_diff, addrs, n_addrs,
                               ledger_buf, 16, n_ledger_out, &n_second,
                               300000LL, 12, payouts, &n_payouts);
    assert(rc == 0);
    rc = store_prop_settle_block(s, base_ts + 300, ledger_buf, n_second);
    assert(rc == 0);
    free(stored);
    stored = NULL;
    assert(store_prop_get_ledger(s, &stored, &n_stored) == 0);
    assert(n_stored == n_second);

    sqlite3_close(db);
    free(addrs);
    free(stored);
    store_close(s);
    printf("  ok test_proportional\n");
}

int main(void) {
    log_init(2 /* WARN */);
    printf("running test_store...\n");
    test_basic();
    test_rejects();
    test_concurrent();
    test_drop();
    test_credited_sats();
    test_rate_history();
    test_template_history();
    test_template_retention();
    test_commit_survives_a_locked_db();
    test_worker_recent_difficulty();
    test_proportional_window_pages();
    test_proportional();
    test_proportional_window_floor();
    test_proportional_window_when_every_share_is_a_block();
    cleanup_dbs();
    printf("all tests passed\n");
    return 0;
}
