/* The pool-identity strip.
 *
 * A miner handed a stratum URL cannot see which chain the coinbase is built
 * for, whether a block pays them or the pool wallet, or where the fee goes.
 * These tests pin the two properties that make the strip worth trusting:
 * it states those facts when the proxy has published them, and it says
 * "unknown" rather than guessing when it has not.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import Database from 'better-sqlite3';
import ejs from 'ejs';

import { poolMeta } from '../lib/stats.js';
import * as fmt from '../lib/fmt.js';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const SCHEMA = path.resolve(__dirname, '../../schema.sql');
const VIEWS  = path.resolve(__dirname, '../views');

const OPERATOR = 'tb1qw508d6qejxtdg4y5r3zarvary0c5xw7kxpjzsx';
const POOL_BTC = 'tb1qrp33g0q5c5txsp9arysrx4k6zdkfs4nce4xj0gdcccefvpysxf3qccfmv3';

function makeDb(identity = {}) {
    const file = path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'sp-poolid-')), 'shares.db');
    const db = new Database(file);
    db.exec(fs.readFileSync(SCHEMA, 'utf8'));
    const row = {
        network: 'signet', network_source: 'node', coinbase_tag: '/simplepool/',
        operator_address: OPERATOR, pool_btc_address: POOL_BTC,
        pool_mode: 'pps-classic', fee_bps: 100, listeners: null,
        ...identity,
    };
    db.prepare(`INSERT INTO pool_meta
                  (id, network, network_source, coinbase_tag, operator_address,
                   pool_btc_address, pool_mode, fee_bps, rate_source,
                   rate_sats_per_diff, gross_sats_per_diff, effective_fee_bps,
                   network_difficulty, block_value_sats, credited_from,
                   listeners, updated_at)
                VALUES (1, @network, @network_source, @coinbase_tag,
                        @operator_address, @pool_btc_address, @pool_mode,
                        @fee_bps, 'derived', 990, 1000, 100, 1, 312500000, 1,
                        @listeners, 1)`)
      .run(row);
    return db;
}

const render = (view, locals) =>
    ejs.renderFile(path.join(VIEWS, view), { ...fmt.all, ...locals },
                   { views: [VIEWS] });

const strip = db => render('partial/pool-identity.ejs', { pool: poolMeta(db) });

test('the strip states network, mode, tag and both addresses', async () => {
    const html = await strip(makeDb());
    assert.match(html, /signet/);
    assert.match(html, /pps-classic/);
    assert.match(html, /\/simplepool\//);
    assert.match(html, /fee 1\.00%/);
    /* In full. A truncated address cannot be checked against anything, and
     * checking where the money goes is the reason this exists. */
    assert.ok(html.includes(OPERATOR), 'operator address in full');
    assert.ok(html.includes(POOL_BTC), 'pool wallet address in full');
});

test('solo mode shows no pool wallet', async () => {
    /* The proxy stores NULL rather than "" in solo mode precisely so this
     * renders as "not applicable" instead of as a blank address. */
    const html = await strip(makeDb({ pool_mode: 'solo', pool_btc_address: null }));
    assert.match(html, /solo/);
    assert.doesNotMatch(html, /pool wallet/);
    assert.ok(!html.includes(POOL_BTC));
});

test('an inferred network is labelled, not asserted', async () => {
    const html = await strip(makeDb({ network_source: 'inferred' }));
    assert.match(html, /inferred/);
});

test('a non-mainnet pool is visually flagged', async () => {
    assert.match(await strip(makeDb({ network: 'signet' })), /poolid-test/);
    assert.doesNotMatch(await strip(makeDb({ network: 'main' })), /poolid-test/);
});

test('a DB predating the identity columns says unknown, not a guess', async () => {
    /* The upgrade order that actually happens in production: the dashboard
     * ships first and the proxy has not restarted, so its migrations have
     * not run. The strip must not invent a network — and the rate half of
     * pool_meta must survive, which is why the identity columns are read by
     * a separate, separately-guarded SELECT. */
    const db = makeDb();
    db.exec(`
        CREATE TABLE pm_old AS
            SELECT id, pool_mode, fee_bps, rate_source, rate_sats_per_diff,
                   gross_sats_per_diff, effective_fee_bps, network_difficulty,
                   block_value_sats, credited_from, updated_at, events_lost
              FROM pool_meta;
        DROP TABLE pool_meta;
        ALTER TABLE pm_old RENAME TO pool_meta;
    `);

    const meta = poolMeta(db);
    assert.equal(meta.network, null);
    assert.equal(meta.pool_mode, 'pps-classic', 'rate half must not be lost');
    assert.equal(meta.rate_sats_per_diff, 990);

    const html = await strip(db);
    assert.match(html, /unknown/);
    assert.doesNotMatch(html, /signet|regtest|\bmain\b/);
});

test('no pool_meta row at all still renders', async () => {
    const html = await render('partial/pool-identity.ejs', { pool: null });
    assert.match(html, /pool identity unknown/);
});

/* Which port to point which machine at.
 *
 * The strip exists because a stratum URL hides everything that matters, and
 * the port is now part of that: nothing about `:3334` versus `:3335` says one
 * expects a single ASIC and the other a rented fleet. Sending a fleet to the
 * low-difficulty port is not a subtle failure — it floods the pool and gets
 * the order cancelled — so the difference has to be stated. */
const TWO_PORTS = JSON.stringify([
    { port: 3334, label: '',        min_diff: 1,     initial_diff: 1 },
    { port: 3335, label: 'braiins', min_diff: 65536, initial_diff: 65536 },
]);

test('the strip names each port and what it is for', async () => {
    const html = await strip(makeDb({ listeners: TWO_PORTS }));
    assert.match(html, /3334/);
    assert.match(html, /3335/);
    assert.match(html, /braiins/);
    /* Not just the numbers — the guidance is the point. */
    assert.match(html, /for individual miners/);
    assert.match(html, /for rented or aggregated hashrate/);
    assert.match(html, /65,536/, 'difficulty is readable, not raw');
});

test('a single-port pool is not told to choose', async () => {
    /* There is no decision to explain, and a "ports" row on a pool with one
     * port is noise in a strip whose whole discipline is being quiet. */
    const one = JSON.stringify([{ port: 3334, label: '', min_diff: 1, initial_diff: 1 }]);
    const html = await strip(makeDb({ listeners: one }));
    assert.doesNotMatch(html, /poolid-ports/);
});

test('a proxy that has not published its ports says nothing about them', async () => {
    /* An upgraded DB whose proxy has not restarted. Inventing "3334" here
     * would be a guess, and a miner acting on a wrong port gets a refused
     * connection. */
    const html = await strip(makeDb({ listeners: null }));
    assert.doesNotMatch(html, /poolid-ports/);
});

test('malformed listener JSON does not take the strip down', async () => {
    const html = await strip(makeDb({ listeners: '{not json' }));
    assert.doesNotMatch(html, /poolid-ports/);
    /* The rest of the strip still renders — a pool that cannot describe its
     * ports must still say where the money goes. */
    assert.ok(html.includes(OPERATOR));
});
