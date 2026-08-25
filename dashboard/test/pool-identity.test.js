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
        pool_mode: 'pps-classic', fee_bps: 100,
        ...identity,
    };
    db.prepare(`INSERT INTO pool_meta
                  (id, network, network_source, coinbase_tag, operator_address,
                   pool_btc_address, pool_mode, fee_bps, rate_source,
                   rate_sats_per_diff, gross_sats_per_diff, effective_fee_bps,
                   network_difficulty, block_value_sats, credited_from, updated_at)
                VALUES (1, @network, @network_source, @coinbase_tag,
                        @operator_address, @pool_btc_address, @pool_mode,
                        @fee_bps, 'derived', 990, 1000, 100, 1, 312500000, 1, 1)`)
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
