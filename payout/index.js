/* simplepool-payout — Thunder payout worker.
 *
 * Long-running service that drains pps_credits.accrued_sats - paid_sats
 * by sending Thunder transactions to each miner. Reads/writes the same
 * SQLite database the C proxy owns; coordinates via SQLite's WAL lock.
 *
 * Config is environment-only — see lib/config.js for the full list.
 *
 * Run:
 *   PAYOUT_DB_PATH=../data/shares.db \
 *   THUNDER_RPC_URL=http://127.0.0.1:6009 \
 *   THUNDER_FROM_ADDRESS=<pool base58 thunder addr> \
 *   node index.js
 *
 * Dry-run (no Thunder txs, no DB writes):
 *   PAYOUT_DRY_RUN=1 node index.js
 */

import { loadConfig }   from './lib/config.js';
import { openDb }       from './lib/db.js';
import { ThunderClient } from './lib/thunder.js';
import { startLoop, reportStuck, humanMs } from './lib/payout.js';
import { startAdminHttp } from './lib/admin-http.js';

const cfg = loadConfig();

const log = {
    debug: (m) => process.env.PAYOUT_DEBUG === '1' && console.log(`[debug] ${m}`),
    info:  (m) => console.log(`[info]  ${m}`),
    warn:  (m) => console.warn(`[warn]  ${m}`),
    error: (m) => console.error(`[error] ${m}`),
};

log.info(`simplepool-payout starting (db=${cfg.dbPath} rpc=${cfg.rpcUrl}` +
         `${cfg.dryRun ? ' DRY-RUN' : ''})`);
log.info(`  payout run every ${humanMs(cfg.intervalMs)} ` +
         `(settle re-check ${humanMs(cfg.settleIntervalMs)}, ` +
         `retry ${humanMs(cfg.retryIntervalMs)})`);
log.info(`  min_sats=${cfg.minSats} max_per_tick=${cfg.maxPerTick}`);

const db      = openDb(cfg.dbPath);
const thunder = new ThunderClient({
    url:  cfg.rpcUrl,
    user: cfg.rpcUser,
    pass: cfg.rpcPass,
});

/* Surface any in-flight rows older than 5 minutes — they're a crashed
 * payout that needs manual reconciliation. We never auto-resolve them
 * because we can't safely tell apart "broadcast didn't happen" from
 * "broadcast happened, finalize crashed". */
reportStuck({ db }, log);

const loop = startLoop({ db, thunder, cfg }, log);

/* Small HTTP admin surface for the dashboard's "Trigger payout now"
 * button. port=0 disables. Loopback-only unless overridden. */
let adminHttp = null;
if (cfg.adminHttpPort > 0) {
    adminHttp = startAdminHttp({
        port: cfg.adminHttpPort,
        bind: cfg.adminHttpBind,
        ctx:  { db, thunder, cfg },
        log,
    });
}

for (const sig of ['SIGINT', 'SIGTERM']) {
    process.on(sig, () => {
        log.info(`got ${sig}, stopping`);
        loop.stop();
        adminHttp?.close();
        db.close();
        process.exit(0);
    });
}
