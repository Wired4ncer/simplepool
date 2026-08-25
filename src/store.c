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
#include <math.h>
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
    "  fee_sats        INTEGER,"
    /* A row is a *candidate* until something says otherwise. Only
     * status='confirmed' means "this pool mined a block that is in the
     * chain" — every count and every solvency sum must filter on it.
     * 'rejected' is a candidate submitblock refused; 'orphaned' one that
     * was accepted and later reorged out; 'pending' one nothing has
     * verified yet, which under a backend that answers only
     * getblocktemplate/submitblock is a normal steady state, not a
     * transient. Pending is never revenue.
     * checked_via records who answered: 'node' (getblockhash) or 'tips'
     * (the observed chain of getblocktemplate prev_hashes), the same
     * distinction pool_meta.network_source draws. */
    "  status          TEXT NOT NULL DEFAULT 'pending',"
    "  confirmations   INTEGER NOT NULL DEFAULT 0,"
    "  submit_error    TEXT,"
    "  checked_via     TEXT"
    ");"
    "CREATE INDEX IF NOT EXISTS blocks_found_ts_idx ON blocks_found(ts);"
    /* ⛔ The status index is NOT here, and must not be moved here.
     *
     * This array is applied STRICTLY — any error fails store_open and the pool
     * does not start. `CREATE TABLE IF NOT EXISTS` is a no-op on a database
     * that already has blocks_found, so on every existing deployment the table
     * still lacks `status` at this point, and indexing a column that does not
     * exist yet fails with "no such column: status". The ALTER that adds it
     * lives in the MIGRATIONS array below, which is error-tolerant — and which
     * never runs, because this section aborts first.
     *
     * That took the production pool down on 2026-08-25: three crash-loop
     * restarts, ~3 minutes with no listener, reverted. ⚠️ Every test passed,
     * because tests build a FRESH database where the CREATE TABLE above does
     * include `status`. The failure needs a pre-existing table without it —
     * i.e. only production. See test_store_opens_a_pre_status_database.
     *
     * The migrations array creates this index, which covers both cases:
     * migrations run on every open, so a fresh database gets it there too. */
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
    /* Pool identity. Config, not measurement, so it is written once at
     * startup rather than on the template path. It lives here because the
     * dashboard has no other honest source for it: a miner pointed at the
     * stratum port cannot see which chain the coinbase is built for, whose
     * tag is in it, or where the money goes, and a second copy in the
     * dashboard's own environment is exactly the drift this table exists
     * to prevent. network_source records whether getblockchaininfo
     * answered ('node') or the network was read off the operator address
     * ('inferred'), which cannot tell testnet from signet. */
    "  network             TEXT,"
    "  network_source      TEXT,"    /* 'node' | 'inferred' */
    "  coinbase_tag        TEXT,"
    "  operator_address    TEXT,"    /* fee_bps recipient */
    "  pool_btc_address    TEXT,"    /* pps-classic only; NULL in solo */
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
    /* Pool identity. An upgraded DB has these NULL until the proxy restarts
     * and writes them, which is why the dashboard renders "unknown" rather
     * than guessing — a banner that asserts the wrong network is worse than
     * one that admits it doesn't know yet. */
    "ALTER TABLE pool_meta    ADD COLUMN network          TEXT",
    "ALTER TABLE pool_meta    ADD COLUMN network_source   TEXT",
    "ALTER TABLE pool_meta    ADD COLUMN coinbase_tag     TEXT",
    "ALTER TABLE pool_meta    ADD COLUMN operator_address TEXT",
    "ALTER TABLE pool_meta    ADD COLUMN pool_btc_address TEXT",
    /* Stratum ports and their difficulty policies. NULL on an upgraded DB
     * until the proxy restarts, which the dashboard renders as "not
     * published yet" rather than claiming the pool has one port. */
    "ALTER TABLE pool_meta    ADD COLUMN listeners        TEXT",
    /* Block accounting. Every pre-existing row becomes 'pending' — which
     * counts as nothing — rather than being assumed good: the rows were
     * written unconditionally, including for candidates submitblock had
     * already refused, so trusting them is what disabled the solvency
     * guard in the first place. A reconciliation pass classifies them.
     *
     * The UNIQUE index on hash is deliberately NOT here. It fails outright
     * on a table that already holds duplicate hashes, and the runner below
     * only special-cases "duplicate column" — every other error is a
     * warning and carry on, so putting it here would leave the index
     * silently absent on exactly the databases that needed it. It belongs
     * after the dedupe, in the reconciliation pass. */
    "ALTER TABLE blocks_found ADD COLUMN status        TEXT NOT NULL DEFAULT 'pending'",
    "ALTER TABLE blocks_found ADD COLUMN confirmations INTEGER NOT NULL DEFAULT 0",
    "ALTER TABLE blocks_found ADD COLUMN submit_error  TEXT",
    "ALTER TABLE blocks_found ADD COLUMN checked_via   TEXT",
    "CREATE INDEX IF NOT EXISTS blocks_found_status_idx ON blocks_found(status)",
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
    uint8_t  block_status;      /* EV_BLOCK only: STORE_BLOCK_* */
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

    /* Serialises TRANSACTION SCOPES on `db`.
     *
     * SQLITE_OPEN_FULLMUTEX makes individual API calls thread-safe, but a
     * transaction is not one call — and BEGIN..COMMIT is per-connection, not
     * per-thread. The writer thread and store_prop_settle_block (which runs on
     * the caller's thread, off the block-found callback) both open one on this
     * same handle, so whichever lost the race got "cannot start a transaction
     * within a transaction". That was silent: settle ignored its BEGIN result,
     * ran its statements inside the WRITER's transaction, and then committed
     * it early — tearing a share batch in half. Reproduced at ~6/25 runs of
     * test_store before this lock existed. */
    pthread_mutex_t tx_mu;

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
        sqlite3_bind_text(s->st_insert_block, 8,
                          store_block_status_text(ev->block_status), -1,
                          SQLITE_STATIC);
        if (ev->reason[0])
            sqlite3_bind_text(s->st_insert_block, 9, ev->reason, -1,
                              SQLITE_TRANSIENT);
        else
            sqlite3_bind_null(s->st_insert_block, 9);
        if (sqlite3_step(s->st_insert_block) != SQLITE_DONE) {
            atomic_fetch_add(&s->pg_errors, 1);
        } else if (sqlite3_changes(s->db) > 0 &&
                   ev->block_status != STORE_BLOCK_REJECTED) {
            /* Candidates submitblock refused are recorded but not counted:
             * the whole point of this column is that they are not blocks.
             * An OR IGNORE that changed nothing is a duplicate hash, which
             * is not a new block either. */
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
    pthread_mutex_lock(&s->tx_mu);
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
            pthread_mutex_unlock(&s->tx_mu);
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
    pthread_mutex_unlock(&s->tx_mu);
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
    /* OR IGNORE so a re-found hash cannot fail the step. The dedupe guard
     * in stratum is an in-memory ring that empties on restart, so the same
     * solution can legitimately arrive twice; once the unique index exists
     * that would otherwise land in pg_errors and vanish. sqlite3_changes()
     * below tells a real insert from an ignored duplicate. */
    static const char *Q_INS_BLOCK =
        "INSERT OR IGNORE INTO blocks_found "
        "  (ts, height, hash, finder_id, finder_address, reward_sats, fee_sats,"
        "   status, submit_error) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)";
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
    pthread_mutex_init(&s->tx_mu, NULL);

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
    pthread_mutex_destroy(&s->tx_mu);
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

const char *store_block_status_text(int status) {
    switch (status) {
    case STORE_BLOCK_CONFIRMED: return "confirmed";
    case STORE_BLOCK_ORPHANED:  return "orphaned";
    case STORE_BLOCK_REJECTED:  return "rejected";
    default:                    return "pending";
    }
}

int store_list_unresolved_blocks(store_t *s, int tip_height, int final_depth,
                                 store_block_candidate_t *out, size_t cap)
{
    if (!s || !out || cap == 0) return -1;
    /* Deliberately narrower than the templates pass: only pending and
     * confirmed rows, and only while shallow. Re-checking every orphan over
     * RPC forever would be one call per settled row per tick — on a
     * low-difficulty chain that is the whole table. Restoring an orphan after
     * a second reorg is left to the templates pass, which does it in bulk SQL
     * for nothing. */
    static const char *Q =
        "SELECT hash, height FROM blocks_found "
        " WHERE status IN ('pending','confirmed') "
        "   AND confirmations < ? "
        "   AND height <= ? "
        " ORDER BY height DESC, id DESC LIMIT ?";
    sqlite3_stmt *st = NULL;
    int n = 0;
    pthread_mutex_lock(&s->node_tip_mu);
    if (sqlite3_prepare_v2(s->db, Q, -1, &st, NULL) != SQLITE_OK) {
        pthread_mutex_unlock(&s->node_tip_mu);
        atomic_fetch_add(&s->pg_errors, 1);
        return -2;
    }
    sqlite3_bind_int(st, 1, final_depth);
    sqlite3_bind_int(st, 2, tip_height);
    sqlite3_bind_int(st, 3, (int)cap);
    while (n < (int)cap && sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *h = sqlite3_column_text(st, 0);
        if (!h) continue;
        snprintf(out[n].hash, sizeof(out[n].hash), "%s", (const char *)h);
        out[n].height = sqlite3_column_int(st, 1);
        n++;
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&s->node_tip_mu);
    return n;
}

int store_set_block_status(store_t *s, const char *hash, int status,
                           int confirmations, const char *checked_via)
{
    if (!s || !hash) return -1;
    static const char *Q =
        "UPDATE blocks_found SET status = ?, confirmations = ?, checked_via = ? "
        " WHERE hash = ?";
    sqlite3_stmt *st = NULL;
    pthread_mutex_lock(&s->node_tip_mu);
    if (sqlite3_prepare_v2(s->db, Q, -1, &st, NULL) != SQLITE_OK) {
        pthread_mutex_unlock(&s->node_tip_mu);
        atomic_fetch_add(&s->pg_errors, 1);
        return -2;
    }
    sqlite3_bind_text(st, 1, store_block_status_text(status), -1, SQLITE_STATIC);
    sqlite3_bind_int (st, 2, confirmations < 0 ? 0 : confirmations);
    if (checked_via)
        sqlite3_bind_text(st, 3, checked_via, -1, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(st, 3);
    sqlite3_bind_text(st, 4, hash, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    pthread_mutex_unlock(&s->node_tip_mu);
    if (rc != SQLITE_DONE) {
        atomic_fetch_add(&s->pg_errors, 1);
        return -2;
    }
    return 0;
}

/* Count rows in one status. Caller holds node_tip_mu. */
static int count_blocks_with_status(store_t *s, const char *status) {
    static const char *Q = "SELECT COUNT(*) FROM blocks_found WHERE status = ?";
    sqlite3_stmt *st = NULL;
    int n = 0;
    if (sqlite3_prepare_v2(s->db, Q, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, status, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
}

int store_reconcile_blocks_from_templates(store_t *s, int tip_height,
                                          int *confirmed, int *orphaned,
                                          int *pending)
{
    if (!s) return -1;
    /* Compare against the LATEST observation at height+1, not merely any of
     * them. After a reorg both the winning and the losing prev_hash have been
     * seen at that height, so "a template exists whose prev_hash is ours" would
     * keep calling a reorged-out block confirmed forever. The newest row is
     * what the node believes now.
     *
     * Every non-rejected status is in the WHERE, so the pass is idempotent and
     * symmetric: a confirmed block is demoted when it is reorged out — losing
     * the chain has to take the reward back, not merely fail to grant it — and
     * an orphan is promoted again if a later reorg restores it. Only 'rejected'
     * is terminal: the node never accepted that candidate, so no amount of
     * reorganising can put it in the chain. */
    static const char *Q_RESOLVE =
        "WITH tip_at AS ("
        "  SELECT b.id AS bid,"
        "         (SELECT t.prev_hash FROM templates t"
        "           WHERE t.height = b.height + 1"
        "           ORDER BY t.id DESC LIMIT 1) AS observed"
        "    FROM blocks_found b"
        "   WHERE b.status <> 'rejected'"
        ") "
        "UPDATE blocks_found SET"
        "  status = CASE WHEN (SELECT observed FROM tip_at WHERE bid = blocks_found.id)"
        "                     = blocks_found.hash THEN 'confirmed' ELSE 'orphaned' END,"
        "  checked_via = 'tips',"
        "  confirmations = CASE WHEN (SELECT observed FROM tip_at WHERE bid = blocks_found.id)"
        "                            = blocks_found.hash"
        "                       THEN MAX(0, ? - blocks_found.height + 1) ELSE 0 END "
        " WHERE status <> 'rejected'"
        "   AND (SELECT observed FROM tip_at WHERE bid = blocks_found.id) IS NOT NULL";

    /* A height at or above the tip cannot be a block in the chain, and a
     * height of 0 was never valid. Neither is verifiable, and leaving them
     * pending would leave junk looking merely unverified. */
    static const char *Q_IMPOSSIBLE =
        "UPDATE blocks_found SET status = 'orphaned', confirmations = 0,"
        "       checked_via = 'tips' "
        " WHERE status <> 'rejected' AND (height <= 0 OR height > ?)";

    pthread_mutex_lock(&s->node_tip_mu);
    char *err = NULL;
    sqlite3_stmt *st = NULL;
    int rc = 0;
    if (sqlite3_prepare_v2(s->db, Q_RESOLVE, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, tip_height);
        if (sqlite3_step(st) != SQLITE_DONE) rc = -2;
        sqlite3_finalize(st);
    } else {
        rc = -2;
    }
    st = NULL;
    if (tip_height > 0 &&
        sqlite3_prepare_v2(s->db, Q_IMPOSSIBLE, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, tip_height);
        if (sqlite3_step(st) != SQLITE_DONE) rc = -2;
        sqlite3_finalize(st);
    }
    sqlite3_free(err);
    if (confirmed) *confirmed = count_blocks_with_status(s, "confirmed");
    if (orphaned)  *orphaned  = count_blocks_with_status(s, "orphaned");
    if (pending)   *pending   = count_blocks_with_status(s, "pending");
    pthread_mutex_unlock(&s->node_tip_mu);
    if (rc != 0) atomic_fetch_add(&s->pg_errors, 1);
    return rc;
}

int store_finalize_block_hash_index(store_t *s) {
    if (!s) return -1;
    /* Carry any resolved verdict onto the row that will survive, so collapsing
     * duplicates cannot lose a confirmation. */
    static const char *Q_PROMOTE =
        "UPDATE blocks_found SET status = ("
        "  SELECT b2.status FROM blocks_found b2"
        "   WHERE b2.hash = blocks_found.hash AND b2.status <> 'pending'"
        "   ORDER BY b2.id LIMIT 1) "
        " WHERE status = 'pending' AND EXISTS ("
        "  SELECT 1 FROM blocks_found b3"
        "   WHERE b3.hash = blocks_found.hash AND b3.status <> 'pending')";
    /* Keep the earliest sighting of each hash — that is when the pool
     * actually found it. Competing candidates at one height have DIFFERENT
     * hashes and are all kept: several rows per height is expected on a
     * low-difficulty chain, and status is what stops them counting. */
    static const char *Q_DEDUPE =
        "DELETE FROM blocks_found WHERE id NOT IN ("
        "  SELECT MIN(id) FROM blocks_found GROUP BY hash)";
    static const char *Q_INDEX =
        "CREATE UNIQUE INDEX IF NOT EXISTS blocks_found_hash_idx "
        "  ON blocks_found(hash)";

    pthread_mutex_lock(&s->node_tip_mu);
    int rc = 0;
    char *err = NULL;
    if (sqlite3_exec(s->db, Q_PROMOTE, NULL, NULL, &err) != SQLITE_OK) {
        LOG_WARN("store: block hash promote failed: %s", err ? err : "?");
        rc = -2;
    }
    sqlite3_free(err); err = NULL;
    if (sqlite3_exec(s->db, Q_DEDUPE, NULL, NULL, &err) != SQLITE_OK) {
        LOG_WARN("store: block hash dedupe failed: %s", err ? err : "?");
        rc = -2;
    }
    sqlite3_free(err); err = NULL;
    if (sqlite3_exec(s->db, Q_INDEX, NULL, NULL, &err) != SQLITE_OK) {
        /* Loud: a missing unique index is exactly the silent failure this
         * function exists to avoid. */
        LOG_ERROR("store: blocks_found unique hash index NOT created: %s",
                  err ? err : "?");
        rc = -2;
    }
    sqlite3_free(err);
    pthread_mutex_unlock(&s->node_tip_mu);
    return rc;
}

int store_record_block(store_t *s, uint64_t ts_ms, int height,
                       const char *hash, const char *finder_name,
                       const char *finder_address,
                       int64_t reward_sats, int64_t fee_sats,
                       int status, const char *submit_error)
{
    if (!s || !hash) return -1;
    /* A coinbase height of zero is never valid. bitcoind_parse_template
     * already refuses a template without a numeric height, so reaching here
     * with 0 means the template was not parsed — record nothing and say so
     * rather than filing a block at a height that cannot exist. */
    if (height <= 0) {
        atomic_fetch_add(&s->pg_errors, 1);
        return -1;
    }
    event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = EV_BLOCK;
    ev.ts_ms = ts_ms;
    ev.height = height;
    ev.reward_sats = reward_sats;
    ev.fee_sats = fee_sats;
    ev.block_status = (uint8_t)status;
    if (submit_error)
        strncpy(ev.reason, submit_error, REASON_MAX - 1);
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

int store_record_pool_identity(store_t *s, const char *network,
                               const char *network_source,
                               const char *coinbase_tag,
                               const char *operator_address,
                               const char *pool_btc_address,
                               const char *listeners_json)
{
    if (!s) return -1;
    /* Upserts the same id=1 row as store_record_pool_meta(), but only the
     * identity columns — the two never write each other's fields, so
     * whichever runs first is harmless. Notably this does NOT touch
     * updated_at: that timestamp means "when the rate was last refreshed",
     * and identity is written once at startup, so stamping it here would
     * make a stalled template path look alive.
     *
     * pool_btc_address is stored as NULL rather than "" in solo mode, so a
     * reader can tell "not applicable in this mode" from "configured
     * blank". */
    static const char *Q =
        "INSERT INTO pool_meta (id, network, network_source, coinbase_tag,"
        "  operator_address, pool_btc_address, listeners) "
        "VALUES (1, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(id) DO UPDATE SET "
        "  network = excluded.network,"
        "  network_source = excluded.network_source,"
        "  coinbase_tag = excluded.coinbase_tag,"
        "  operator_address = excluded.operator_address,"
        "  pool_btc_address = excluded.pool_btc_address,"
        "  listeners = excluded.listeners";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(s->db, Q, -1, &st, NULL) != SQLITE_OK) {
        atomic_fetch_add(&s->pg_errors, 1);
        return -2;
    }
    pthread_mutex_lock(&s->node_tip_mu);
    sqlite3_bind_text(st, 1, network          ? network          : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, network_source   ? network_source   : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, coinbase_tag     ? coinbase_tag     : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, operator_address ? operator_address : "", -1, SQLITE_TRANSIENT);
    if (pool_btc_address && pool_btc_address[0]) {
        sqlite3_bind_text(st, 5, pool_btc_address, -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(st, 5);
    }
    /* NULL rather than "[]" when there is nothing to say, so the dashboard
     * can tell "this proxy predates the column" from "this pool really does
     * serve one port". */
    if (listeners_json && listeners_json[0]) {
        sqlite3_bind_text(st, 6, listeners_json, -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(st, 6);
    }
    int rc = sqlite3_step(st);
    pthread_mutex_unlock(&s->node_tip_mu);
    sqlite3_finalize(st);
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

double store_worker_recent_difficulty(store_t *s, const char *worker_name,
                                      int lookback_sec) {
    if (!s || !worker_name || !*worker_name || lookback_sec <= 0) return 0.0;

    /* Median of the most RECENT shares — newest first, then median of those.
     *
     * Not the median over the whole lookback: vardiff climbs 4x per window, so
     * a miner that has reconnected a few times leaves a long tail of low-
     * difficulty ramp shares. Observed live — an Avalon that had converged to
     * 13,680 had an hour-median of 4, because repeated restarts meant most of
     * its shares were from the climb. The last few dozen shares are where
     * vardiff had actually settled. */
    static const char *Q =
        "SELECT s.difficulty FROM ("
        "  SELECT s2.difficulty AS difficulty, s2.ts AS ts FROM shares s2"
        "    JOIN workers w2 ON s2.worker_id = w2.id"
        /* Block-shares count here too: at low difficulty every share is a
         * block, and filtering them reported 0 for every worker -- which
         * both blanked the dashboard hashrate and reset reconnecting
         * miners to initial_diff. Same root cause as prop_window_agg. */
        "    WHERE w2.name = ? AND s2.ts >= ?"
        "    ORDER BY s2.ts DESC, s2.id DESC LIMIT 32"
        ") s ORDER BY s.difficulty";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(s->db, Q, -1, &st, NULL) != SQLITE_OK) {
        atomic_fetch_add(&s->pg_errors, 1);
        return 0.0;
    }
    sqlite3_bind_text (st, 1, worker_name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)(time(NULL) - lookback_sec));

    size_t cap = 256, n = 0;
    double *vals = (double *)calloc(cap, sizeof(*vals));
    if (!vals) { sqlite3_finalize(st); return 0.0; }
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (n >= cap) {
            size_t ncap = cap * 2;
            double *nv = (double *)realloc(vals, ncap * sizeof(*nv));
            if (!nv) { free(vals); sqlite3_finalize(st); return 0.0; }
            vals = nv; cap = ncap;
        }
        vals[n++] = sqlite3_column_double(st, 0);
    }
    sqlite3_finalize(st);

    /* Too few samples to be worth trusting — let initial_diff stand. */
    if (n < 8) { free(vals); return 0.0; }
    double med = (n % 2) ? vals[n / 2]
                         : (vals[n / 2 - 1] + vals[n / 2]) / 2.0;
    free(vals);
    return (med > 0.0) ? med : 0.0;
}

/* ---------- proportional / PPLNS helpers ---------- */

/* ⛔ TAKES tx_mu, and must. store_prop_settle_block rewrites this table WHOLE —
 * DELETE FROM prop_ledger, then re-INSERT row by row, inside one transaction on
 * THIS SAME HANDLE. A connection sees its own uncommitted writes, so an
 * unsynchronised read here can land between the DELETE and the COMMIT and
 * return a PARTIALLY REBUILT table. A subset of a zero-sum ledger is not
 * zero-sum, and pplns_compute_payouts then refuses it — costing that block its
 * proportional split, which is paid to the finder instead.
 *
 * Observed in production 2026-08-25: six torn reads in ~19 h, five of which
 * landed on a block. Always negative, because the table holds a few large
 * negative claims and many small positive ones, so an interrupted rebuild
 * skews that way. The stored ledger was never corrupt — only the read was.
 *
 * ⚠️ The writer thread's batches only INSERT, so an unsynchronised read of the
 * tables IT owns is merely stale. Only a DELETE-then-rebuild turns a stale read
 * into a torn one — which is why this was the one getter that needed the lock
 * and the only one that showed it. Any future path that rewrites a table whole
 * puts every reader of that table under the same rule.
 *
 * Safe against deadlock: the only caller is prop_build_plan on the template
 * thread, which holds no store lock. */
int store_prop_get_ledger(store_t *s, pplns_claim_t **out, size_t *n) {
    if (!s || !out || !n) return -1;
    *out = NULL; *n = 0;

    static const char *Q =
        "SELECT address, claim_fraction FROM prop_ledger"
        "  WHERE claim_fraction != 0";
    sqlite3_stmt *st = NULL;

    pthread_mutex_lock(&s->tx_mu);
    if (sqlite3_prepare_v2(s->db, Q, -1, &st, NULL) != SQLITE_OK) {
        pthread_mutex_unlock(&s->tx_mu);
        atomic_fetch_add(&s->pg_errors, 1);
        return -1;
    }

    size_t cap = 64, count = 0;
    pplns_claim_t *buf = (pplns_claim_t *)calloc(cap, sizeof(*buf));
    if (!buf) {
        sqlite3_finalize(st);
        pthread_mutex_unlock(&s->tx_mu);
        return -1;
    }

    while (sqlite3_step(st) == SQLITE_ROW) {
        if (count >= cap) {
            size_t ncap = cap * 2;
            pplns_claim_t *nb = (pplns_claim_t *)realloc(buf, ncap * sizeof(*nb));
            if (!nb) {
                free(buf);
                sqlite3_finalize(st);
                pthread_mutex_unlock(&s->tx_mu);
                return -1;
            }
            buf = nb; cap = ncap;
        }
        const char *addr = (const char *)sqlite3_column_text(st, 0);
        snprintf(buf[count].address, sizeof buf[count].address, "%s",
                 addr ? addr : "");
        buf[count].claim_fraction = sqlite3_column_double(st, 1);
        count++;
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&s->tx_mu);
    *out = buf;
    *n = count;
    return 0;
}

/* SUM(difficulty), MIN(ts) and COUNT(*) over [from_s, before_s]. One aggregate,
 * so SQLite never hands the rows back to C. Returns 0 ok, -1 on error.
 *
 * !! Do NOT re-add an `is_block = 0` filter here or in the other window
 * queries. A share that also cleared the network target is still work the
 * miner performed, and in proportional mode the finder gets no separate
 * payment -- prop_build_plan replaces the coinbase outputs wholesale -- so
 * counting it double-pays nobody.
 *
 * Excluding them looked harmless because blocks are one share in millions
 * at normal difficulty. It is fatal at low difficulty: vardiff clamps the
 * share difficulty to never exceed the network difficulty (stratum.c,
 * vardiff_maybe_retarget), and is_block is `hash <= network_target`. Once
 * that clamp binds, share target == network target, so EVERY share is a
 * block, every row is filtered out, the window is empty, and every block
 * falls through to paying its finder directly. PPLNS silently becomes solo
 * -- exactly during the post-fork minimum-difficulty window. */
static int prop_window_agg(store_t *s, sqlite3_int64 from_s, sqlite3_int64 before_s,
                           double *out_sum, sqlite3_int64 *out_min_ts,
                           sqlite3_int64 *out_count) {
    static const char *Q =
        "SELECT COALESCE(SUM(s.difficulty),0), COALESCE(MIN(s.ts),0), COUNT(*)"
        "  FROM shares s JOIN workers w ON s.worker_id = w.id"
        "  WHERE s.ts >= ? AND s.ts <= ?"
        "    AND w.payout_address IS NOT NULL AND w.payout_address != ''";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(s->db, Q, -1, &st, NULL) != SQLITE_OK) {
        atomic_fetch_add(&s->pg_errors, 1);
        return -1;
    }
    sqlite3_bind_int64(st, 1, from_s);
    sqlite3_bind_int64(st, 2, before_s);
    int rc = -1;
    if (sqlite3_step(st) == SQLITE_ROW) {
        *out_sum    = sqlite3_column_double(st, 0);
        *out_min_ts = sqlite3_column_int64(st, 1);
        *out_count  = sqlite3_column_int64(st, 2);
        rc = 0;
    }
    sqlite3_finalize(st);
    return rc;
}

/* How many share rows the backward walk may read before giving up. Generous:
 * this is only reached when the work target needs more history than the time
 * floor holds, and the alternative to a cap is an unbounded scan on every
 * template. Hitting it is reported, never silently absorbed. */
#define PROP_WINDOW_MAX_ROWS 5000000
#define PROP_WINDOW_PAGE     100000

/* Overridable so tests can exercise paging and the cap without inserting
 * millions of rows. Tests only. */
static sqlite3_int64 g_window_max_rows = PROP_WINDOW_MAX_ROWS;
static int           g_window_page     = PROP_WINDOW_PAGE;

void store_test_set_window_limits(int page, long max_rows) {
    g_window_page     = (page > 0) ? page : PROP_WINDOW_PAGE;
    g_window_max_rows = (max_rows > 0) ? (sqlite3_int64)max_rows : PROP_WINDOW_MAX_ROWS;
}

int store_prop_compute_window(store_t *s, double window_difficulty,
                              uint64_t before_ms, int min_window_sec,
                              uint64_t *out_start_ms,
                              double *out_actual_difficulty,
                              int *out_truncated) {
    if (!s || window_difficulty <= 0.0 || !out_start_ms || !out_actual_difficulty ||
        min_window_sec < 0)
        return -1;
    if (out_truncated) *out_truncated = 0;

    /* The shares table stores Unix seconds; callers pass milliseconds. */
    sqlite3_int64 before_s = (sqlite3_int64)(before_ms / 1000);
    sqlite3_int64 floor_s = before_s - (sqlite3_int64)min_window_sec;
    if (floor_s < 0) floor_s = 0;

    /* Fast path: does the time floor alone already carry the work target?
     *
     * This is the fork case, and the reason the row walk below is not enough on
     * its own. When difficulty resets to powLimit the work target is a handful
     * of difficulty units while the floor holds every share of the last ten
     * minutes — hundreds of thousands of them at fork share rates. Answering
     * that with one SUM avoids walking any of them. */
    if (min_window_sec > 0) {
        double sum = 0.0; sqlite3_int64 min_ts = 0, count = 0;
        if (prop_window_agg(s, floor_s, before_s, &sum, &min_ts, &count) < 0) return -1;
        if (count > 0 && sum >= window_difficulty) {
            *out_start_ms = (uint64_t)min_ts * 1000ULL;
            *out_actual_difficulty = sum;
            return 0;
        }
    }

    /* Slow path: the floor does not hold enough work, so walk further back.
     * Paged rather than a single capped query — a fixed LIMIT silently returns
     * a SHORTER window than asked for, which looks like a working pool paying
     * on the wrong window. */
    static const char *Q =
        "SELECT s.ts, s.id, s.difficulty FROM shares s"
        "  JOIN workers w ON s.worker_id = w.id"
        /* No is_block filter -- see prop_window_agg. */
        "  WHERE (s.ts < ? OR (s.ts = ? AND s.id < ?))"
        "    AND w.payout_address IS NOT NULL AND w.payout_address != ''"
        "  ORDER BY s.ts DESC, s.id DESC"
        "  LIMIT ?";

    double cum = 0.0;
    uint64_t boundary_s = 0;
    int any = 0, done = 0, exhausted = 0, boundary_closed = 0;
    sqlite3_int64 cur_ts = before_s + 1, cur_id = 0;   /* exclusive cursor */
    sqlite3_int64 rows_read = 0;

    /* Loop on boundary_closed, NOT on done.
     *
     * `done` means the work target and the time floor are both met; the walk
     * must then keep taking rows until it has SEEN a row older than
     * boundary_s, because shares sharing the boundary second are taken
     * together or not at all. Exiting on `done` alone drops the rest of that
     * second whenever the deciding row happens to be the last of a page — and
     * store_prop_window_addrs() still counts those rows, because it selects on
     * `ts >= start_s` with no id cursor. The window total then disagrees with
     * the addresses in it, which is exactly the disagreement pplns.c now
     * refuses to be handed. */
    while (!boundary_closed && !exhausted && rows_read < g_window_max_rows) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(s->db, Q, -1, &st, NULL) != SQLITE_OK) {
            atomic_fetch_add(&s->pg_errors, 1);
            return -1;
        }
        sqlite3_bind_int64(st, 1, cur_ts);
        sqlite3_bind_int64(st, 2, cur_ts);
        sqlite3_bind_int64(st, 3, cur_id);
        sqlite3_bind_int  (st, 4, g_window_page);

        sqlite3_int64 in_page = 0;
        while (sqlite3_step(st) == SQLITE_ROW) {
            sqlite3_int64 ts = sqlite3_column_int64(st, 0);
            sqlite3_int64 id = sqlite3_column_int64(st, 1);
            double diff = sqlite3_column_double(st, 2);
            in_page++; rows_read++;
            any = 1;
            /* Stop only once BOTH the work target and the time floor are met,
             * and only on a second boundary — shares sharing a timestamp are
             * taken together or not at all. */
            if (done && (uint64_t)ts != boundary_s) { boundary_closed = 1; break; }
            cum += diff;
            boundary_s = (uint64_t)ts;
            cur_ts = ts; cur_id = id;
            done = (cum >= window_difficulty) && (ts <= floor_s);
        }
        sqlite3_finalize(st);
        if (in_page < g_window_page) exhausted = 1;   /* no older shares */
    }

    if (!any) return -1;
    /* Truncation is still about the WORK target, not the boundary sweep: a walk
     * that met the target and then ran out of rows closing the second read the
     * whole window it asked for. */
    if (!done && !exhausted && out_truncated) *out_truncated = 1;

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
        /* No is_block filter -- see prop_window_agg. */
        "  WHERE s.ts >= ? AND s.ts <= ?"
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

/* Accumulate `delta` against `addr` in a working ledger, appending a row if the
 * address is not there yet. Returns 0 ok, -1 if the array is full. */
static int ledger_acc(pplns_claim_t *w, size_t cap, size_t *n,
                      const char *addr, double delta) {
    for (size_t i = 0; i < *n; i++) {
        if (strcmp(w[i].address, addr) != 0) continue;
        w[i].claim_fraction += delta;
        return 0;
    }
    if (*n >= cap) return -1;
    snprintf(w[*n].address, sizeof w[*n].address, "%s", addr);
    w[*n].claim_fraction = delta;
    (*n)++;
    return 0;
}

int store_prop_settle_block(store_t *s, uint64_t ts_ms,
                            const pplns_claim_t *ledger_in, size_t n_ledger_in,
                            const pplns_claim_t *ledger_out, size_t n_ledger_out) {
    if (!s) return -1;
    if ((n_ledger_in && !ledger_in) || (n_ledger_out && !ledger_out)) return -1;

    sqlite3_stmt *st = NULL;
    int rc;
    pplns_claim_t *merged = NULL;
    size_t n_merged = 0, merged_cap = 0;

    /* Read the CURRENT ledger inside the write transaction, apply this block's
     * delta to it, and write the result. The plan's own post-block ledger is
     * not the answer on its own: it was computed against whatever the table
     * held when the template was fetched, and another block may have settled
     * since. See the contract in store.h. */
    static const char *Q_CURRENT =
        "SELECT address, claim_fraction FROM prop_ledger";
    static const char *Q_UPSERT =
        "INSERT INTO prop_ledger (address, claim_fraction, last_settled_ts)"
        "  VALUES (?, ?, ?)"
        "  ON CONFLICT(address) DO UPDATE SET"
        "    claim_fraction = excluded.claim_fraction,"
        "    last_settled_ts = excluded.last_settled_ts";
    static const char *Q_CLEAR = "DELETE FROM prop_ledger";

    /* IMMEDIATE, not deferred. A deferred transaction takes its write lock
     * lazily, and in WAL mode an upgrade after another writer has committed
     * fails with SQLITE_BUSY_SNAPSHOT — which busy_timeout does NOT retry.
     * The store's own writer thread commits concurrently with this, so that
     * is a live possibility, not a theoretical one. */
    pthread_mutex_lock(&s->tx_mu);
    if (sqlite3_exec(s->db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
        LOG_ERROR("store: settle could not open a transaction: %s",
                  sqlite3_errmsg(s->db));
        atomic_fetch_add(&s->pg_errors, 1);
        pthread_mutex_unlock(&s->tx_mu);
        return -1;
    }

    if (sqlite3_prepare_v2(s->db, Q_CURRENT, -1, &st, NULL) != SQLITE_OK) {
        LOG_ERROR("store: settle could not read prop_ledger: %s",
                  sqlite3_errmsg(s->db));
        goto fail;
    }
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (n_merged >= merged_cap) {
            size_t ncap = merged_cap ? merged_cap * 2 : 64;
            pplns_claim_t *nb = (pplns_claim_t *)realloc(merged, ncap * sizeof(*nb));
            if (!nb) { sqlite3_finalize(st); st = NULL; goto fail; }
            merged = nb; merged_cap = ncap;
        }
        const char *a = (const char *)sqlite3_column_text(st, 0);
        snprintf(merged[n_merged].address, sizeof merged[n_merged].address,
                 "%s", a ? a : "");
        merged[n_merged].claim_fraction = sqlite3_column_double(st, 1);
        n_merged++;
    }
    sqlite3_finalize(st);
    st = NULL;
    if (rc != SQLITE_DONE) {
        LOG_ERROR("store: settle could not read prop_ledger: %s",
                  sqlite3_errmsg(s->db));
        goto fail;
    }

    /* Room for every address the delta can introduce on top of what is stored. */
    {
        size_t need = n_merged + n_ledger_in + n_ledger_out;
        if (need > merged_cap) {
            pplns_claim_t *nb = (pplns_claim_t *)realloc(merged, need * sizeof(*nb));
            if (!nb) goto fail;
            merged = nb; merged_cap = need;
        }
    }
    for (size_t i = 0; i < n_ledger_in; i++) {
        if (ledger_in[i].address[0] == '\0') continue;
        if (ledger_acc(merged, merged_cap, &n_merged,
                       ledger_in[i].address, -ledger_in[i].claim_fraction) < 0)
            goto fail;
    }
    for (size_t i = 0; i < n_ledger_out; i++) {
        if (ledger_out[i].address[0] == '\0') continue;
        if (ledger_acc(merged, merged_cap, &n_merged,
                       ledger_out[i].address, ledger_out[i].claim_fraction) < 0)
            goto fail;
    }

    if (sqlite3_prepare_v2(s->db, Q_CLEAR, -1, &st, NULL) != SQLITE_OK) goto fail;
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    st = NULL;
    if (rc != SQLITE_DONE) {
        LOG_ERROR("store: settle could not clear prop_ledger: %s",
                  sqlite3_errmsg(s->db));
        goto fail;
    }

    for (size_t i = 0; i < n_merged; i++) {
        /* A claim that cancelled out is absent from the table, not stored as a
         * zero. The bound here is floating-point noise only — pruning a claim
         * that is merely SMALL is pplns_compute_payouts's job, because only it
         * knows the reward that says what a satoshi is worth. */
        if (fabs(merged[i].claim_fraction) < 1e-12) continue;
        if (sqlite3_prepare_v2(s->db, Q_UPSERT, -1, &st, NULL) != SQLITE_OK) goto fail;
        sqlite3_bind_text  (st, 1, merged[i].address, -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(st, 2, merged[i].claim_fraction);
        sqlite3_bind_int64 (st, 3, (sqlite3_int64)(ts_ms / 1000));
        rc = sqlite3_step(st);
        sqlite3_finalize(st);
        st = NULL;
        if (rc != SQLITE_DONE) goto fail;
    }

    /* ⛔ This function does NOT write blocks_found. on_block_found_cb() calls
     * store_record_block() for EVERY block, pooled or solo, and that row is the
     * complete one — finder_id, finder_address, reward_sats, fee_sats. A second
     * insert here (finder NULL, reward 0) duplicated every pooled block in the
     * table and double-counted blocks_committed: 27 rows for 26 blocks, observed
     * on regtest 2026-08-20. The settle path owns prop_ledger, nothing else. */
    /* The COMMIT return value is the whole ballgame. on_block_found_cb has
     * ALREADY consumed the plan (memset, so it can never settle twice) by the
     * time it calls us, so reporting success on a failed commit means: the
     * plan is gone, prop_ledger still holds the pre-block state, and the
     * claims this block just paid out in its coinbase stay on the books to be
     * paid again in the next window. Silently. */
    if (sqlite3_exec(s->db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        LOG_ERROR("store: settle COMMIT failed: %s — prop_ledger is unchanged, "
                  "so the claims this block paid are still recorded as owed",
                  sqlite3_errmsg(s->db));
        goto fail;
    }
    pthread_mutex_unlock(&s->tx_mu);
    free(merged);
    return 0;

fail:
    if (st) sqlite3_finalize(st);
    sqlite3_exec(s->db, "ROLLBACK", NULL, NULL, NULL);
    atomic_fetch_add(&s->pg_errors, 1);
    pthread_mutex_unlock(&s->tx_mu);
    free(merged);
    return -1;
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
