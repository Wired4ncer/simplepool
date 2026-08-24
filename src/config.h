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

    /* logging */
    int  log_level;            /* 0..3 */
} proxy_config_t;

void proxy_config_defaults(proxy_config_t *cfg);
int  proxy_config_load(const char *path, proxy_config_t *cfg,
                       char *errbuf, size_t errlen);

#endif /* SIMPLEPOOL_CONFIG_H */
