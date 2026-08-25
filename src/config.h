#ifndef SIMPLEPOOL_CONFIG_H
#define SIMPLEPOOL_CONFIG_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    /* listener */
    char listen_addr[64];
    int  listen_port;
    int  max_conns;
    /* listen() backlog for both stratum listeners. 0 -> STRATUM_DEFAULT_BACKLOG.
     * Raise only; the kernel clamps to net.core.somaxconn. */
    int  listen_backlog;
    double initial_diff;

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

    /* Reaper budget for a connection that HAS authorized. Default 3600.
     *
     * ⚠️ The reaper measures inbound silence, and a miner with nothing to
     * submit sends nothing — so at a high difficulty floor a perfectly
     * healthy small rig is indistinguishable from a half-open socket.
     * At rental_min_diff=500000 a share takes 500000*2^32/H seconds, so a
     * 600s budget reaps a 3.6 TH/s rig 37% of the time and a 1 TH/s rig
     * 76% of the time. It then reconnects and is reaped again: the miner
     * reads a permanent 0 TH/s. Observed in production 2026-08-24, where
     * every reaped connection had lived EXACTLY 604s.
     *
     * Keeping the two budgets separate preserves what the short one is
     * actually for — scanners and half-open TCPs that never authenticate —
     * without applying it to miners that are demonstrably alive, since the
     * job broadcast keeps succeeding on their socket. */
    int    idle_timeout_authorized_sec;  /* default 3600 (1 h) */

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
