#ifndef SIMPLEPOOL_CONFIG_H
#define SIMPLEPOOL_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#include "stratum.h"   /* stratum_listener_t, STRATUM_MAX_LISTENERS */

typedef struct {
    /* listener */
    char listen_addr[64];
    int  listen_port;
    int  max_conns;
    /* Still 1: the difficulty policy lives on the listener now, so a config
     * naming no listeners behaves exactly as it always did. A rental port
     * sets its own via `listener = port=3335 min_diff=65536`. */

    /* listen() backlog for every stratum listener. 0 -> STRATUM_DEFAULT_BACKLOG.
     * Raise only; the kernel clamps to net.core.somaxconn.
     * ⚠️ OURS — no upstream equivalent. 64 was silently dropping SYNs under a
     * marketplace burst (784 ListenOverflows, no log line, no counter). */
    int  listen_backlog;
    double initial_diff;

    /* Extra stratum ports beyond listen_port, each with its own difficulty
     * policy — see `listener` in proxy.conf.example. The point is serving a
     * home ASIC and a rented fleet from the same pool without either one
     * getting the other's difficulty. */
    stratum_listener_t listeners[STRATUM_MAX_LISTENERS];
    int  listener_count;

    /* Per-connection ceiling on mining.submit per second. 0 disables.
     * See stratum.h for why it sits where it does. */
    int  max_submits_per_sec;

    /* Kill switch for the `sd=<n>` static-difficulty request. Default 0 = OFF.
     *
     * ⚠️ Deliberately NOT shared with max_suggested_diff. That knob also gates
     * the `d=` FLOOR request, which real miners use today — so turning pins off
     * by way of it would also disable floors for everyone, and a switch you
     * cannot afford to throw is not a kill switch. */
    int  static_diff_enabled;

    /* 🔴 The minimum a `sd=` pin may resolve to. Default 16384.
     *
     * This exists so the pin floor does NOT depend on vardiff_min. vardiff_min
     * defaults to 1, and a pin at 1 from a large miner is the denial-of-service
     * the whole design has been arguing about — bounded only by
     * max_submits_per_sec, and a REFUSED submit still costs a socket read, a
     * full cJSON_Parse and a reply, because the ceiling is checked inside
     * handle_submit after the message is already parsed. So the ceiling caps
     * share processing, not message processing.
     *
     * Making the floor a value this feature owns means the guard stops
     * depending on a config nobody verified. → feedback_a-guard-can-disable-what-it-guards
     *
     * Why 16384: at 400 TH/s a pin here offers ~5.7 shares/sec, comfortably
     * under a 120/s ceiling; and it is servable for the miners actually asking
     * (the worst-rejecting worker on the pool requests exactly 16384 today), so
     * the feature works at its default instead of inviting an operator to lower
     * it blind.
     *
     * ⚠️ THE THRESHOLD, so the residual is a number rather than a hand-wave. A
     * pin here saturates the 120/s submit ceiling with VALID shares at
     * 120 * 16384 * 2^32 ~= 8.4 PH/s. Above that a legitimate fleet pinned low
     * sustains a refusal flood vardiff would have corrected in two windows, and
     * ~900 PH/s has been seen on this pool — so it is live, not theoretical.
     * ⛔ Do NOT claim marketplace ports contain this. The rental port is a
     * convention; nothing stops a fleet arriving on the public port anyway.
     *
     * 📌 What `sd=` does NOT add is adversarial capability: the per-submit cost
     * (read, full cJSON_Parse, error object, reply) is paid before the ceiling
     * check, so any TCP client can inflict it today with zero hashrate and no
     * pin. The real fix for that is checking the ceiling BEFORE the parse, or
     * stopping reads on an over-ceiling connection so TCP backpressure moves
     * the cost to the sender — pre-existing, upstreamable, not this feature. */
    int  static_diff_min;

    /* vardiff — auto-adjust each connection's difficulty to keep the
     * share rate near `target_spm` shares/minute. Set vardiff_enabled = 0
     * to pin every connection to initial_diff (the legacy behaviour). */
    int    vardiff_enabled;       /* default 1 */
    double vardiff_target_spm;    /* default 12 shares/min (one every 5s) */
    double vardiff_min;           /* default 1.0 */
    double vardiff_max;           /* default 1e12; clamped by network diff */
    int    vardiff_window_sec;    /* retarget interval, default 30 */

    /* Minimum accepted shares a window must contain before its rate is
     * trusted enough to retarget on. Default 20.
     *
     * At target_spm=12 and a 30s window an ON-TARGET connection produces
     * SIX shares, and Poisson noise on six samples is +/-41% (1/sqrt(6)).
     * The ratio therefore lands outside the [0.5, 2.0] deadband routinely
     * even when the difficulty is already perfect, so the controller
     * oscillates instead of converging. Below this floor the window is
     * EXTENDED rather than acted on — up to vardiff_max_window_mult times
     * the nominal window, after which we act on what we have so a
     * genuinely over-difficult connection still ratchets down. */
    int    vardiff_min_samples;   /* default 20 */

    /* Ceiling on a miner-requested difficulty (stratum password `d=<n>` or
     * mining.suggest_difficulty). Default 50000000.
     *
     * ⚠️ <= 0 DISABLES requests; it does not mean "no ceiling". An
     * uncapped request is not a safe state -- a miner naming a difficulty
     * far above its hashrate simply stops submitting and is reaped -- so
     * the off position and the unbounded position are deliberately the
     * same switch. A disabled request is still logged, which is what makes
     * a staged rollout able to measure existing `d=` usage first.
     *
     * ⚠️ Size this against idle_timeout_authorized_sec, not against taste.
     * A request is a floor on the connection's difficulty, so it directly
     * lengthens that connection's expected share interval — and the reaper
     * measures inbound silence. At 50M a 25 TH/s connection expects a share
     * every ~8600s and would be reaped from a 3600s budget. Raise one and
     * you must consider the other. */
    double max_suggested_diff;    /* default 5e7 */

    /* How far a window may be extended, as a multiple of vardiff_window_sec,
     * while waiting for vardiff_min_samples. Default 8. */
    int    vardiff_max_window_mult;  /* default 8 */

    /* Max step for a window that did NOT meet vardiff_min_samples. A window
     * that met it keeps the historical 4x. Default 2.
     *
     * This is what stops an idle connection being driven to the floor: a
     * proxied rental customer spreads one rig over many connections, each
     * of which goes quiet between bursts, and a zero-share window otherwise
     * cuts difficulty 4x — twice in a row is 16x. */
    double vardiff_idle_step;     /* default 2.0 */

    /* Set clean_jobs=true on the periodic same-tip template refresh as well
     * as on a real new block. Default 1 preserves the historical behaviour;
     * 0 is correct and is what a hashrate marketplace requires, because its
     * proxy flushes its whole fleet on the flag. Kept as a switch so the
     * behaviour can be reverted without rebuilding the binary. */
    int    clean_jobs_on_refresh; /* default 1 */

    /* Rental port — a SECOND stratum listener for hashrate marketplaces
     * (Braiins Hashpower, NiceHash). 0 disables it entirely, which is the
     * default and leaves the pool exactly as it was.
     *
     * It exists because rented hashrate does not arrive as many small miners;
     * it arrives as one aggregated worker with a hard minimum share
     * difficulty. Braiins floors at 1024 and recommends 65536; NiceHash
     * requires 500000. Serving those on the public port would price out every
     * home miner, so they get their own listener, and shares from both feed
     * the same PPLNS window.
     *
     * ⛔ Do NOT try to serve a marketplace by letting vardiff ramp up to the
     * floor. It starts a connection below the minimum and every operator who
     * has tried it had the order cancelled for invalid shares before the ramp
     * finished. rental_min_diff is applied as BOTH the starting difficulty and
     * the vardiff floor, so the very first share is already at it.
     *
     * ⚠️ The network-difficulty clamp still wins over this floor. During a
     * minimum-difficulty window (a fork resets difficulty to powLimit) the
     * chain cannot support the floor, connections get clamped below it, and
     * marketplace orders will reject-flood until difficulty ramps back. That
     * is a known, time-bounded limitation, not a misconfiguration. */
    int    rental_listen_port;    /* default 0 = disabled */
    double rental_min_diff;       /* default 500000 (clears Braiins + NiceHash) */
    int    rental_max_conns;      /* default: same as max_conns */

    /* Idle-connection reaper. A connection that hasn't sent any bytes in
     * idle_timeout_sec is closed. Guards against half-open TCPs from
     * crashed miners and clients that connect but never authenticate.
     * Set to a negative value to disable entirely; 0 uses the default. */
    int    idle_timeout_sec;      /* default 600 (10 min) */

    /* The same reaper, but for a connection that has authorized. It is a
     * separate (much longer) budget because the two cases are not the same
     * risk: an unauthorized socket is a squatter and costs an fd for nothing,
     * whereas an authorized miner that has sent nothing is usually just a
     * small rig that has not found a share at its assigned difficulty yet.
     * The pool never solicits anything from a miner, so a healthy ASIC has no
     * reason to speak between shares — reaping it at 10 minutes disconnects
     * working hashrate, which is exactly the behaviour marketplaces blacklist
     * pools for. TCP keepalive (2 min idle + 3x30s probes) already reaps a
     * genuinely dead socket in ~3.5 min, so this only needs to catch a peer
     * that is answering keepalives while doing no work.
     * Negative disables; 0 uses the default. */
    int    idle_timeout_authorized_sec;  /* default 7200 (2 h) */

    /* bitcoind */
    char bitcoind_url[512];
    char bitcoind_user[128];
    char bitcoind_pass[256];
    int  bitcoind_poll_interval_ms;

    /* coinbase */
    char operator_address[128];   /* 1% (fee_bps) fee recipient */
    int  fee_bps;                 /* basis points, default 100 (=1%), cap 1000 */
    char coinbase_tag[64];

    /* sqlite */
    char db_path[512];
    int  commit_window_ms;
    int  commit_max_shares;
    int  templates_retention_days;  /* template history kept; 0 = forever */

    /* redis broadcast — optional. Empty url disables the module. */
    char redis_url[256];
    int  redis_publish_timeout_ms;
    int  redis_reconnect_backoff_ms;

    /* PPS mode. pool_mode = "solo" (default) preserves the per-block
     * direct-payout flow: each miner's coinbase pays that miner. pool_mode
     * = "pps-classic" pays every block into a single pool-owned BTC wallet
     * and credits each accepted share to the worker's pps_credits row; the
     * operator later batches that BTC into Thunder via the admin
     * dashboard's deposit action, and the payout worker drains the Thunder
     * reserve to miners. */
    char pool_mode[16];                       /* "solo" | "pps-classic" | "proportional" */
    /* pps-classic: coinbase pays this BTC address (P2WPKH/P2PKH/P2SH) for
     * the net-of-fee reward. Required when pool_mode = pps-classic;
     * ignored otherwise. */
    char pool_btc_address[128];
    /* Proportional / coinbase-direct PPLNS settings.
     *
     * window_difficulty = prop_window_k * current_network_difficulty. The PPLNS
     * window walks back from the current tip over the newest shares until the
     * cumulative difficulty reaches window_difficulty. Shares are grouped by
     * payout address.
     *
     * prop_min_payout_sats is the minimum a coinbase output may pay; smaller
     * amounts are carried forward in prop_balances and paid once the balance
     * crosses the threshold.
     *
     * prop_max_outputs caps the number of payout outputs per block to keep the
     * block within the 4 MWU weight limit; the smallest payouts are carried
     * forward until under the cap. */
    double prop_window_k;
    int64_t prop_min_payout_sats;
    int prop_max_outputs;
    /* prop_max_coinbase_bytes caps the SERIALIZED SIZE of the coinbase, in
     * bytes, as a second limit alongside prop_max_outputs. 0 disables it.
     *
     * Why a byte cap when there is already a count cap: prop_max_outputs is a
     * COUNT, and the same count is a different size depending on the address
     * types being paid. A P2WPKH payout is 31 bytes on the wire and a taproot
     * one is 43. So 16 payouts is ~702 B today and would be ~906 B if the same
     * 16 miners moved to bc1p addresses -- a 29% growth that no configuration
     * change caused and nothing would report.
     *
     * That matters because a hashrate marketplace validates our coinbase
     * before it will place an order. NiceHash's verificator rejected this pool
     * at 919 B and accepted it at 702 B; where in between it breaks cannot be
     * measured, because the endpoint needs an account we do not have. So the
     * risk is not that a block becomes invalid -- the weight budget is a
     * separate and much looser limit -- it is that orders silently stop being
     * placeable, weeks after the change that caused it, with no error anywhere.
     *
     * With this set, a taproot-heavy payout set simply gets FEWER SLOTS and the
     * smallest payouts carry forward, exactly as they do when the count cap
     * binds. That turns an unbounded external dependency into a bounded
     * internal one we control. */
    int prop_max_coinbase_bytes;
    /* Floor on how far back the window reaches, in seconds. The window is
     * whichever is LARGER: prop_window_k blocks of work, or this many seconds
     * of shares.
     *
     * Without a floor the window collapses at exactly the moment this pool
     * exists for. Difficulty resets to powLimit at each fork, so network
     * difficulty is ~1 and a k of 3 makes the window three difficulty units —
     * about three shares from one small miner, a couple of seconds of history.
     * Everyone else falls outside it and payouts go lumpy. A time floor keeps
     * the window meaningful whatever the difficulty does. */
    int prop_window_min_sec;
    /* PPS rate override — sats credited per unit of share difficulty.
     *
     * Leave unset (0) and the proxy derives the rate from each block
     * template as (coinbasevalue / network_difficulty) * (1 - fee_bps/1e4).
     * That is the recommended configuration: fee_bps becomes the single
     * knob controlling the fee, and the rate tracks difficulty instead of
     * going stale.
     *
     * Set it and the value is used verbatim and taken to be ALREADY NET of
     * fee — fee_bps is not applied on top, because historically operators
     * baked the fee into this number by hand. The proxy logs the fee that
     * choice actually implies and warns when it disagrees with fee_bps.
     * A static value silently drifts as difficulty moves, and can invert
     * into paying miners more than the pool earns, so prefer derived. */
    double pps_sats_per_diff;

    /* Minimum network difficulty at which PPS accrual is allowed to run.
     *
     * The PPS rate is block_value / network_difficulty, which is the expected
     * value of a share — correct only while every share the pool produces has
     * an independent chance of becoming a block. That holds when difficulty is
     * calibrated to hashrate. It stops holding when the pool produces
     * solutions faster than the chain accepts blocks, and then the rate is
     * overstated by exactly that ratio.
     *
     * The threshold is the difficulty at which this pool ALONE would find one
     * block per block interval:
     *
     *     min_difficulty = pool_hashrate * block_interval_sec / 2^32
     *
     * A 40 TH/s pool on a 600s chain needs difficulty >= ~5,600,000. Below it,
     * accrual is refused. That is a floor, not a target: the pool shares the
     * chain with other miners, so the genuinely safe difficulty is higher.
     *
     * 0 disables the check, which is only safe on a chain whose difficulty is
     * already calibrated — mainnet, testnet, signet. On a young forknet during
     * its difficulty ramp, leaving this at 0 is how a pool accrues millions of
     * BTC of liability in minutes. The proxy logs the value it observes to be
     * necessary, so a wrong setting is visible rather than silent. */
    double pps_min_network_difficulty;

    /* Target seconds between blocks on this chain — 600 for Bitcoin and every
     * chain derived from it. Used for the difficulty floor above and for the
     * issuance ceiling, which caps accrual at what the chain can actually mint
     * (one block_value per interval, across all miners on earth). */
    int block_interval_sec;

    /* Refuse mining.authorize while accrual is gated off by the floor.
     *
     * Default on, and deliberately so: a miner whose shares are accepted but
     * not credited is working for free without being told. Turning it off
     * accepts shares that earn nothing, which is only reasonable if the miners
     * are yours and you know why. */
    int pps_refuse_shares_below_min;

    /* logging */
    int  log_level;            /* 0..3 */
} proxy_config_t;

void proxy_config_defaults(proxy_config_t *cfg);
int  proxy_config_load(const char *path, proxy_config_t *cfg,
                       char *errbuf, size_t errlen);

#endif /* SIMPLEPOOL_CONFIG_H */
