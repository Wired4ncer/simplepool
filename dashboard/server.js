/* simplepool dashboard — two surfaces, one process:
 *
 *   - Public read-only:  /, /worker/:name, /blocks, /api/*
 *   - Admin:             /admin/* (Basic auth on every route)
 *
 * Admin routes live in lib/admin-router.js; server.js is the assembly:
 * open DB handles, mount routes, wire middleware.
 */

import express from 'express';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { renderFile } from 'ejs';
import { openDb } from './lib/db.js';
import { openAdminDb } from './lib/db-admin.js';
import * as stats from './lib/stats.js';
import { startHealthMonitor, currentHealth } from './lib/health.js';
import { versions } from './lib/versions.js';
import * as fmt from './lib/fmt.js';
import { createAdminRouter } from './lib/admin-router.js';
import { createHash, timingSafeEqual } from 'node:crypto';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const PORT     = parseInt(process.env.PORT || '8081', 10);
const DB_PATH  = process.env.PROXY_DB_PATH || '../data/shares.db';

const app = express();
app.engine('ejs', renderFile);
app.set('views', path.join(__dirname, 'views'));
app.set('view engine', 'ejs');
app.disable('x-powered-by');

/* --- security headers on every response --------------------------------- */
app.use((_req, res, next) => {
    /* No cookies here yet, but if any land later they must be same-origin. */
    res.setHeader('X-Content-Type-Options',  'nosniff');
    res.setHeader('X-Frame-Options',         'DENY');
    res.setHeader('Referrer-Policy',         'same-origin');
    res.setHeader('Permissions-Policy',      'interest-cohort=()');
    next();
});

/* --- fmt helpers on every render ---------------------------------------- */
app.use((_req, res, next) => {
    Object.assign(res.locals, fmt.all);
    /* Every page renders the banner, so every render needs the snapshot. */
    res.locals.health = currentHealth();
    /* Same reason: the pool-identity strip is in both navs. Cheap enough to
     * read per request — one row, by primary key, off a one-row table — and
     * reading it live means a proxy restart onto a different network shows
     * up on the next refresh instead of on the next dashboard restart. */
    res.locals.pool = stats.poolMeta(db);
    /* The about-numbers card needs both to tell a miner how to connect, and
     * it is a partial rather than an index-only block, so they live here
     * rather than being threaded through one render call. */
    res.locals.stratumUrl  = PUBLIC_STRATUM_URL;
    res.locals.sidechainId = THUNDER_SIDECHAIN_ID;
    next();
});

app.use('/static', express.static(path.join(__dirname, 'public'), { maxAge: '1h' }));

/* Two handles: read-only for public + admin read routes, writable one
 * confined to admin write actions (lives inside admin router). */
const db   = openDb(path.resolve(__dirname, DB_PATH));
const dbRw = openAdminDb(path.resolve(__dirname, DB_PATH));

/* Hard-failure checks, evaluated on a timer rather than per request: the
 * duplicate-hash scan measured 2.0s against 337k shares, and better-sqlite3
 * is synchronous, so running it on the request path would stall the whole
 * dashboard on every 15s auto-refresh. */
const HEALTH_INTERVAL_MS = parseInt(process.env.HEALTH_INTERVAL_MS || '300000', 10);
startHealthMonitor(db, { intervalMs: HEALTH_INTERVAL_MS });

/* --- public-side config ------------------------------------------------- */
const PUBLIC_STRATUM_URL = process.env.PUBLIC_STRATUM_URL || 'stratum+tcp://<pool-host>:3334';

/* The PPS rate is NOT configured here. It is read from pool_meta, which the
 * proxy writes on every template change, so the dashboard always reports the
 * rate that was actually applied rather than a second copy of the config
 * that can silently disagree with it. POOL_PPS_SATS_PER_DIFF is accepted
 * only to warn that it is now ignored. */
if (process.env.POOL_PPS_SATS_PER_DIFF) {
    console.warn('[warn] POOL_PPS_SATS_PER_DIFF is ignored — the rate is read ' +
                 'from the pool_meta table written by the proxy. Remove it ' +
                 'from the environment.');
}

/* --- admin-side config -------------------------------------------------- */
/* Credentials come from ADMIN_CREDENTIALS_FILE (a "user:password" file —
 * docker-secret friendly, same shape as the enforcer's RPC cookie) or from
 * the ADMIN_USER + ADMIN_PASSWORD env vars. The file takes precedence.
 * A set-but-unreadable/malformed file is a fatal boot error: crashing beats
 * silently running with admin disabled when the operator asked for it. */
function loadAdminCredentials() {
    const file = process.env.ADMIN_CREDENTIALS_FILE || '';
    let user, pass;
    if (!file) {
        user = process.env.ADMIN_USER || '';
        pass = process.env.ADMIN_PASSWORD || '';
    } else {
        let raw;
        try {
            raw = fs.readFileSync(file, 'utf8');
        } catch (e) {
            throw new Error(`ADMIN_CREDENTIALS_FILE ${file}: ${e.message}`);
        }
        /* Strip trailing newlines only — `echo user:pass > file` and
         * docker secrets end with one. Spaces stay significant. */
        raw = raw.replace(/[\r\n]+$/, '');
        const sep = raw.indexOf(':');
        if (sep < 1 || sep === raw.length - 1) {
            throw new Error(`ADMIN_CREDENTIALS_FILE ${file}: expected "user:password"`);
        }
        user = raw.slice(0, sep);
        pass = raw.slice(sep + 1);
    }
    /* HTTP Basic auth uses ':' as the user/password separator, so a colon
     * in either would silently truncate at login time. Refuse at boot. */
    if (user.includes(':') || pass.includes(':')) {
        throw new Error('admin credentials must not contain ":" ' +
                        '(HTTP Basic auth uses it as the separator)');
    }
    return { user, pass };
}
const { user: ADMIN_USER, pass: ADMIN_PASS } = loadAdminCredentials();
const RESERVE_ADDRESS      = process.env.POOL_THUNDER_RESERVE_ADDRESS || '(unset)';
const THUNDER_RPC_URL      = process.env.THUNDER_RPC_URL      || 'http://127.0.0.1:6009';
const PAYOUT_ADMIN_URL     = process.env.PAYOUT_ADMIN_URL     || '';
const ENFORCER_GRPC_ADDR   = process.env.ENFORCER_GRPC_ADDR   || '127.0.0.1:50051';
const THUNDER_SIDECHAIN_ID = parseInt(process.env.THUNDER_SIDECHAIN_ID || '9', 10);

/* Constant-time credential comparison.
 *
 * `===` on strings short-circuits at the first differing byte, so response
 * timing leaks the length and a prefix of the password. Hashing both sides
 * first means timingSafeEqual always gets equal-length buffers (it throws
 * otherwise) and the length itself does not leak. This surface authorises
 * real Thunder deposits and payouts. */
function credEq(a, b) {
    const h = v => createHash('sha256').update(String(v ?? '')).digest();
    return timingSafeEqual(h(a), h(b));
}

function requireAdminAuth(req, res, next) {
    if (!ADMIN_USER || !ADMIN_PASS) {
        return res.status(503).send('admin disabled — set ADMIN_USER + ADMIN_PASSWORD or ADMIN_CREDENTIALS_FILE');
    }
    const h = req.headers.authorization || '';
    if (h.startsWith('Basic ')) {
        const [u, p] = Buffer.from(h.slice(6), 'base64').toString('utf8').split(':');
        if (credEq(u, ADMIN_USER) && credEq(p, ADMIN_PASS)) return next();
    }
    res.setHeader('WWW-Authenticate', 'Basic realm="simplepool admin"');
    return res.status(401).send('unauthorised');
}

/* ================================ PUBLIC ================================ */

app.get('/', (_req, res) => {
    const ov     = stats.overview(db);
    const lb     = stats.leaderboard(db);
    const lbAddr = stats.leaderboardByAddress(db);
    const blocks = stats.recentBlocks(db, 5);
    const node   = stats.nodeStatus(db);
    res.render('index', {
        ov, lb, lbAddr, blocks, node,
        fmtHashrate: stats.fmtHashrate,
        fmtBtc:      stats.fmtBtc,
    });
});

app.get('/worker/:name', (req, res) => {
    const w = stats.worker(db, req.params.name, 86400);
    if (!w.worker) return res.status(404).render('404', { what: 'worker' });
    res.render('worker', {
        ...w, name: req.params.name,
        fmtHashrate: stats.fmtHashrate,
    });
});

/* Search box on the public nav points here (GET /worker-lookup?name=…).
 * Strip whitespace, cap length, redirect to the canonical URL — or 404
 * quietly if empty. */
app.get('/worker-lookup', (req, res) => {
    const raw = typeof req.query.name === 'string' ? req.query.name.trim() : '';
    if (!raw) return res.redirect(302, '/');
    const name = raw.slice(0, 200);
    return res.redirect(302, '/worker/' + encodeURIComponent(name));
});

app.get('/blocks', (req, res) => {
    const beforeTs = req.query.before ? Number(req.query.before) : null;
    const page = stats.allBlocks(db, { limit: 50, beforeTs });
    res.render('blocks', {
        rows: page.rows,
        next_before: page.next_before,
        fmtBtc: stats.fmtBtc,
    });
});

/* Public view of the work the pool is handing miners. Read-only and
 * unauthenticated: what a pool is mining, and whether its blocks can carry
 * sidechain commitments, is exactly the sort of thing miners should be able
 * to check without asking the operator. */
app.get('/templates', (req, res) => {
    const limit = req.query.limit ? Number(req.query.limit) : 50;
    res.render('templates', {
        templates: stats.templates(db, { limit }),
        fmtBtc: stats.fmtBtc,
    });
});

/* --- JSON API (unchanged) ---------------------------------------------- */
app.get('/api/overview',              (_req, res) => res.json(stats.overview(db)));
app.get('/api/node',                  (_req, res) => res.json(stats.nodeStatus(db) || {}));
app.get('/api/leaderboard',           (_req, res) => res.json(stats.leaderboard(db)));
app.get('/api/leaderboard/by-address',(_req, res) => res.json(stats.leaderboardByAddress(db)));
app.get('/api/worker/:name', (req, res) => {
    const w = stats.worker(db, req.params.name);
    if (!w.worker) return res.status(404).json({ error: 'unknown worker' });
    res.json(w);
});
app.get('/api/templates', (req, res) => {
    const limit = req.query.limit ? Number(req.query.limit) : 50;
    res.json(stats.templates(db, { limit }) || { current: null, history: [], total: 0 });
});

app.get('/api/blocks', (req, res) => {
    const beforeTs = req.query.before ? Number(req.query.before) : null;
    const limit = req.query.limit ? Number(req.query.limit) : 50;
    res.json(stats.allBlocks(db, { limit, beforeTs }));
});
app.get('/healthz', (_req, res) => res.json({ ok: true, db_ready: db.ready() }));

/* Full hard-failure detail. Public: what a pool's ledger checks say is
 * exactly the sort of thing miners should be able to read without asking.
 * 503 when something is failing so an uptime checker can watch it — and also
 * before the first pass, because "not yet known" is not "healthy". */
app.get('/health', (_req, res) => {
    const h = currentHealth();
    if (!h) return res.status(503).json({ ok: false, status: 'checking' });
    res.status(h.ok ? 200 : 503).json(h);
});

/* Which commit of each moving part is actually running — simplepool, the
 * enforcer, thunder, bitcoind. See lib/versions.js for why each component is
 * reported from both the live process and its checkout. `?force=1` skips the
 * cache, which is what you want immediately after a redeploy. */
app.get('/api/versions', async (req, res, next) => {
    try {
        res.json(await versions({ force: req.query.force === '1' }));
    } catch (e) { next(e); }
});

/* One URL that answers "how is the pool doing, and what is it running" —
 * everything the separate endpoints above return, in a single document, so a
 * miner or a monitor doesn't have to stitch four requests together.
 *
 * Deliberately always 200, unlike /health: this is a status report, and a
 * report that a check is failing was successfully produced. Read
 * `health.ok` for the condition itself. */
app.get('/api/status', async (req, res, next) => {
    try {
        const [v, h] = [
            await versions({ force: req.query.force === '1' }).catch(e => ({ error: e.message })),
            currentHealth(),
        ];
        const meta = stats.poolMeta(db);
        res.json({
            generated_at: Math.floor(Date.now() / 1000),
            pool: {
                mode:        meta ? meta.pool_mode : null,
                fee_bps:     meta ? meta.fee_bps   : null,
                /* Same four facts the header strip shows. A monitor should
                 * not have to scrape HTML to learn that the pool it is
                 * watching restarted onto a different network. */
                network:          meta ? meta.network          : null,
                network_source:   meta ? meta.network_source   : null,
                coinbase_tag:     meta ? meta.coinbase_tag     : null,
                operator_address: meta ? meta.operator_address : null,
                pool_btc_address: meta ? meta.pool_btc_address : null,
                stratum_url: PUBLIC_STRATUM_URL,
                ...stats.overview(db),
            },
            node:     stats.nodeStatus(db) || null,
            health:   h || { ok: false, status: 'checking' },
            versions: v,
        });
    } catch (e) { next(e); }
});

/* ================================ ADMIN ================================ */

app.use('/admin',
    requireAdminAuth,
    createAdminRouter({
        db,
        dbRw,
        THUNDER_RPC_URL,
        PAYOUT_ADMIN_URL,
        ENFORCER_GRPC_ADDR,
        THUNDER_SIDECHAIN_ID,
        RESERVE_ADDRESS,
    }));

/* ================================ 404 =================================== */

app.use((_req, res) => res.status(404).render('404', { what: 'page' }));

app.listen(PORT, () => {
    console.log(`simplepool dashboard on :${PORT} (db: ${db.path})`);
});
