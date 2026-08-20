/* SQLite-backed event store with a single batched writer thread.
 *
 * Producers enqueue events into a bounded ring buffer (mutex + cond).
 * The writer thread wakes either on signal or every commit_window_ms,
 * drains up to commit_max_shares events into one transaction, commits.
 *
 * Worker name -> id resolution is cached in a small open-addressing
 * hash table (16384 slots) to avoid hammering SQLite for repeats.
 */

#include "store.h"
#include "log.h"

#include <sqlite3.h>

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

/* Keep in sync with schema.sql.
 *
 * Split into parts only because one concatenated literal now exceeds the
 * 4095 characters ISO C99 requires a compiler to support, which -Wpedantic
 * flags as an error here. The parts are applied in order and the split point
 * carries no meaning — when adding tables, start a new part rather than
 * growing one past the limit. */
static const char *SCHEMA_SQL_PARTS[] = {
    "PRAGMA journal_mode = WAL;\n"
    "PRAGMA synchronous = NORMAL;\n"
    "PRAGMA foreign_keys = ON;\n"
    /* Without this SQLite returns SQLITE_BUSY the instant another connection
     * holds the write lock — no waiting at all. The writer thread has already
     * dequeued its batch by then, so a single concurrent writer (a manual
     * sqlite3 session, a backup, a maintenance script) silently destroyed
     * shares the miner had been told were accepted. Wait instead. */
    "PRAGMA busy_timeout = 5000;\n"
    "CREATE TABLE IF NOT EXISTS workers ("
    "  id              INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  name            TEXT UNIQUE NOT NULL,"
    "  first_seen      INTEGER NOT NULL,"
    "  last_seen       INTEGER NOT NULL,"
    "  payout_address  TEXT"
    ");"
    /* credited_sats is what this share was ACTUALLY credited at the time it
     * was accepted — not something to be recomputed later from a rate read
     * from config. The rate is derived per-template and moves with network
     * difficulty, so recomputing historical shares against a current rate
     * silently misreports them. Audits must sum this column. 0 in solo mode,
     * where no PPS accrual happens.
     *
     * rate_used is the exact rate that produced credited_sats. Recording the
     * multiplicand next to the product is what turns the credit from
     * self-attested into checkable: CAST(difficulty * rate_used AS INTEGER)
     * must equal credited_sats for every row, and that holds no matter how
     * far the rate has since moved. */
    "CREATE TABLE IF NOT EXISTS shares ("
    "  id            INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  worker_id     INTEGER NOT NULL REFERENCES workers(id),"
    "  ts            INTEGER NOT NULL,"
    "  difficulty    REAL NOT NULL,"
    "  is_block      INTEGER NOT NULL DEFAULT 0,"
    "  block_hash    TEXT,"
    "  credited_sats INTEGER NOT NULL DEFAULT 0,"
    "  rate_used     REAL NOT NULL DEFAULT 0"
    ");"
    "CREATE INDEX IF NOT EXISTS shares_ts_idx ON shares(ts);"
    "CREATE INDEX IF NOT EXISTS shares_worker_ts_idx ON shares(worker_id, ts);"
    "CREATE TABLE IF NOT EXISTS rejects ("
    "  id          INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  worker_name TEXT,"
    "  ts          INTEGER NOT NULL,"
    "  reason      TEXT NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS rejects_ts_idx ON rejects(ts);"
    "CREATE TABLE IF NOT EXISTS blocks_found ("
    "  id              INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  ts              INTEGER NOT NULL,"
    "  height          INTEGER NOT NULL,"
    "  hash            TEXT NOT NULL,"
    "  finder_id       INTEGER REFERENCES workers(id),"
    "  finder_address  TEXT,"
    "  reward_sats     INTEGER,"
    "  fee_sats        INTEGER"
    ");"
    "CREATE INDEX IF NOT EXISTS blocks_found_ts_idx ON blocks_found(ts);"
    /* Single-row mirror of the upstream bitcoind tip. Updated on every
     * tip-watcher poll. The dashboard reads this for 'latest block' /
     * 'time since last block' without needing any RPC of its own. */
    "CREATE TABLE IF NOT EXISTS node_status ("
    "  id              INTEGER PRIMARY KEY CHECK (id = 1),"
    "  tip_height      INTEGER,"
    "  tip_hash        TEXT,"
    "  tip_observed_at INTEGER,"
    "  updated_at      INTEGER"
    ");"
    /* Single source of truth for what the running proxy is actually paying.
     *
     * The dashboard MUST read the rate from here rather than from its own
     * config or environment. Holding the same number in two places is how
     * the audit ends up disagreeing with the ledger it is meant to check.
     *
     * rate_source is 'derived' (rate computed from the live template and
     * fee_bps — the default) or 'override' (operator pinned
     * pps_sats_per_diff, which is taken NET of fee and bypasses fee_bps).
     * effective_fee_bps is what the numbers actually imply, which under an
     * override can differ from the configured fee_bps. */
    "CREATE TABLE IF NOT EXISTS pool_meta ("
    "  id                  INTEGER PRIMARY KEY CHECK (id = 1),"
    "  pool_mode           TEXT,"
    "  fee_bps             INTEGER,"
    "  rate_source         TEXT,"
    "  rate_sats_per_diff  REAL,"     /* effective, net of fee */
    "  gross_sats_per_diff REAL,"     /* fair value before fee */
    "  effective_fee_bps   REAL,"
    "  network_difficulty  REAL,"
    "  block_value_sats    INTEGER,"
    "  credited_from       INTEGER,"  /* first ts with credited_sats populated */
    "  updated_at          INTEGER,"
    /* Mirror of the writer thread's events_lost counter. Lives here because
     * it is otherwise process-local: accepted work that never reached the DB
     * is invisible to every query, so the dashboard could not surface it.
     * Written on the template path, which is a different connection state
     * from the batch commit that failed. */
    "  events_lost         INTEGER NOT NULL DEFAULT 0"
    ");"
    /* Append-only log of every distinct rate the proxy has paid at. pool_meta
     * is overwritten on every template, so without this the rate a share was
     * credited at is unrecoverable after the fact. Appended only when the
     * tuple changes. Prunable: shares.rate_used carries per-share
     * verification on its own; this table exists to show the rate itself was
     * derived fairly from the template. */
    "CREATE TABLE IF NOT EXISTS rate_history ("
    "  id                  INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  ts                  INTEGER NOT NULL,"
    "  rate_sats_per_diff  REAL    NOT NULL,"
    "  gross_sats_per_diff REAL    NOT NULL,"
    "  fee_bps             INTEGER NOT NULL,"
    "  network_difficulty  REAL    NOT NULL,"
    "  block_value_sats    INTEGER NOT NULL,"
    "  rate_source         TEXT    NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS rate_history_ts_idx   ON rate_history(ts);"
    "CREATE INDEX IF NOT EXISTS rate_history_rate_idx ON rate_history(rate_sats_per_diff);"
    /* What the pool is mining now, and what it mined before. One row per
     * materially distinct template — see store_record_template(). `source`
     * distinguishes a backend-dictated coinbase (BIP22 "coinbasetxn", carries
     * the BIP300/301 commitments) from one we built ourselves (carries none,
     * so no sidechain can be merge-mined into the block) — see the schema.sql
     * comment. */
    "CREATE TABLE IF NOT EXISTS templates ("
    "  id                  INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  ts                  INTEGER NOT NULL,"
    "  height              INTEGER NOT NULL,"
    "  prev_hash           TEXT    NOT NULL,"
    "  bits                TEXT    NOT NULL,"
    "  network_difficulty  REAL    NOT NULL,"
    "  coinbase_value_sats INTEGER NOT NULL,"
    "  tx_count            INTEGER NOT NULL,"
    "  tx_fees_sats        INTEGER NOT NULL,"
    "  source              TEXT    NOT NULL,"
    "  cb_spendable        INTEGER NOT NULL,"
    "  cb_op_returns       INTEGER NOT NULL,"
    "  longpoll            INTEGER NOT NULL,"
    "  rate_sats_per_diff  REAL    NOT NULL,"
    "  last_seen           INTEGER NOT NULL DEFAULT 0,"
    "  polls               INTEGER NOT NULL DEFAULT 1"
    ");"
    "CREATE INDEX IF NOT EXISTS templates_ts_idx     ON templates(ts);"
    "CREATE INDEX IF NOT EXISTS templates_height_idx ON templates(height);",

    /* ---- part 2 ---- */
    /* PPS accrual ledger. One row per worker; the C proxy only INCREMENTS
     * accrued_sats. paid_sats is updated by a downstream payout service
     * that issues Thunder transactions to drain accrued - paid. */
    "CREATE TABLE IF NOT EXISTS pps_credits ("
    "  worker_id     INTEGER PRIMARY KEY REFERENCES workers(id),"
    "  accrued_sats  INTEGER NOT NULL DEFAULT 0,"
    "  paid_sats     INTEGER NOT NULL DEFAULT 0,"
    "  last_updated  INTEGER NOT NULL"
    ");"
    /* In-flight payout ledger. Owned by the payout worker; the C proxy
     * creates the table for fresh-DB convenience but never writes here.
     * See schema.sql for the crash-semantics commentary. */
    "CREATE TABLE IF NOT EXISTS payouts_in_flight ("
    "  id            INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  worker_id     INTEGER NOT NULL REFERENCES workers(id),"
    "  sats          INTEGER NOT NULL,"
    "  txid          TEXT NOT NULL DEFAULT '',"
    "  started_at    INTEGER NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS payouts_in_flight_worker_idx ON payouts_in_flight(worker_id);"
    /* Every attempt to broadcast a transaction, successful or not. Owned by
     * the dashboard and the payout worker; the C proxy never writes here.
     *
     * `deposits` and `payouts` record what actually happened. A failed
     * broadcast is not a deposit or a payout, but it is the thing an
     * operator most needs to see — so it lands here instead, with the raw
     * transaction whenever it can be recovered. Without this a failure left
     * nothing behind but a truncated flash message.
     *
     * raw_tx is the full hex when obtainable. For a deposit that failed at
     * broadcast the enforcer has still signed and stored the tx, so it can
     * be recovered afterwards via ListSidechainDepositTransactions; `stage`
     * records how far the attempt got. */
    "CREATE TABLE IF NOT EXISTS tx_attempts ("
    "  id          INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  ts          INTEGER NOT NULL,"          /* unix seconds */
    "  kind        TEXT    NOT NULL,"          /* 'deposit' | 'payout' */
    "  status      TEXT    NOT NULL,"          /* 'broadcast' | 'failed' */
    "  stage       TEXT,"                      /* step reached when it failed */
    "  txid        TEXT,"
    "  raw_tx      TEXT,"                      /* full hex, when recoverable */
    "  amount_sats INTEGER,"
    "  fee_sats    INTEGER,"
    "  destination TEXT,"
    "  worker_id   INTEGER,"                   /* payouts only */
    "  error       TEXT,"                      /* full, never truncated */
    "  detail      TEXT"                       /* JSON: request params */
    ");"
    "CREATE INDEX IF NOT EXISTS tx_attempts_ts_idx ON tx_attempts(ts);"
    "CREATE INDEX IF NOT EXISTS tx_attempts_kind_idx ON tx_attempts(kind, ts);"
    /* pps-classic deposit ledger. Owned by the admin dashboard; the C
     * proxy never writes here. Created here so a fresh DB is complete. */
    "CREATE TABLE IF NOT EXISTS deposits ("
    "  id                INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  ts                INTEGER NOT NULL,"
    "  btc_txid          TEXT    NOT NULL,"
    "  sats_deposited    INTEGER NOT NULL,"
    "  fee_sats          INTEGER NOT NULL,"
    "  thunder_recipient TEXT    NOT NULL,"
    "  ctip_seq_before   INTEGER,"
    "  ctip_seq_after    INTEGER,"
    "  notes             TEXT"
    ");"
    "CREATE INDEX IF NOT EXISTS deposits_ts_idx ON deposits(ts);"
    /* Payout history — permanent record populated by payout worker. */
    "CREATE TABLE IF NOT EXISTS payouts ("
    "  id           INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  worker_id    INTEGER NOT NULL REFERENCES workers(id),"
    "  sats         INTEGER NOT NULL,"
    "  fee_sats     INTEGER NOT NULL,"
    "  txid         TEXT    NOT NULL,"
    "  paid_at      INTEGER NOT NULL,"
    "  note         TEXT"
    ");"
    "CREATE INDEX IF NOT EXISTS payouts_worker_ts_idx ON payouts(worker_id, paid_at);"
    "CREATE INDEX IF NOT EXISTS payouts_paid_at_idx   ON payouts(paid_at);",

    /* ---- part 3 ---- */
    /* Proportional / PPLNS deferred-claim ledger. NOT a balance: the pool holds
     * no funds. claim_fraction is a signed fraction of one block reward —
     * positive means the address was skipped (its cut fell below
     * prop_min_payout_sats, or it was demoted to keep the block under the
     * output/weight cap) and is owed that fraction of a future block; negative
     * means it was paid early, covering someone else's skipped share. The ledger
     * sums to zero. See src/pplns.h for why this is a fraction and not sats or
     * raw difficulty. address is the miner's payout_address from workers. */
    "CREATE TABLE IF NOT EXISTS prop_ledger ("
    "  address         TEXT PRIMARY KEY,"
    "  claim_fraction  REAL NOT NULL DEFAULT 0,"
    "  last_settled_ts INTEGER"
    ");"
    "CREATE INDEX IF NOT EXISTS prop_ledger_ts_idx ON prop_ledger(last_settled_ts);"
};

/* Forward-compat: ALTER existing DBs to add columns that didn't exist in
 * earlier schemas. Duplicate-column errors are silently ignored. */
static const char *MIGRATIONS_SQL[] = {
    "ALTER TABLE workers      ADD COLUMN payout_address TEXT",
    "ALTER TABLE blocks_found ADD COLUMN finder_address TEXT",
    "ALTER TABLE blocks_found ADD COLUMN reward_sats    INTEGER",
    "ALTER TABLE blocks_found ADD COLUMN fee_sats       INTEGER",
    /* Rows written before this column existed keep 0. They are not
     * retroactively creditable — the rate in force when they were accepted
     * is not recoverable — so an audit spanning the upgrade must fall back
     * to pps_credits for the earlier period. pool_meta.credited_from marks
     * the boundary. */
    "ALTER TABLE shares       ADD COLUMN credited_sats  INTEGER NOT NULL DEFAULT 0",
    /* Rows predating this column keep 0, which the audit reports as
     * "unverifiable" rather than "wrong": their credited_sats is still the
     * authoritative amount, there is simply no stored multiplicand to check
     * it against. rate_history (created by SCHEMA_SQL above) likewise only
     * covers rates published after the upgrade. */
    "ALTER TABLE shares       ADD COLUMN rate_used      REAL NOT NULL DEFAULT 0",
    /* Template rows used to be append-only per material change, where
     * "material" included the block value — which moves on nearly every
     * mempool tick. These two turn each row into a span: `ts` stays first-seen
     * and `last_seen`/`polls` record how many polls collapsed into it. Rows
     * predating the columns are each a single observation, so backfilling
     * last_seen from ts is exact, not a guess. */
    "ALTER TABLE templates    ADD COLUMN last_seen      INTEGER NOT NULL DEFAULT 0",
    "ALTER TABLE templates    ADD COLUMN polls          INTEGER NOT NULL DEFAULT 1",
    "UPDATE templates SET last_seen = ts WHERE last_seen = 0",
    /* See the pool_meta comment above: without this the counter added in
     * PR #32 is only ever readable at shutdown. */
    "ALTER TABLE pool_meta    ADD COLUMN events_lost    INTEGER NOT NULL DEFAULT 0",
    /* Proportional / PPLNS deferred-claim ledger, added alongside
     * pool_mode=proportional. The earlier prop_balances table held sats, a model
     * that could not be settled inside a single coinbase; it never ran anywhere,
     * so it is dropped rather than migrated. */
    "DROP TABLE IF EXISTS prop_balances",
    "CREATE TABLE IF NOT EXISTS prop_ledger ("
    "  address         TEXT PRIMARY KEY,"
    "  claim_fraction  REAL NOT NULL DEFAULT 0,"
    "  last_settled_ts INTEGER"
    ")",
    "CREATE INDEX IF NOT EXISTS prop_ledger_ts_idx ON prop_ledger(last_settled_ts)",
};

/* Retries for one batch. busy_timeout (5s) bounds each attempt, so the worst
 * case is a long stall rather than a fast loop — which is the right trade:
 * enqueue-side overflow is counted in shares_dropped and visible, whereas a
 * dropped batch here is credited work vanishing. */
#define STORE_COMMIT_ATTEMPTS 3

#define EV_SHARE   1
#define EV_REJECT  2
#define EV_BLOCK   3
#define EV_CREDIT  4

#define WORKER_NAME_MAX 128
#define HASH_STR_MAX    96
#define REASON_MAX      128
#define ADDR_MAX        128

#define WORKER_CACHE_SLOTS 16384

typedef struct {
    uint8_t  kind;
    uint64_t ts_ms;
    double   difficulty;
    int      is_block;
    int      height;
    int64_t  reward_sats;       /* EV_BLOCK only */
    int64_t  fee_sats;          /* EV_BLOCK only */
    int64_t  delta_sats;        /* EV_CREDIT only */
    double   rate_used;         /* EV_SHARE only: multiplicand for delta_sats */
    char     worker_name[WORKER_NAME_MAX];
    char     payout_address[ADDR_MAX];   /* EV_SHARE, EV_BLOCK, EV_CREDIT: may be empty */
    char     hash[HASH_STR_MAX];
    char     reason[REASON_MAX];
} event_t;

typedef struct {
    char    name[WORKER_NAME_MAX];
    int64_t id;
    int     used;
} worker_slot_t;

struct store {
    sqlite3 *db;

    sqlite3_stmt *st_upsert_worker;
    sqlite3_stmt *st_get_worker;
    sqlite3_stmt *st_insert_share;
    sqlite3_stmt *st_insert_reject;
    sqlite3_stmt *st_insert_block;
    sqlite3_stmt *st_upsert_node_tip;
    sqlite3_stmt *st_upsert_credit;
    pthread_mutex_t node_tip_mu;   /* serialise binds on st_upsert_node_tip */

    /* Ring buffer */
    event_t  *ring;
    size_t    ring_cap;
    size_t    ring_head;     /* write index */
    size_t    ring_tail;     /* read index */
    size_t    ring_count;

    pthread_mutex_t mu;
    pthread_cond_t  cv_not_empty;
    pthread_cond_t  cv_drained;     /* signaled when queue empties */

    pthread_t writer;
    int       writer_started;
    int       stop;

    int commit_window_ms;
    int commit_max_shares;
    int templates_retention_days;   /* 0 = keep every row */

    worker_slot_t cache[WORKER_CACHE_SLOTS];

    _Atomic uint64_t shares_queued;
    _Atomic uint64_t shares_committed;
    _Atomic uint64_t shares_dropped;
    _Atomic uint64_t rejects_queued;
    _Atomic uint64_t rejects_committed;
    _Atomic uint64_t blocks_committed;
    _Atomic uint64_t credits_committed;
    _Atomic uint64_t batches;
    _Atomic uint64_t pg_errors;
    _Atomic uint64_t events_lost;

    /* Sequence: monotonically increasing counter of enqueued events.
     * 'committed_seq' tracks the highest sequence that has been
     * persisted. flush() waits for committed_seq >= a snapshot of
     * enqueue_seq taken at flush() entry. */
    uint64_t enqueue_seq;
    uint64_t committed_seq;
    pthread_cond_t cv_committed;
};

static size_t g_test_ring_cap = 0;

void store_test_set_ring_capacity(size_t cap) { g_test_ring_cap = cap; }

/* ---- helpers ---------------------------------------------------------- */

/* Linear backoff between commit attempts: 25ms, 50ms, ... */
static void backoff_sleep(int attempt) {
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 25L * 1000000L * attempt };
    nanosleep(&ts, NULL);
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000);
}

static uint32_t name_hash(const char *s) {
    /* FNV-1a */
    uint32_t h = 2166136261u;
    for (; *s; ++s) {
        h ^= (uint8_t)*s;
        h *= 16777619u;
    }
    return h;
}

static int64_t cache_lookup(store_t *s, const char *name) {
    uint32_t h = name_hash(name) & (WORKER_CACHE_SLOTS - 1);
    for (size_t i = 0; i < WORKER_CACHE_SLOTS; ++i) {
        size_t idx = (h + i) & (WORKER_CACHE_SLOTS - 1);
        if (!s->cache[idx].used) return -1;
        if (strncmp(s->cache[idx].name, name, WORKER_NAME_MAX) == 0)
            return s->cache[idx].id;
    }
    return -1;
}

static void cache_insert(store_t *s, const char *name, int64_t id) {
    uint32_t h = name_hash(name) & (WORKER_CACHE_SLOTS - 1);
    for (size_t i = 0; i < WORKER_CACHE_SLOTS; ++i) {
        size_t idx = (h + i) & (WORKER_CACHE_SLOTS - 1);
        if (!s->cache[idx].used) {
            s->cache[idx].used = 1;
            strncpy(s->cache[idx].name, name, WORKER_NAME_MAX - 1);
            s->cache[idx].name[WORKER_NAME_MAX - 1] = '\0';
            s->cache[idx].id = id;
            return;
        }
        if (strncmp(s->cache[idx].name, name, WORKER_NAME_MAX) == 0) {
            s->cache[idx].id = id;
            return;
        }
    }
    /* full - silently drop; future lookups go to DB */
}

static int64_t resolve_worker_id(store_t *s, const char *name,
                                 const char *payout_address,
                                 uint64_t ts_ms) {
    int64_t id = cache_lookup(s, name);
    int cached = (id >= 0);

    /* Schema stores Unix seconds; callers pass milliseconds. */
    const sqlite3_int64 ts_s = (sqlite3_int64)(ts_ms / 1000);

    sqlite3_reset(s->st_upsert_worker);
    sqlite3_clear_bindings(s->st_upsert_worker);
    sqlite3_bind_text(s->st_upsert_worker, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s->st_upsert_worker, 2, ts_s);
    sqlite3_bind_int64(s->st_upsert_worker, 3, ts_s);
    if (payout_address && payout_address[0]) {
        sqlite3_bind_text(s->st_upsert_worker, 4, payout_address, -1,
                          SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(s->st_upsert_worker, 4);
    }
    int rc = sqlite3_step(s->st_upsert_worker);
    if (rc == SQLITE_ROW) {
        id = sqlite3_column_int64(s->st_upsert_worker, 0);
    } else if (!cached) {
        atomic_fetch_add(&s->pg_errors, 1);
        id = -1;
    }
    sqlite3_reset(s->st_upsert_worker);
    if (id >= 0 && !cached) cache_insert(s, name, id);
    return id;
}

/* ---- writer thread ---------------------------------------------------- */

static void process_event(store_t *s, const event_t *ev) {
    if (ev->kind == EV_SHARE) {
        int64_t wid = resolve_worker_id(s, ev->worker_name,
                                        ev->payout_address, ev->ts_ms);
        if (wid < 0) {
            atomic_fetch_add(&s->pg_errors, 1);
            return;
        }
        sqlite3_reset(s->st_insert_share);
        sqlite3_clear_bindings(s->st_insert_share);
        sqlite3_bind_int64(s->st_insert_share, 1, wid);
        sqlite3_bind_int64(s->st_insert_share, 2, (sqlite3_int64)(ev->ts_ms / 1000));
        sqlite3_bind_double(s->st_insert_share, 3, ev->difficulty);
        sqlite3_bind_int(s->st_insert_share, 4, ev->is_block);
        if (ev->hash[0])
            sqlite3_bind_text(s->st_insert_share, 5, ev->hash, -1, SQLITE_TRANSIENT);
        else
            sqlite3_bind_null(s->st_insert_share, 5);
        /* What this share was credited, at the rate in force when it was
         * accepted, and the rate itself. 0 in solo mode. Both come from the
         * same computation in the caller, so the pair is always internally
         * consistent — see the shares schema comment. */
        sqlite3_bind_int64 (s->st_insert_share, 6, ev->delta_sats);
        sqlite3_bind_double(s->st_insert_share, 7, ev->rate_used);
        if (sqlite3_step(s->st_insert_share) != SQLITE_DONE) {
            atomic_fetch_add(&s->pg_errors, 1);
        } else {
            atomic_fetch_add(&s->shares_committed, 1);
        }
        sqlite3_reset(s->st_insert_share);
    } else if (ev->kind == EV_REJECT) {
        sqlite3_reset(s->st_insert_reject);
        sqlite3_clear_bindings(s->st_insert_reject);
        if (ev->worker_name[0])
            sqlite3_bind_text(s->st_insert_reject, 1, ev->worker_name, -1, SQLITE_TRANSIENT);
        else
            sqlite3_bind_null(s->st_insert_reject, 1);
        sqlite3_bind_int64(s->st_insert_reject, 2, (sqlite3_int64)(ev->ts_ms / 1000));
        sqlite3_bind_text(s->st_insert_reject, 3, ev->reason, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(s->st_insert_reject) != SQLITE_DONE) {
            atomic_fetch_add(&s->pg_errors, 1);
        } else {
            atomic_fetch_add(&s->rejects_committed, 1);
        }
        sqlite3_reset(s->st_insert_reject);
    } else if (ev->kind == EV_BLOCK) {
        int64_t finder = -1;
        if (ev->worker_name[0])
            finder = resolve_worker_id(s, ev->worker_name,
                                       ev->payout_address, ev->ts_ms);
        sqlite3_reset(s->st_insert_block);
        sqlite3_clear_bindings(s->st_insert_block);
        sqlite3_bind_int64(s->st_insert_block, 1, (sqlite3_int64)(ev->ts_ms / 1000));
        sqlite3_bind_int(s->st_insert_block, 2, ev->height);
        sqlite3_bind_text(s->st_insert_block, 3, ev->hash, -1, SQLITE_TRANSIENT);
        if (finder >= 0)
            sqlite3_bind_int64(s->st_insert_block, 4, finder);
        else
            sqlite3_bind_null(s->st_insert_block, 4);
        if (ev->payout_address[0])
            sqlite3_bind_text(s->st_insert_block, 5, ev->payout_address, -1,
                              SQLITE_TRANSIENT);
        else
            sqlite3_bind_null(s->st_insert_block, 5);
        if (ev->reward_sats > 0)
            sqlite3_bind_int64(s->st_insert_block, 6, ev->reward_sats);
        else
            sqlite3_bind_null(s->st_insert_block, 6);
        if (ev->fee_sats > 0)
            sqlite3_bind_int64(s->st_insert_block, 7, ev->fee_sats);
        else
            sqlite3_bind_null(s->st_insert_block, 7);
        if (sqlite3_step(s->st_insert_block) != SQLITE_DONE) {
            atomic_fetch_add(&s->pg_errors, 1);
        } else {
            atomic_fetch_add(&s->blocks_committed, 1);
        }
        sqlite3_reset(s->st_insert_block);
    } else if (ev->kind == EV_CREDIT) {
        int64_t wid = resolve_worker_id(s, ev->worker_name,
                                        ev->payout_address, ev->ts_ms);
        if (wid < 0) {
            atomic_fetch_add(&s->pg_errors, 1);
            return;
        }
        sqlite3_reset(s->st_upsert_credit);
        sqlite3_clear_bindings(s->st_upsert_credit);
        sqlite3_bind_int64(s->st_upsert_credit, 1, wid);
        sqlite3_bind_int64(s->st_upsert_credit, 2, ev->delta_sats);
        sqlite3_bind_int64(s->st_upsert_credit, 3, (sqlite3_int64)(ev->ts_ms / 1000));
        if (sqlite3_step(s->st_upsert_credit) != SQLITE_DONE) {
            atomic_fetch_add(&s->pg_errors, 1);
        } else {
            atomic_fetch_add(&s->credits_committed, 1);
        }
        sqlite3_reset(s->st_upsert_credit);
    }
}

/* Attempts to land one batch. The events are already out of the ring, so a
 * failure here destroys accepted work — retry rather than count and move on.
 *
 * busy_timeout already makes SQLITE_BUSY rare; these attempts cover a lock
 * held longer than that, and an I/O error that clears. Counters advance only
 * on the attempt that actually commits, so a retried batch is counted once.
 * Returns 0 committed, -1 out of attempts. */
static int commit_batch(store_t *s, event_t *batch, size_t take) {
    for (int attempt = 1; attempt <= STORE_COMMIT_ATTEMPTS; ++attempt) {
        char *err = NULL;
        if (sqlite3_exec(s->db, "BEGIN IMMEDIATE", NULL, NULL, &err) != SQLITE_OK) {
            LOG_WARN("store: BEGIN failed (attempt %d/%d): %s",
                     attempt, STORE_COMMIT_ATTEMPTS, err ? err : "?");
            sqlite3_free(err);
            atomic_fetch_add(&s->pg_errors, 1);
            backoff_sleep(attempt);
            continue;
        }

        for (size_t i = 0; i < take; ++i) process_event(s, &batch[i]);

        if (sqlite3_exec(s->db, "COMMIT", NULL, NULL, &err) == SQLITE_OK) {
            atomic_fetch_add(&s->batches, 1);
            return 0;
        }
        LOG_WARN("store: COMMIT failed (attempt %d/%d): %s",
                 attempt, STORE_COMMIT_ATTEMPTS, err ? err : "?");
        sqlite3_free(err);
        /* Nothing was durably written, so replaying the batch is safe. The
         * per-event counters process_event() bumped are lost accuracy we
         * accept: they describe attempts, the ledger describes reality. */
        sqlite3_exec(s->db, "ROLLBACK", NULL, NULL, NULL);
        atomic_fetch_add(&s->pg_errors, 1);
        backoff_sleep(attempt);
    }
    return -1;
}

static void *writer_main(void *arg) {
    store_t *s = (store_t *)arg;

    event_t *batch = malloc(sizeof(event_t) * (size_t)s->commit_max_shares);
    if (!batch) {
        LOG_ERROR("store: writer batch alloc failed");
        return NULL;
    }

    pthread_mutex_lock(&s->mu);
    while (1) {
        /* Wait until: stop OR ring has events */
        while (!s->stop && s->ring_count == 0) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            int wms = s->commit_window_ms;
            ts.tv_sec += wms / 1000;
            ts.tv_nsec += (long)(wms % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec += 1;
                ts.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&s->cv_not_empty, &s->mu, &ts);
            if (s->stop || s->ring_count > 0) break;
            /* timed out idle; loop */
            if (s->ring_count == 0) break;
        }

        if (s->stop && s->ring_count == 0) break;

        /* Drain up to commit_max_shares */
        size_t take = s->ring_count;
        if (take > (size_t)s->commit_max_shares) take = (size_t)s->commit_max_shares;
        if (take == 0) continue;

        for (size_t i = 0; i < take; ++i) {
            batch[i] = s->ring[s->ring_tail];
            s->ring_tail = (s->ring_tail + 1) % s->ring_cap;
        }
        s->ring_count -= take;
        uint64_t seq_after = s->enqueue_seq - (uint64_t)s->ring_count;
        pthread_mutex_unlock(&s->mu);

        /* BEGIN/COMMIT outside the producer mutex */
        if (commit_batch(s, batch, take) != 0) {
            /* Out of retries. These events left the ring before the
             * transaction opened and cannot be put back, so say so plainly —
             * this is accepted work that will never be credited, not a
             * transient blip. */
            LOG_ERROR("store: LOST %zu event(s) after %d failed commit attempts"
                      " — accepted shares in this batch are not credited",
                      take, STORE_COMMIT_ATTEMPTS);
            atomic_fetch_add(&s->events_lost, (uint64_t)take);
        }

        pthread_mutex_lock(&s->mu);
        s->committed_seq = seq_after;
        pthread_cond_broadcast(&s->cv_committed);
        if (s->ring_count == 0) pthread_cond_broadcast(&s->cv_drained);
    }
    pthread_mutex_unlock(&s->mu);

    free(batch);
    return NULL;
}

/* ---- enqueue ---------------------------------------------------------- */

static int enqueue(store_t *s, const event_t *ev) {
    pthread_mutex_lock(&s->mu);
    if (s->ring_count == s->ring_cap) {
        pthread_mutex_unlock(&s->mu);
        return -1;
    }
    s->ring[s->ring_head] = *ev;
    s->ring_head = (s->ring_head + 1) % s->ring_cap;
    s->ring_count++;
    s->enqueue_seq++;
    pthread_cond_signal(&s->cv_not_empty);
    pthread_mutex_unlock(&s->mu);
    return 0;
}

/* ---- public API ------------------------------------------------------- */

int store_open(const store_cfg_t *cfg, store_t **out) {
    if (!cfg || !out) return -1;
    store_t *s = calloc(1, sizeof(*s));
    if (!s) return -1;

    s->commit_window_ms = cfg->commit_window_ms > 0 ? cfg->commit_window_ms : 100;
    s->commit_max_shares = cfg->commit_max_shares > 0 ? cfg->commit_max_shares : 100;
    s->templates_retention_days =
        cfg->templates_retention_days > 0 ? cfg->templates_retention_days : 0;
    s->ring_cap = g_test_ring_cap > 0 ? g_test_ring_cap : 65536;
    s->ring = calloc(s->ring_cap, sizeof(event_t));
    if (!s->ring) { free(s); return -1; }

    pthread_mutex_init(&s->mu, NULL);
    pthread_cond_init(&s->cv_not_empty, NULL);
    pthread_cond_init(&s->cv_drained, NULL);
    pthread_cond_init(&s->cv_committed, NULL);

    int rc = sqlite3_open_v2(cfg->path, &s->db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        NULL);
    if (rc != SQLITE_OK) {
        LOG_ERROR("store: sqlite3_open(%s) failed: %s", cfg->path,
                  s->db ? sqlite3_errmsg(s->db) : "?");
        if (s->db) sqlite3_close(s->db);
        free(s->ring); free(s);
        return -2;
    }

    char *err = NULL;
    for (size_t i = 0; i < sizeof(SCHEMA_SQL_PARTS) / sizeof(SCHEMA_SQL_PARTS[0]); ++i) {
        err = NULL;
        if (sqlite3_exec(s->db, SCHEMA_SQL_PARTS[i], NULL, NULL, &err) != SQLITE_OK) {
            LOG_ERROR("store: schema apply (part %zu) failed: %s",
                      i + 1, err ? err : "?");
            sqlite3_free(err);
            sqlite3_close(s->db);
            free(s->ring); free(s);
            return -3;
        }
        sqlite3_free(err);
    }
    /* Best-effort migrations for DBs created by an older simplepool. Each
     * ALTER returns "duplicate column" on already-migrated DBs, which is
     * expected — only log other failures. */
    for (size_t i = 0; i < sizeof(MIGRATIONS_SQL) / sizeof(MIGRATIONS_SQL[0]); ++i) {
        err = NULL;
        if (sqlite3_exec(s->db, MIGRATIONS_SQL[i], NULL, NULL, &err) != SQLITE_OK) {
            if (err && !strstr(err, "duplicate column")) {
                LOG_WARN("store: migration '%s' failed: %s",
                         MIGRATIONS_SQL[i], err);
            }
            sqlite3_free(err);
        }
    }

    /* Prepared statements. The workers upsert sets payout_address on first
     * INSERT only — once set, it is immutable for that worker name. */
    static const char *Q_UPSERT =
        "INSERT INTO workers (name, first_seen, last_seen, payout_address) "
        "VALUES (?, ?, ?, ?) "
        "ON CONFLICT(name) DO UPDATE SET "
        "  last_seen      = excluded.last_seen, "
        "  payout_address = COALESCE(workers.payout_address, excluded.payout_address) "
        "RETURNING id";
    static const char *Q_INS_SHARE =
        "INSERT INTO shares "
        "  (worker_id, ts, difficulty, is_block, block_hash, credited_sats, rate_used) "
        "VALUES (?, ?, ?, ?, ?, ?, ?)";
    static const char *Q_INS_REJECT =
        "INSERT INTO rejects (worker_name, ts, reason) VALUES (?, ?, ?)";
    static const char *Q_INS_BLOCK =
        "INSERT INTO blocks_found "
        "  (ts, height, hash, finder_id, finder_address, reward_sats, fee_sats) "
        "VALUES (?, ?, ?, ?, ?, ?, ?)";
    /* Single-row upsert keyed on id=1. tip_observed_at is only set when
     * the tip actually changes (height or hash differ from the stored
     * row), so 'time since last tip change' stays meaningful across
     * repeated polls of the same tip. */
    static const char *Q_UPSERT_NODE_TIP =
        "INSERT INTO node_status (id, tip_height, tip_hash, tip_observed_at, updated_at) "
        "VALUES (1, ?, ?, ?, ?) "
        "ON CONFLICT(id) DO UPDATE SET "
        "  tip_height = excluded.tip_height, "
        "  tip_hash   = excluded.tip_hash, "
        "  tip_observed_at = CASE "
        "    WHEN node_status.tip_hash IS NULL OR node_status.tip_hash != excluded.tip_hash "
        "      THEN excluded.tip_observed_at "
        "    ELSE node_status.tip_observed_at "
        "  END, "
        "  updated_at = excluded.updated_at";
    /* PPS credit: increment accrued_sats for this worker_id. The downstream
     * payout worker reads (accrued_sats - paid_sats) and updates paid_sats
     * after a successful Thunder tx. */
    static const char *Q_UPSERT_CREDIT =
        "INSERT INTO pps_credits (worker_id, accrued_sats, paid_sats, last_updated) "
        "VALUES (?, ?, 0, ?) "
        "ON CONFLICT(worker_id) DO UPDATE SET "
        "  accrued_sats = pps_credits.accrued_sats + excluded.accrued_sats, "
        "  last_updated = excluded.last_updated";

    pthread_mutex_init(&s->node_tip_mu, NULL);

    if (sqlite3_prepare_v2(s->db, Q_UPSERT, -1, &s->st_upsert_worker, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(s->db, Q_INS_SHARE, -1, &s->st_insert_share, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(s->db, Q_INS_REJECT, -1, &s->st_insert_reject, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(s->db, Q_INS_BLOCK, -1, &s->st_insert_block, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(s->db, Q_UPSERT_NODE_TIP, -1, &s->st_upsert_node_tip, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(s->db, Q_UPSERT_CREDIT, -1, &s->st_upsert_credit, NULL) != SQLITE_OK)
    {
        LOG_ERROR("store: prepare failed: %s", sqlite3_errmsg(s->db));
        store_close(s);
        return -4;
    }

    if (pthread_create(&s->writer, NULL, writer_main, s) != 0) {
        LOG_ERROR("store: pthread_create failed: %s", strerror(errno));
        store_close(s);
        return -5;
    }
    s->writer_started = 1;

    LOG_INFO("store: opened %s (ring=%zu, window=%dms, batch=%d)",
             cfg->path, s->ring_cap, s->commit_window_ms, s->commit_max_shares);
    *out = s;
    return 0;
}

void store_close(store_t *s) {
    if (!s) return;
    if (s->writer_started) {
        pthread_mutex_lock(&s->mu);
        s->stop = 1;
        pthread_cond_broadcast(&s->cv_not_empty);
        pthread_mutex_unlock(&s->mu);
        pthread_join(s->writer, NULL);
    }
    if (s->st_upsert_worker) sqlite3_finalize(s->st_upsert_worker);
    if (s->st_get_worker)    sqlite3_finalize(s->st_get_worker);
    if (s->st_insert_share)  sqlite3_finalize(s->st_insert_share);
    if (s->st_insert_reject) sqlite3_finalize(s->st_insert_reject);
    if (s->st_insert_block)  sqlite3_finalize(s->st_insert_block);
    if (s->st_upsert_node_tip) sqlite3_finalize(s->st_upsert_node_tip);
    if (s->st_upsert_credit) sqlite3_finalize(s->st_upsert_credit);
    if (s->db) sqlite3_close(s->db);
    pthread_mutex_destroy(&s->node_tip_mu);
    pthread_mutex_destroy(&s->mu);
    pthread_cond_destroy(&s->cv_not_empty);
    pthread_cond_destroy(&s->cv_drained);
    pthread_cond_destroy(&s->cv_committed);
    free(s->ring);
    free(s);
}

int store_record_share(store_t *s, const char *worker_name,
                       uint64_t ts_ms, double difficulty,
                       int is_block, const char *share_hash_or_null)
{
    return store_record_share_addr(s, worker_name, NULL, ts_ms, difficulty,
                                   is_block, share_hash_or_null, 0, 0.0);
}

int store_record_share_addr(store_t *s, const char *worker_name,
                            const char *payout_address,
                            uint64_t ts_ms, double difficulty,
                            int is_block, const char *share_hash_or_null,
                            int64_t credited_sats, double rate_used)
{
    if (!s || !worker_name) return -1;
    event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = EV_SHARE;
    ev.ts_ms = ts_ms;
    ev.difficulty = difficulty;
    ev.is_block = is_block;
    ev.delta_sats = credited_sats;
    ev.rate_used = rate_used;
    strncpy(ev.worker_name, worker_name, WORKER_NAME_MAX - 1);
    if (payout_address)
        strncpy(ev.payout_address, payout_address, ADDR_MAX - 1);
    if (share_hash_or_null) {
        strncpy(ev.hash, share_hash_or_null, HASH_STR_MAX - 1);
    }
    if (enqueue(s, &ev) != 0) {
        atomic_fetch_add(&s->shares_dropped, 1);
        return -1;
    }
    atomic_fetch_add(&s->shares_queued, 1);
    return 0;
}

int store_record_reject(store_t *s, const char *worker_name,
                        uint64_t ts_ms, const char *reason)
{
    if (!s || !reason) return -1;
    event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = EV_REJECT;
    ev.ts_ms = ts_ms;
    if (worker_name) strncpy(ev.worker_name, worker_name, WORKER_NAME_MAX - 1);
    strncpy(ev.reason, reason, REASON_MAX - 1);
    if (enqueue(s, &ev) != 0) {
        atomic_fetch_add(&s->shares_dropped, 1);
        return -1;
    }
    atomic_fetch_add(&s->rejects_queued, 1);
    return 0;
}

int store_record_block(store_t *s, uint64_t ts_ms, int height,
                       const char *hash, const char *finder_name,
                       const char *finder_address,
                       int64_t reward_sats, int64_t fee_sats)
{
    if (!s || !hash) return -1;
    event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = EV_BLOCK;
    ev.ts_ms = ts_ms;
    ev.height = height;
    ev.reward_sats = reward_sats;
    ev.fee_sats = fee_sats;
    strncpy(ev.hash, hash, HASH_STR_MAX - 1);
    if (finder_name) strncpy(ev.worker_name, finder_name, WORKER_NAME_MAX - 1);
    if (finder_address)
        strncpy(ev.payout_address, finder_address, ADDR_MAX - 1);
    if (enqueue(s, &ev) != 0) {
        atomic_fetch_add(&s->shares_dropped, 1);
        return -1;
    }
    return 0;
}

int store_record_credit(store_t *s, const char *worker_name,
                        const char *payout_address,
                        uint64_t ts_ms, int64_t delta_sats)
{
    if (!s || !worker_name || delta_sats <= 0) return -1;
    event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = EV_CREDIT;
    ev.ts_ms = ts_ms;
    ev.delta_sats = delta_sats;
    strncpy(ev.worker_name, worker_name, WORKER_NAME_MAX - 1);
    if (payout_address)
        strncpy(ev.payout_address, payout_address, ADDR_MAX - 1);
    if (enqueue(s, &ev) != 0) {
        atomic_fetch_add(&s->shares_dropped, 1);
        return -1;
    }
    return 0;
}

int store_flush(store_t *s) {
    if (!s) return -1;
    uint64_t target;
    pthread_mutex_lock(&s->mu);
    target = s->enqueue_seq;
    pthread_cond_signal(&s->cv_not_empty);

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 5;

    while (s->committed_seq < target) {
        int rc = pthread_cond_timedwait(&s->cv_committed, &s->mu, &deadline);
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&s->mu);
            return -1;
        }
    }
    pthread_mutex_unlock(&s->mu);

    /* Avoid unused-warning suppression */
    (void)now_ms;
    return 0;
}

int store_record_node_tip(store_t *s, int height, const char *hash,
                          uint64_t observed_ts_s, uint64_t updated_ts_s)
{
    if (!s || !hash) return -1;
    pthread_mutex_lock(&s->node_tip_mu);
    sqlite3_reset(s->st_upsert_node_tip);
    sqlite3_clear_bindings(s->st_upsert_node_tip);
    sqlite3_bind_int (s->st_upsert_node_tip, 1, height);
    sqlite3_bind_text(s->st_upsert_node_tip, 2, hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s->st_upsert_node_tip, 3, (sqlite3_int64)observed_ts_s);
    sqlite3_bind_int64(s->st_upsert_node_tip, 4, (sqlite3_int64)updated_ts_s);
    int rc = sqlite3_step(s->st_upsert_node_tip);
    sqlite3_reset(s->st_upsert_node_tip);
    pthread_mutex_unlock(&s->node_tip_mu);
    if (rc != SQLITE_DONE) {
        atomic_fetch_add(&s->pg_errors, 1);
        return -2;
    }
    return 0;
}

int store_record_pool_meta(store_t *s, const char *pool_mode, int fee_bps,
                           const char *rate_source,
                           double rate_sats_per_diff,
                           double gross_sats_per_diff,
                           double effective_fee_bps,
                           double network_difficulty,
                           int64_t block_value_sats,
                           uint64_t updated_ts_s)
{
    if (!s) return -1;
    /* Prepared ad-hoc rather than cached: this runs once per template
     * change, so the prepare cost is irrelevant and it keeps the hot
     * writer-thread statement set untouched.
     *
     * credited_from is stamped on first write and never overwritten. It
     * marks where shares.credited_sats becomes trustworthy, so an audit
     * spanning the upgrade can tell which period it may sum directly. */
    static const char *Q =
        "INSERT INTO pool_meta (id, pool_mode, fee_bps, rate_source,"
        "  rate_sats_per_diff, gross_sats_per_diff, effective_fee_bps,"
        "  network_difficulty, block_value_sats, credited_from, updated_at,"
        "  events_lost) "
        "VALUES (1, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(id) DO UPDATE SET "
        "  pool_mode = excluded.pool_mode,"
        "  fee_bps = excluded.fee_bps,"
        "  rate_source = excluded.rate_source,"
        "  rate_sats_per_diff = excluded.rate_sats_per_diff,"
        "  gross_sats_per_diff = excluded.gross_sats_per_diff,"
        "  effective_fee_bps = excluded.effective_fee_bps,"
        "  network_difficulty = excluded.network_difficulty,"
        "  block_value_sats = excluded.block_value_sats,"
        "  credited_from = COALESCE(pool_meta.credited_from, excluded.credited_from),"
        "  updated_at = excluded.updated_at,"
        "  events_lost = excluded.events_lost";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(s->db, Q, -1, &st, NULL) != SQLITE_OK) {
        atomic_fetch_add(&s->pg_errors, 1);
        return -2;
    }
    pthread_mutex_lock(&s->node_tip_mu);
    sqlite3_bind_text  (st, 1, pool_mode   ? pool_mode   : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int   (st, 2, fee_bps);
    sqlite3_bind_text  (st, 3, rate_source ? rate_source : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(st, 4, rate_sats_per_diff);
    sqlite3_bind_double(st, 5, gross_sats_per_diff);
    sqlite3_bind_double(st, 6, effective_fee_bps);
    sqlite3_bind_double(st, 7, network_difficulty);
    sqlite3_bind_int64 (st, 8, (sqlite3_int64)block_value_sats);
    sqlite3_bind_int64 (st, 9, (sqlite3_int64)updated_ts_s);
    sqlite3_bind_int64 (st, 10, (sqlite3_int64)updated_ts_s);
    sqlite3_bind_int64 (st, 11, (sqlite3_int64)atomic_load(&s->events_lost));
    int rc = sqlite3_step(st);
    pthread_mutex_unlock(&s->node_tip_mu);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        atomic_fetch_add(&s->pg_errors, 1);
        return -2;
    }
    return 0;
}

int store_record_rate(store_t *s, const char *rate_source,
                      double rate_sats_per_diff,
                      double gross_sats_per_diff,
                      int fee_bps,
                      double network_difficulty,
                      int64_t block_value_sats,
                      uint64_t ts_s)
{
    if (!s) return -1;

    /* Append only when something actually moved. Compared bitwise against
     * the newest row rather than with a tolerance: rate_used on the share
     * rows is the same double, so an exact match is what makes the
     * "every rate a share used appears in this log" check work. On a chain
     * with a busy mempool the block value shifts every template and this
     * appends about that often; on a quiet one it barely grows. */
    static const char *Q_LAST =
        "SELECT rate_sats_per_diff, gross_sats_per_diff, fee_bps,"
        "       network_difficulty, block_value_sats, rate_source"
        "  FROM rate_history ORDER BY id DESC LIMIT 1";
    sqlite3_stmt *last = NULL;
    int unchanged = 0;
    pthread_mutex_lock(&s->node_tip_mu);
    if (sqlite3_prepare_v2(s->db, Q_LAST, -1, &last, NULL) == SQLITE_OK &&
        sqlite3_step(last) == SQLITE_ROW)
    {
        const unsigned char *src = sqlite3_column_text(last, 5);
        unchanged =
            sqlite3_column_double(last, 0) == rate_sats_per_diff  &&
            sqlite3_column_double(last, 1) == gross_sats_per_diff &&
            sqlite3_column_int   (last, 2) == fee_bps             &&
            sqlite3_column_double(last, 3) == network_difficulty  &&
            sqlite3_column_int64 (last, 4) == (sqlite3_int64)block_value_sats &&
            src && rate_source && strcmp((const char *)src, rate_source) == 0;
    }
    sqlite3_finalize(last);
    if (unchanged) {
        pthread_mutex_unlock(&s->node_tip_mu);
        return 0;
    }

    static const char *Q_INS =
        "INSERT INTO rate_history (ts, rate_sats_per_diff, gross_sats_per_diff,"
        "  fee_bps, network_difficulty, block_value_sats, rate_source) "
        "VALUES (?, ?, ?, ?, ?, ?, ?)";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(s->db, Q_INS, -1, &st, NULL) != SQLITE_OK) {
        pthread_mutex_unlock(&s->node_tip_mu);
        atomic_fetch_add(&s->pg_errors, 1);
        return -2;
    }
    sqlite3_bind_int64 (st, 1, (sqlite3_int64)ts_s);
    sqlite3_bind_double(st, 2, rate_sats_per_diff);
    sqlite3_bind_double(st, 3, gross_sats_per_diff);
    sqlite3_bind_int   (st, 4, fee_bps);
    sqlite3_bind_double(st, 5, network_difficulty);
    sqlite3_bind_int64 (st, 6, (sqlite3_int64)block_value_sats);
    sqlite3_bind_text  (st, 7, rate_source ? rate_source : "", -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    pthread_mutex_unlock(&s->node_tip_mu);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        atomic_fetch_add(&s->pg_errors, 1);
        return -2;
    }
    return 0;
}

int store_record_template(store_t *s, const store_template_t *t) {
    if (!s || !t) return -1;

    /* Open a new row only when the *work* changes: the tip, the nBits, the
     * template source or the shape of the server's coinbase.
     *
     * The block value and transaction count are deliberately NOT in the key.
     * They drift with every mempool tick, so keying on them appended a row
     * per poll at a height already recorded — ~2,880 rows/day here, almost
     * all of it fee churn. A poll that matches the newest row now refreshes
     * that row instead.
     *
     * source / cb_spendable / cb_op_returns stay in the key on purpose: a
     * template that stops carrying the BIP300/301 commitments part-way
     * through a height is precisely the regression the /templates page exists
     * to surface, so it has to open its own row rather than overwrite the
     * good one. */
    static const char *Q_LAST =
        "SELECT id, height, prev_hash, bits, source, cb_spendable, cb_op_returns,"
        "       longpoll"
        "  FROM templates ORDER BY id DESC LIMIT 1";
    sqlite3_stmt *last = NULL;
    int unchanged = 0;
    sqlite3_int64 last_id = 0;
    pthread_mutex_lock(&s->node_tip_mu);
    if (sqlite3_prepare_v2(s->db, Q_LAST, -1, &last, NULL) == SQLITE_OK &&
        sqlite3_step(last) == SQLITE_ROW)
    {
        const unsigned char *ph  = sqlite3_column_text(last, 2);
        const unsigned char *bt  = sqlite3_column_text(last, 3);
        const unsigned char *src = sqlite3_column_text(last, 4);
        last_id = sqlite3_column_int64(last, 0);
        unchanged =
            sqlite3_column_int(last, 1) == t->height &&
            ph  && strcmp((const char *)ph,  t->prev_hash ? t->prev_hash : "") == 0 &&
            bt  && strcmp((const char *)bt,  t->bits      ? t->bits      : "") == 0 &&
            src && strcmp((const char *)src, t->source    ? t->source    : "") == 0 &&
            sqlite3_column_int(last, 5) == t->cb_spendable &&
            sqlite3_column_int(last, 6) == t->cb_op_returns &&
            sqlite3_column_int(last, 7) == (t->longpoll ? 1 : 0);
    }
    sqlite3_finalize(last);

    /* Same work, fresher numbers: fold this poll into the row it belongs to.
     * `ts` stays first-seen so the row remains a span of one template. */
    if (unchanged) {
        static const char *Q_UPD =
            "UPDATE templates SET last_seen = ?, polls = polls + 1,"
            "  network_difficulty = ?, coinbase_value_sats = ?, tx_count = ?,"
            "  tx_fees_sats = ?, rate_sats_per_diff = ? WHERE id = ?";
        sqlite3_stmt *up = NULL;
        if (sqlite3_prepare_v2(s->db, Q_UPD, -1, &up, NULL) != SQLITE_OK) {
            pthread_mutex_unlock(&s->node_tip_mu);
            atomic_fetch_add(&s->pg_errors, 1);
            return -2;
        }
        sqlite3_bind_int64 (up, 1, (sqlite3_int64)t->ts_s);
        sqlite3_bind_double(up, 2, t->network_difficulty);
        sqlite3_bind_int64 (up, 3, (sqlite3_int64)t->coinbase_value_sats);
        sqlite3_bind_int   (up, 4, t->tx_count);
        sqlite3_bind_int64 (up, 5, (sqlite3_int64)t->tx_fees_sats);
        sqlite3_bind_double(up, 6, t->rate_sats_per_diff);
        sqlite3_bind_int64 (up, 7, last_id);
        int urc = sqlite3_step(up);
        pthread_mutex_unlock(&s->node_tip_mu);
        sqlite3_finalize(up);
        if (urc != SQLITE_DONE) {
            atomic_fetch_add(&s->pg_errors, 1);
            return -2;
        }
        return 0;
    }

    static const char *Q_INS =
        "INSERT INTO templates (ts, height, prev_hash, bits, network_difficulty,"
        "  coinbase_value_sats, tx_count, tx_fees_sats, source, cb_spendable,"
        "  cb_op_returns, longpoll, rate_sats_per_diff, last_seen, polls) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 1)";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(s->db, Q_INS, -1, &st, NULL) != SQLITE_OK) {
        pthread_mutex_unlock(&s->node_tip_mu);
        atomic_fetch_add(&s->pg_errors, 1);
        return -2;
    }
    sqlite3_bind_int64 (st,  1, (sqlite3_int64)t->ts_s);
    sqlite3_bind_int   (st,  2, t->height);
    sqlite3_bind_text  (st,  3, t->prev_hash ? t->prev_hash : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text  (st,  4, t->bits      ? t->bits      : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(st,  5, t->network_difficulty);
    sqlite3_bind_int64 (st,  6, (sqlite3_int64)t->coinbase_value_sats);
    sqlite3_bind_int   (st,  7, t->tx_count);
    sqlite3_bind_int64 (st,  8, (sqlite3_int64)t->tx_fees_sats);
    sqlite3_bind_text  (st,  9, t->source    ? t->source    : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int   (st, 10, t->cb_spendable);
    sqlite3_bind_int   (st, 11, t->cb_op_returns);
    sqlite3_bind_int   (st, 12, t->longpoll ? 1 : 0);
    sqlite3_bind_double(st, 13, t->rate_sats_per_diff);
    sqlite3_bind_int64 (st, 14, (sqlite3_int64)t->ts_s);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        pthread_mutex_unlock(&s->node_tip_mu);
        atomic_fetch_add(&s->pg_errors, 1);
        return -2;
    }

    /* Trim history on the way out. Driven off the template's own timestamp
     * rather than wall-clock time so a replay or a test is deterministic.
     * Nothing but the dashboard reads this table, so a dropped row costs
     * visibility and nothing else — the ledger lives in shares/rate_history. */
    int keep_days = s->templates_retention_days;
    if (keep_days > 0) {
        static const char *Q_TRIM = "DELETE FROM templates WHERE ts < ?";
        sqlite3_stmt *tr = NULL;
        if (sqlite3_prepare_v2(s->db, Q_TRIM, -1, &tr, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(tr, 1,
                (sqlite3_int64)t->ts_s - (sqlite3_int64)keep_days * 86400);
            sqlite3_step(tr);
            sqlite3_finalize(tr);
        }
    }
    pthread_mutex_unlock(&s->node_tip_mu);
    return 0;
}

/* ---------- proportional / PPLNS helpers ---------- */

int store_prop_get_ledger(store_t *s, pplns_claim_t **out, size_t *n) {
    if (!s || !out || !n) return -1;
    *out = NULL; *n = 0;

    static const char *Q =
        "SELECT address, claim_fraction FROM prop_ledger"
        "  WHERE claim_fraction != 0";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(s->db, Q, -1, &st, NULL) != SQLITE_OK) {
        atomic_fetch_add(&s->pg_errors, 1);
        return -1;
    }

    size_t cap = 64, count = 0;
    pplns_claim_t *buf = (pplns_claim_t *)calloc(cap, sizeof(*buf));
    if (!buf) { sqlite3_finalize(st); return -1; }

    while (sqlite3_step(st) == SQLITE_ROW) {
        if (count >= cap) {
            size_t ncap = cap * 2;
            pplns_claim_t *nb = (pplns_claim_t *)realloc(buf, ncap * sizeof(*nb));
            if (!nb) { free(buf); sqlite3_finalize(st); return -1; }
            buf = nb; cap = ncap;
        }
        const char *addr = (const char *)sqlite3_column_text(st, 0);
        snprintf(buf[count].address, sizeof buf[count].address, "%s",
                 addr ? addr : "");
        buf[count].claim_fraction = sqlite3_column_double(st, 1);
        count++;
    }
    sqlite3_finalize(st);
    *out = buf;
    *n = count;
    return 0;
}

int store_prop_compute_window(store_t *s, double window_difficulty,
                              uint64_t before_ms,
                              uint64_t *out_start_ms,
                              double *out_actual_difficulty) {
    if (!s || window_difficulty <= 0.0 || !out_start_ms || !out_actual_difficulty)
        return -1;

    /* The shares table stores Unix seconds; callers pass milliseconds. */
    sqlite3_int64 before_s = (sqlite3_int64)(before_ms / 1000);

    /* Walk backwards from before_ms until cumulative difficulty reaches the
     * target. SQLite window functions are clean but we avoid depending on them
     * by walking an ordered result set in C. */
    static const char *Q =
        "SELECT s.ts, s.difficulty FROM shares s"
        "  JOIN workers w ON s.worker_id = w.id"
        "  WHERE s.ts <= ? AND s.is_block = 0"
        "    AND w.payout_address IS NOT NULL AND w.payout_address != ''"
        "  ORDER BY s.ts DESC, s.id DESC"
        "  LIMIT 100000";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(s->db, Q, -1, &st, NULL) != SQLITE_OK) {
        atomic_fetch_add(&s->pg_errors, 1);
        return -1;
    }
    sqlite3_bind_int64(st, 1, before_s);

    double cum = 0.0;
    uint64_t boundary_s = 0;
    int any = 0;
    int reached = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        uint64_t ts = (uint64_t)sqlite3_column_int64(st, 0);
        double diff = sqlite3_column_double(st, 1);
        any = 1;
        if (reached && ts != boundary_s) break;
        cum += diff;
        boundary_s = ts;
        if (cum >= window_difficulty) reached = 1;
    }
    sqlite3_finalize(st);
    if (!any) return -1;

    *out_start_ms = boundary_s * 1000ULL;
    *out_actual_difficulty = cum;
    return 0;
}

int store_prop_window_addrs(store_t *s, uint64_t start_ms, uint64_t end_ms,
                            pplns_addr_t **out, size_t *n) {
    if (!s || !out || !n) return -1;
    *out = NULL; *n = 0;

    sqlite3_int64 start_s = (sqlite3_int64)(start_ms / 1000);
    sqlite3_int64 end_s   = (sqlite3_int64)(end_ms   / 1000);

    static const char *Q =
        "SELECT w.payout_address, SUM(s.difficulty) AS total_diff"
        "  FROM shares s"
        "  JOIN workers w ON s.worker_id = w.id"
        "  WHERE s.ts >= ? AND s.ts <= ? AND s.is_block = 0"
        "    AND w.payout_address IS NOT NULL AND w.payout_address != ''"
        "  GROUP BY w.payout_address"
        "  ORDER BY total_diff DESC";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(s->db, Q, -1, &st, NULL) != SQLITE_OK) {
        atomic_fetch_add(&s->pg_errors, 1);
        return -1;
    }
    sqlite3_bind_int64(st, 1, start_s);
    sqlite3_bind_int64(st, 2, end_s);

    size_t cap = 64, count = 0;
    pplns_addr_t *buf = (pplns_addr_t *)calloc(cap, sizeof(*buf));
    if (!buf) { sqlite3_finalize(st); return -1; }

    while (sqlite3_step(st) == SQLITE_ROW) {
        if (count >= cap) {
            size_t ncap = cap * 2;
            pplns_addr_t *nb = (pplns_addr_t *)realloc(buf, ncap * sizeof(*nb));
            if (!nb) { free(buf); sqlite3_finalize(st); return -1; }
            buf = nb; cap = ncap;
        }
        const char *addr = (const char *)sqlite3_column_text(st, 0);
        snprintf(buf[count].address, sizeof buf[count].address, "%s",
                 addr ? addr : "");
        buf[count].total_difficulty = sqlite3_column_double(st, 1);
        count++;
    }
    sqlite3_finalize(st);
    *out = buf;
    *n = count;
    return 0;
}

int store_prop_settle_block(store_t *s, uint64_t ts_ms, int height,
                            const char *block_hash,
                            const pplns_payout_t *payouts, size_t n_payouts,
                            const pplns_claim_t *ledger, size_t n_ledger) {
    if (!s || !block_hash) return -1;

    sqlite3_stmt *st = NULL;
    int rc;

    /* Replace the ledger wholesale: pplns_compute_payouts returns the complete
     * post-block state, and a claim that has settled to zero is simply absent
     * from it. Merging row by row would leave stale rows behind and break the
     * sums-to-zero invariant. */
    static const char *Q_UPSERT =
        "INSERT INTO prop_ledger (address, claim_fraction, last_settled_ts)"
        "  VALUES (?, ?, ?)"
        "  ON CONFLICT(address) DO UPDATE SET"
        "    claim_fraction = excluded.claim_fraction,"
        "    last_settled_ts = excluded.last_settled_ts";
    static const char *Q_CLEAR = "DELETE FROM prop_ledger";

    sqlite3_exec(s->db, "BEGIN TRANSACTION", NULL, NULL, NULL);

    if (sqlite3_prepare_v2(s->db, Q_CLEAR, -1, &st, NULL) != SQLITE_OK) {
        sqlite3_exec(s->db, "ROLLBACK", NULL, NULL, NULL);
        atomic_fetch_add(&s->pg_errors, 1);
        return -1;
    }
    sqlite3_step(st);
    sqlite3_finalize(st);

    for (size_t i = 0; i < n_ledger; i++) {
        if (ledger[i].claim_fraction == 0.0) continue;
        if (sqlite3_prepare_v2(s->db, Q_UPSERT, -1, &st, NULL) != SQLITE_OK) {
            sqlite3_exec(s->db, "ROLLBACK", NULL, NULL, NULL);
            atomic_fetch_add(&s->pg_errors, 1);
            return -1;
        }
        sqlite3_bind_text  (st, 1, ledger[i].address, -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(st, 2, ledger[i].claim_fraction);
        sqlite3_bind_int64 (st, 3, (sqlite3_int64)(ts_ms / 1000));
        rc = sqlite3_step(st);
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) {
            sqlite3_exec(s->db, "ROLLBACK", NULL, NULL, NULL);
            atomic_fetch_add(&s->pg_errors, 1);
            return -1;
        }
    }

    /* Record the block. */
    static const char *Q_BLOCK =
        "INSERT INTO blocks_found (ts, height, hash, finder_id, finder_address,"
        "  reward_sats, fee_sats) VALUES (?, ?, ?, NULL, ?, ?, ?)";
    if (sqlite3_prepare_v2(s->db, Q_BLOCK, -1, &st, NULL) != SQLITE_OK) {
        sqlite3_exec(s->db, "ROLLBACK", NULL, NULL, NULL);
        atomic_fetch_add(&s->pg_errors, 1);
        return -1;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)(ts_ms / 1000));
    sqlite3_bind_int  (st, 2, height);
    sqlite3_bind_text (st, 3, block_hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 4, n_payouts ? payouts[0].address : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 5, 0);
    sqlite3_bind_int64(st, 6, 0);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        sqlite3_exec(s->db, "ROLLBACK", NULL, NULL, NULL);
        atomic_fetch_add(&s->pg_errors, 1);
        return -1;
    }

    sqlite3_exec(s->db, "COMMIT", NULL, NULL, NULL);
    atomic_fetch_add(&s->blocks_committed, 1);
    return 0;
}

void store_get_stats(store_t *s, store_stats_t *out) {
    if (!s || !out) return;
    out->shares_queued    = atomic_load(&s->shares_queued);
    out->shares_committed = atomic_load(&s->shares_committed);
    out->shares_dropped   = atomic_load(&s->shares_dropped);
    out->rejects_queued   = atomic_load(&s->rejects_queued);
    out->rejects_committed= atomic_load(&s->rejects_committed);
    out->blocks_committed = atomic_load(&s->blocks_committed);
    out->batches          = atomic_load(&s->batches);
    out->pg_errors        = atomic_load(&s->pg_errors);
    out->events_lost      = atomic_load(&s->events_lost);
}
