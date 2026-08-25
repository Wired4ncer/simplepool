PRAGMA journal_mode = WAL;
PRAGMA synchronous = NORMAL;
PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS workers (
  id              INTEGER PRIMARY KEY AUTOINCREMENT,
  name            TEXT UNIQUE NOT NULL,
  first_seen      INTEGER NOT NULL,
  last_seen       INTEGER NOT NULL,
  payout_address  TEXT
);

-- credited_sats is what the share was ACTUALLY credited when it was
-- accepted. The PPS rate is derived per-template and moves with network
-- difficulty, so recomputing historical shares against a current rate
-- misreports them. Audits must sum this column, not re-derive it.
-- 0 in solo mode, and 0 on rows written before the column existed
-- (see pool_meta.credited_from for where it becomes trustworthy).
-- rate_used is the exact rate the proxy multiplied by to get credited_sats.
-- Storing it alongside the result is what makes the credit *verifiable*
-- rather than merely recorded: an auditor can recompute
-- CAST(difficulty * rate_used AS INTEGER) and must get credited_sats back,
-- with no need to know what the rate happened to be at that moment.
-- 0 in solo mode and on rows written before the column existed.
CREATE TABLE IF NOT EXISTS shares (
  id            INTEGER PRIMARY KEY AUTOINCREMENT,
  worker_id     INTEGER NOT NULL REFERENCES workers(id),
  ts            INTEGER NOT NULL,
  difficulty    REAL NOT NULL,
  is_block      INTEGER NOT NULL DEFAULT 0,
  block_hash    TEXT,
  credited_sats INTEGER NOT NULL DEFAULT 0,
  rate_used     REAL NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS shares_ts_idx ON shares(ts);
CREATE INDEX IF NOT EXISTS shares_worker_ts_idx ON shares(worker_id, ts);

CREATE TABLE IF NOT EXISTS rejects (
  id          INTEGER PRIMARY KEY AUTOINCREMENT,
  worker_name TEXT,
  ts          INTEGER NOT NULL,
  reason      TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS rejects_ts_idx ON rejects(ts);

-- A row here is a block CANDIDATE, not a block. A share meeting network
-- difficulty produces one, and submitblock refuses stale, duplicate and
-- high-hash candidates routinely — on a low-difficulty chain that is nearly
-- every one of them. `status` is what separates the two, and every count and
-- every sum of reward_sats MUST filter on status='confirmed':
--
--   pending    submitted and accepted by the node, not yet verified to be in
--              the chain. NOT revenue. Against a backend that answers only
--              getblocktemplate/submitblock there may be nothing able to
--              verify it for some time, so this is a normal steady state
--              rather than a transient.
--   confirmed  verified to be in the chain. The only status worth money.
--   orphaned   was in the chain, then reorged out. Earns nothing.
--   rejected   submitblock refused it; submit_error carries the reason.
--
-- checked_via records who answered: 'node' (getblockhash) or 'tips' (the
-- observed chain of getblocktemplate prev_hashes, used when the backend does
-- not serve getblockhash) — the same distinction pool_meta.network_source
-- draws between an authoritative answer and an inferred one.
--
-- Several rows per height is expected, not a bug: on a low-difficulty chain
-- the pool genuinely finds competing candidates at one height. At most one of
-- them can be 'confirmed'.
CREATE TABLE IF NOT EXISTS blocks_found (
  id              INTEGER PRIMARY KEY AUTOINCREMENT,
  ts              INTEGER NOT NULL,
  height          INTEGER NOT NULL,
  hash            TEXT NOT NULL,
  finder_id       INTEGER REFERENCES workers(id),
  finder_address  TEXT,
  reward_sats     INTEGER,
  fee_sats        INTEGER,
  status          TEXT NOT NULL DEFAULT 'pending',
  confirmations   INTEGER NOT NULL DEFAULT 0,
  submit_error    TEXT,
  checked_via     TEXT
);
CREATE INDEX IF NOT EXISTS blocks_found_ts_idx ON blocks_found(ts);
CREATE INDEX IF NOT EXISTS blocks_found_status_idx ON blocks_found(status);

/* Single-row mirror of the upstream bitcoind tip the proxy is currently
 * mining on. Written by the proxy's tip watcher on every successful
 * getblocktemplate poll. Lets the dashboard show "latest block from the
 * node" and "time since last block" without any RPC of its own. */
CREATE TABLE IF NOT EXISTS node_status (
  id              INTEGER PRIMARY KEY CHECK (id = 1),
  tip_height      INTEGER,
  tip_hash        TEXT,
  tip_observed_at INTEGER,  /* unix seconds — when we first saw this tip */
  updated_at      INTEGER   /* unix seconds — last successful poll */
);

/* Single source of truth for what the running proxy is actually paying.
 *
 * The dashboard MUST read the rate from here rather than from its own
 * config or environment — holding the same number in two places is how an
 * audit ends up disagreeing with the ledger it exists to check.
 *
 * rate_source is 'derived' (computed from the live template and fee_bps —
 * the default and recommended setup) or 'override' (operator pinned
 * pps_sats_per_diff, which is taken NET of fee and bypasses fee_bps).
 * effective_fee_bps is what the numbers actually imply, which under an
 * override can differ from the configured fee_bps.
 *
 * credited_from is stamped once, on first write, and marks the point from
 * which shares.credited_sats is populated. */
CREATE TABLE IF NOT EXISTS pool_meta (
  id                  INTEGER PRIMARY KEY CHECK (id = 1),
  /* Pool identity — what the proxy is configured to be, written once at
   * startup rather than on the template path. A miner cannot tell any of
   * this from the stratum URL, so the dashboard has to say it: which chain
   * the coinbase is being built for, whose tag is in it, and where the
   * money goes. network_source is 'node' (getblockchaininfo answered) or
   * 'inferred' (it did not, and the network was read off the operator
   * address, which cannot distinguish testnet from signet). */
  network             TEXT,
  network_source      TEXT,     /* 'node' | 'inferred' */
  coinbase_tag        TEXT,
  operator_address    TEXT,     /* fee_bps recipient */
  pool_btc_address    TEXT,     /* pps-classic only; NULL in solo */
  pool_mode           TEXT,
  fee_bps             INTEGER,
  rate_source         TEXT,     /* 'derived' | 'override' */
  rate_sats_per_diff  REAL,     /* effective, net of fee; 0 in solo */
  gross_sats_per_diff REAL,     /* fair value before fee */
  effective_fee_bps   REAL,
  network_difficulty  REAL,
  block_value_sats    INTEGER,
  credited_from       INTEGER,
  /* Mirror of the proxy's in-memory events_lost counter: accepted shares that
   * never reached the DB after every commit retry failed. Must be 0 — it is
   * work a miner was told was accepted and that no query can otherwise see. */
  events_lost         INTEGER NOT NULL DEFAULT 0,  /* unix seconds */
  updated_at          INTEGER   /* unix seconds */
);

/* Append-only log of every distinct PPS rate the proxy has paid at.
 *
 * pool_meta holds one row and is overwritten on every template, so the rate
 * a share was credited at is not recoverable from it after the fact. This
 * table keeps the provenance: what the rate was, and the inputs it was
 * derived from, so an auditor can check the rate itself was fair — not just
 * that the arithmetic was applied consistently (which shares.rate_used
 * already proves on its own).
 *
 * A row is appended only when the tuple actually changes, so on a chain with
 * a quiet mempool this stays small; on a busy one it approaches one row per
 * template. Safe to prune: per-share verification does not depend on it. */
CREATE TABLE IF NOT EXISTS rate_history (
  id                  INTEGER PRIMARY KEY AUTOINCREMENT,
  ts                  INTEGER NOT NULL,  /* unix seconds, when it took effect */
  rate_sats_per_diff  REAL    NOT NULL,  /* net of fee — matches shares.rate_used */
  gross_sats_per_diff REAL    NOT NULL,  /* fair value before fee */
  fee_bps             INTEGER NOT NULL,
  network_difficulty  REAL    NOT NULL,
  block_value_sats    INTEGER NOT NULL,
  rate_source         TEXT    NOT NULL   /* 'derived' | 'override' */
);
CREATE INDEX IF NOT EXISTS rate_history_ts_idx   ON rate_history(ts);
CREATE INDEX IF NOT EXISTS rate_history_rate_idx ON rate_history(rate_sats_per_diff);

/* What the pool is actually mining, and what it mined before.
 *
 * One row per materially distinct template: a new tip, new nBits, a different
 * source or a different coinbase shape opens a row. Repeated polls fold into
 * the row they match, refreshing the block value / tx set / rate and bumping
 * `polls` and `last_seen`, so each row is the *span* over which one template
 * was mined rather than a single instant. The block value is deliberately not
 * part of that identity — it drifts with every mempool tick, and keying on it
 * appended a near-duplicate row per poll, thousands a day of fee churn at a
 * height already recorded.
 *
 * `source` records where the template came from. 'enforcer' means the backend
 * dictated the coinbase (BIP22 "coinbasetxn"), which is how the mandatory
 * BIP300/301 commitments reach the block; 'bitcoind' means the pool built its
 * own coinbase and the block carries none. `cb_op_returns` makes that concrete:
 * one is a bare witness commitment, more means sidechain commitments are
 * present. A pool mining 'bitcoind' templates cannot have any sidechain
 * merge-mined into its blocks, which is invisible from every other view. */
CREATE TABLE IF NOT EXISTS templates (
  id                  INTEGER PRIMARY KEY AUTOINCREMENT,
  ts                  INTEGER NOT NULL,  /* unix seconds, first seen */
  height              INTEGER NOT NULL,  /* height this template builds */
  prev_hash           TEXT    NOT NULL,
  bits                TEXT    NOT NULL,  /* nbits, hex */
  network_difficulty  REAL    NOT NULL,
  coinbase_value_sats INTEGER NOT NULL,  /* subsidy + fees */
  tx_count            INTEGER NOT NULL,
  tx_fees_sats        INTEGER NOT NULL,
  source              TEXT    NOT NULL,  /* 'enforcer' | 'bitcoind' */
  cb_spendable        INTEGER NOT NULL,  /* coinbase outputs, 0 when we build it */
  cb_op_returns       INTEGER NOT NULL,
  longpoll            INTEGER NOT NULL,  /* 1 when the server long-polls */
  rate_sats_per_diff  REAL    NOT NULL,  /* PPS rate derived from this template */
  last_seen           INTEGER NOT NULL DEFAULT 0,  /* unix seconds, last poll */
  polls               INTEGER NOT NULL DEFAULT 1   /* polls folded into this row */
);
CREATE INDEX IF NOT EXISTS templates_ts_idx     ON templates(ts);
CREATE INDEX IF NOT EXISTS templates_height_idx ON templates(height);

/* PPS accrual ledger. One row per worker. The C proxy only INCREMENTs
 * accrued_sats; a separate payout service updates paid_sats after
 * issuing Thunder transactions for (accrued_sats - paid_sats). Empty in
 * solo mode. */
CREATE TABLE IF NOT EXISTS pps_credits (
  worker_id     INTEGER PRIMARY KEY REFERENCES workers(id),
  accrued_sats  INTEGER NOT NULL DEFAULT 0,
  paid_sats     INTEGER NOT NULL DEFAULT 0,
  last_updated  INTEGER NOT NULL
);

/* Ledger of operator-triggered mainchain → Thunder deposits, used by
 * pool_mode=pps-classic. The C proxy does not touch this table; the
 * admin dashboard + a helper CLI are the writers. */
CREATE TABLE IF NOT EXISTS deposits (
  id                INTEGER PRIMARY KEY AUTOINCREMENT,
  ts                INTEGER NOT NULL,          /* unix seconds */
  btc_txid          TEXT    NOT NULL,          /* mainchain deposit tx */
  sats_deposited    INTEGER NOT NULL,
  fee_sats          INTEGER NOT NULL,
  thunder_recipient TEXT    NOT NULL,          /* bare base58 Thunder addr */
  ctip_seq_before   INTEGER,
  ctip_seq_after    INTEGER,
  notes             TEXT
);
CREATE INDEX IF NOT EXISTS deposits_ts_idx ON deposits(ts);

/* Permanent ledger of successful miner payouts. One row per settled
 * Thunder transfer — populated by the payout worker inside the same
 * atomic finalize transaction that increments pps_credits.paid_sats
 * and drops the payouts_in_flight row. Only append; never mutate
 * after write (audit trail). Empty in solo mode. */
CREATE TABLE IF NOT EXISTS payouts (
  id           INTEGER PRIMARY KEY AUTOINCREMENT,
  worker_id    INTEGER NOT NULL REFERENCES workers(id),
  sats         INTEGER NOT NULL,
  fee_sats     INTEGER NOT NULL,
  txid         TEXT    NOT NULL,        /* Thunder tx id (hex) */
  paid_at      INTEGER NOT NULL,        /* unix seconds */
  note         TEXT                     /* 'manual' for hand-driven, else NULL */
);
CREATE INDEX IF NOT EXISTS payouts_worker_ts_idx ON payouts(worker_id, paid_at);
CREATE INDEX IF NOT EXISTS payouts_paid_at_idx   ON payouts(paid_at);

/* Proportional / PPLNS deferred-claim ledger. NOT a balance: the pool holds no
 * funds. claim_fraction is a signed fraction of one block reward — positive
 * means the address was skipped (its cut fell below prop_min_payout_sats, or it
 * was demoted to keep the block under the output/weight cap) and is owed that
 * fraction of a future block; negative means it was paid early, covering
 * someone else's skipped share. The ledger sums to zero. See src/pplns.h for
 * why this is a fraction and not sats or raw difficulty. address is the miner's
 * payout_address from the workers table. */
CREATE TABLE IF NOT EXISTS prop_ledger (
  address         TEXT PRIMARY KEY,
  claim_fraction  REAL NOT NULL DEFAULT 0,
  last_settled_ts INTEGER
);
CREATE INDEX IF NOT EXISTS prop_ledger_ts_idx ON prop_ledger(last_settled_ts);

/* In-flight payout ledger. The payout worker INSERTs a row before
 * broadcasting a Thunder transaction; on successful broadcast it
 * atomically (in one tx) sets txid, increments pps_credits.paid_sats,
 * and DELETEs the row. The C proxy does not touch this table.
 *
 * Crash semantics:
 *   - Row exists with txid='' → the broadcast may or may not have
 *     happened; needs manual reconciliation. listDue skips workers
 *     that have an in-flight row so we never double-pay.
 *   - Row exists with txid set → the broadcast went out and we crashed
 *     before the DELETE finished. The finalize tx is idempotent (its
 *     paid_sats UPDATE is fenced by the row's existence), so a startup
 *     sweep can finish it.
 */
CREATE TABLE IF NOT EXISTS payouts_in_flight (
  id            INTEGER PRIMARY KEY AUTOINCREMENT,
  worker_id     INTEGER NOT NULL REFERENCES workers(id),
  sats          INTEGER NOT NULL,
  txid          TEXT NOT NULL DEFAULT '',
  started_at    INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS payouts_in_flight_worker_idx ON payouts_in_flight(worker_id);

/* Every attempt to broadcast a transaction, successful or not. Owned by the
 * dashboard and the payout worker; the C proxy never writes here.
 *
 * `deposits` and `payouts` record what actually happened. A failed broadcast
 * is neither, but it is the thing an operator most needs to see — so it
 * lands here instead, with the raw transaction whenever it can be recovered.
 * Without this a failure left nothing behind but a truncated flash message.
 *
 * raw_tx is the full hex when obtainable. For a deposit that failed at
 * broadcast the enforcer has still signed and stored the tx, so it can be
 * recovered afterwards via ListSidechainDepositTransactions; `stage` records
 * how far the attempt got. */
CREATE TABLE IF NOT EXISTS tx_attempts (
  id          INTEGER PRIMARY KEY AUTOINCREMENT,
  ts          INTEGER NOT NULL,   /* unix seconds */
  kind        TEXT    NOT NULL,   /* 'deposit' | 'payout' */
  status      TEXT    NOT NULL,   /* 'broadcast' | 'failed' */
  stage       TEXT,               /* step reached when it failed */
  txid        TEXT,
  raw_tx      TEXT,               /* full hex, when recoverable */
  amount_sats INTEGER,
  fee_sats    INTEGER,
  destination TEXT,
  worker_id   INTEGER,            /* payouts only */
  error       TEXT,               /* full, never truncated */
  detail      TEXT                /* JSON: request params */
);
CREATE INDEX IF NOT EXISTS tx_attempts_ts_idx   ON tx_attempts(ts);
CREATE INDEX IF NOT EXISTS tx_attempts_kind_idx ON tx_attempts(kind, ts);
