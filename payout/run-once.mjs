/* One-shot payout tick — same config surface as index.js, but runs a
 * single runOnce() and exits instead of looping. For operators who want
 * cron-style control, and for the payout integration test.
 *
 * Exit codes: 0 = tick completed with no failed payouts,
 *             1 = at least one payout failed,
 *             2 = bad config.
 * Prints the tick summary as one JSON line on stdout.
 *
 * Meant for when the service is NOT running. runOnce() is single-flight only
 * within one process, so this is a second, unserialised run beside a live
 * service. That is still safe for the money — beginBatch and finalizeBatch
 * refuse inside their transactions when another run got there first, so a
 * batch is neither broadcast nor credited twice — but this run may lose that
 * race and do nothing, or catch the service mid-broadcast and report its rows
 * as unresolved. With the service up, use the dashboard's "Trigger payout now"
 * (POST /tick), which joins the service's own lock. See payout/README.md ->
 * One run at a time. */

import { loadConfig } from './lib/config.js';
import { openDb } from './lib/db.js';
import { ThunderClient } from './lib/thunder.js';
import { runOnce, reportStuck } from './lib/payout.js';

const cfg = loadConfig();

const log = {
    debug: (m) => process.env.PAYOUT_DEBUG === '1' && console.error(`[debug] ${m}`),
    info:  (m) => console.error(`[info]  ${m}`),
    warn:  (m) => console.error(`[warn]  ${m}`),
    error: (m) => console.error(`[error] ${m}`),
};

const db      = openDb(cfg.dbPath);
const thunder = new ThunderClient({
    url:  cfg.rpcUrl,
    user: cfg.rpcUser,
    pass: cfg.rpcPass,
});

reportStuck({ db }, log);
const res = await runOnce({ db, thunder, cfg }, log);
db.close();

console.log(JSON.stringify(res));
process.exit(res.failed > 0 ? 1 : 0);
