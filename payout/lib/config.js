/* Payout worker config loaded from environment variables.
 *
 * Required:
 *   PAYOUT_DB_PATH         path to data/shares.db (writable; we UPDATE
 *                          pps_credits.paid_sats on successful tx)
 *   THUNDER_RPC_URL        Thunder node JSON-RPC endpoint (e.g.
 *                          http://127.0.0.1:6000)
 *
 * Optional:
 *   PAYOUT_INTERVAL_MS     how often to start a payout run (default 24h).
 *                          This is the batch cadence miners see: once a day
 *                          everyone over PAYOUT_MIN_SATS goes out in one
 *                          transaction. It deliberately does NOT govern what
 *                          happens to a batch already broadcast — see
 *                          PAYOUT_SETTLE_INTERVAL_MS.
 *   PAYOUT_SETTLE_INTERVAL_MS
 *                          how often to re-check a broadcast batch that has
 *                          not confirmed yet (default 30s). Nobody in a batch
 *                          is credited until a tick sees it in a Thunder
 *                          block, so this has to stay short even when the
 *                          payout cadence is daily.
 *   PAYOUT_RETRY_INTERVAL_MS
 *                          how long to wait after a tick that tried and got
 *                          nowhere — transfer failed, or the reserve could
 *                          not cover what is owed (default 5m). Nothing was
 *                          broadcast and nobody was credited, so waiting a
 *                          full day to try again would strand the queue.
 *   PAYOUT_MIN_SATS        skip workers below this owed balance (default 10000)
 *   PAYOUT_MAX_PER_TICK    cap workers paid per scan (default 50) to bound
 *                          tail latency and Thunder RPC load
 *   PAYOUT_DRY_RUN         '1' = log what would be sent, don't touch
 *                          Thunder or update paid_sats
 *   PAYOUT_NUDGE_MINE      '0' = never ask Thunder to mine. On by default:
 *                          Thunder advances only when a mainchain block
 *                          commits to it and nothing schedules that, so a
 *                          broadcast payout otherwise waits for someone to
 *                          press a button. Fires only while a payout is
 *                          actually waiting, so an idle pool spends no BMM
 *                          bids on empty blocks.
 *   PAYOUT_NUDGE_INTERVAL_MS
 *                          floor between stall-recovery mine attempts
 *                          (default 120s). Each nudge costs a mainchain BMM
 *                          bid. Does NOT apply to the nudge issued right after
 *                          a broadcast, which must always fire.
 *   PAYOUT_NUDGE_STALL_SEC how long a broadcast batch may sit unconfirmed
 *                          before we assume its BMM request was not carried
 *                          and nudge again (default 300s, ~2 mainchain
 *                          blocks on drynet3). Do NOT lower this to the tick
 *                          interval: nudging on every tick makes Thunder park
 *                          a mempool snapshot that predates the next batch,
 *                          costing one extra sidechain block per payout. See
 *                          settlePending() for the mechanism.
 *   THUNDER_RPC_USER       optional basic-auth user
 *   THUNDER_RPC_PASS       optional basic-auth pass
 *   THUNDER_FROM_ADDRESS   pool reserve address to send from (must match
 *                          the dashboard's POOL_THUNDER_RESERVE_ADDRESS —
 *                          the wallet the operator deposits mined BTC into)
 */

function require_env(name) {
    const v = process.env[name];
    if (!v) {
        console.error(`fatal: ${name} is required`);
        process.exit(2);
    }
    return v;
}

export function loadConfig() {
    return {
        dbPath:        require_env('PAYOUT_DB_PATH'),
        rpcUrl:        require_env('THUNDER_RPC_URL'),
        rpcUser:       process.env.THUNDER_RPC_USER || null,
        rpcPass:       process.env.THUNDER_RPC_PASS || null,
        fromAddress:   require_env('THUNDER_FROM_ADDRESS'),
        /* Daily batch cadence. Settlement and retry run on their own,
         * much shorter clocks — see nextDelayMs() in payout.js. */
        intervalMs:       parseInt(process.env.PAYOUT_INTERVAL_MS        || '86400000', 10),
        settleIntervalMs: parseInt(process.env.PAYOUT_SETTLE_INTERVAL_MS || '30000',    10),
        retryIntervalMs:  parseInt(process.env.PAYOUT_RETRY_INTERVAL_MS  || '300000',   10),
        minSats:       BigInt(process.env.PAYOUT_MIN_SATS       || '10000'),
        maxPerTick:    parseInt(process.env.PAYOUT_MAX_PER_TICK || '50',    10),
        dryRun:        process.env.PAYOUT_DRY_RUN === '1',
        nudgeMine:     process.env.PAYOUT_NUDGE_MINE !== '0',
        nudgeIntervalMs: parseInt(process.env.PAYOUT_NUDGE_INTERVAL_MS || '120000', 10),
        nudgeStallSec:   parseInt(process.env.PAYOUT_NUDGE_STALL_SEC || '300', 10),
        /* Admin HTTP surface — used by the dashboard's "Trigger payout now"
         * button. Loopback-bound by default; set port=0 to disable. */
        adminHttpBind: process.env.PAYOUT_ADMIN_BIND || '127.0.0.1',
        adminHttpPort: parseInt(process.env.PAYOUT_ADMIN_PORT || '9080', 10),
    };
}
