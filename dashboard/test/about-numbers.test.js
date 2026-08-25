/* The "about the numbers" card.
 *
 * The card it replaces stated the pps-classic story unconditionally, which
 * on a solo pool told miners to authorize with a Thunder address — rejected
 * by stratum.c with "invalid payout address in stratum username". So these
 * tests are mostly about the card not saying the other mode's thing: what a
 * share is worth and what the username must be are exactly the two facts
 * that differ, and both cost a miner real time when stated wrongly.
 *
 * The rate is asserted to be the live one, never a literal. The literal it
 * replaces ("1 000 sats × share difficulty") was hardcoded HTML describing a
 * rate that is derived per template and moves with difficulty.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import fs from 'node:fs';
import os from 'node:os';
import ejs from 'ejs';
import Database from 'better-sqlite3';

import * as fmt from '../lib/fmt.js';
import * as stats from '../lib/stats.js';
import { openDb } from '../lib/db.js';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const VIEWS = path.resolve(__dirname, '../views');

const OPERATOR = 'tb1qw508d6qejxtdg4y5r3zarvary0c5xw7kxpjzsx';
const POOL_BTC = 'tb1qrp33g0q5c5txsp9arysrx4k6zdkfs4nce4xj0gdcccefvpysxf3qccfmv3';
const URL_ = 'stratum+tcp://pool.example.org:3334';

const PPS = {
    pool_mode: 'pps-classic', fee_bps: 100, network: 'signet',
    rate_source: 'derived', rate_sats_per_diff: 2783.22,
    gross_sats_per_diff: 2811.33, effective_fee_bps: 100,
    operator_address: OPERATOR, pool_btc_address: POOL_BTC,
};
const SOLO = {
    pool_mode: 'solo', fee_bps: 100, network: 'signet',
    rate_source: 'derived', rate_sats_per_diff: 0, gross_sats_per_diff: 2811.33,
    effective_fee_bps: 0, operator_address: OPERATOR, pool_btc_address: null,
};

const card = (pool, extra = {}) =>
    ejs.renderFile(path.join(VIEWS, 'partial/about-numbers.ejs'),
                   { ...fmt.all, pool, stratumUrl: URL_, sidechainId: 9, ...extra },
                   { views: [VIEWS] });

test('pps-classic states the live rate, not a hardcoded one', async () => {
    const html = await card(PPS);
    assert.match(html, /pps-classic/);
    assert.match(html, /2,783\.22/, 'the rate the proxy actually published');
    assert.match(html, /2,811\.33/, 'gross, before fee');
    assert.match(html, /1\.00%/);
    /* The literal this card exists to remove. */
    assert.doesNotMatch(html, /1[  ]000[  ]sats/);
    assert.match(html, /not a fixed number/);
});

test('pps-classic names the Thunder username and both addresses', async () => {
    const html = await card(PPS);
    assert.match(html, /your-Thunder-address/);
    assert.match(html, /invalid thunder address/);
    assert.ok(html.includes(POOL_BTC));
    assert.ok(html.includes(OPERATOR));
    assert.ok(html.includes(URL_));
    /* Must not tell a pps miner to use a Bitcoin address. */
    assert.doesNotMatch(html, /your-bitcoin-address/);
});

test('solo says nothing about Thunder, deposits or PPS credit', async () => {
    const html = await card(SOLO);
    assert.match(html, /\bsolo\b/);
    /* None of the pps-classic machinery exists in solo. */
    assert.doesNotMatch(html, /pps_credits/);
    assert.doesNotMatch(html, /BIP300 deposit/);
    assert.doesNotMatch(html, /payout worker/);
    assert.doesNotMatch(html, /sidechain/);
    /* Thunder appears exactly twice, and both are negations: "there is no
     * Thunder payout in this mode", and "a Thunder address is rejected".
     * Both earn their place — a miner arriving from a pps-classic pool, or
     * from the card this replaces, needs to be told. The count is pinned so
     * copy drift cannot quietly reintroduce the pps-classic story here. */
    assert.equal((html.match(/Thunder/g) || []).length, 2);
    assert.match(html, /no pool wallet and no Thunder payout/);
    assert.match(html, /as is a <strong>Thunder<\/strong> address/);
    /* Never as an instruction. */
    assert.doesNotMatch(html, /your-Thunder-address/);
    /* No pool wallet in solo, so its address must not appear. */
    assert.ok(!html.includes(POOL_BTC));
});

test('solo tells miners to use a bitcoin address, and says taproot is not', async () => {
    const html = await card(SOLO);
    assert.match(html, /your-bitcoin-address/);
    assert.doesNotMatch(html, /your-Thunder-address/);
    assert.match(html, /P2WPKH/);
    assert.match(html, /[Tt]aproot.*not\s*<\/strong>?\s*supported|not\s*<\/strong>\s*and is rejected|Taproot/);
    assert.match(html, /invalid payout address in stratum username/);
    /* Coinbase maturity — the "why can't I spend it" question. */
    assert.match(html, /100 confirmations/);
    assert.ok(html.includes(OPERATOR), 'solo still pays an operator fee');
});

test('address examples follow the network the pool is actually on', async () => {
    assert.match(await card({ ...SOLO, network: 'main' }),    /bc1q…/);
    assert.match(await card({ ...SOLO, network: 'main' }),    /bc1p…/);
    assert.match(await card({ ...SOLO, network: 'signet' }),  /tb1q…/);
    assert.match(await card({ ...SOLO, network: 'regtest' }), /bcrt1q…/);
    /* A mainnet example on a signet pool misleads as surely as the wrong
     * address type does. */
    assert.doesNotMatch(await card({ ...SOLO, network: 'signet' }), /bc1q…/);
});

test('an ambiguous network falls back to prose instead of inventing a prefix', async () => {
    /* "test/signet/regtest" comes from a base58 operator address, which
     * genuinely cannot say which chain it is. */
    const html = await card({ ...SOLO, network: 'test/signet/regtest' });
    assert.match(html, /P2WPKH/);
    assert.doesNotMatch(html, /1q…/, 'no bech32 prefix invented');
});

test('a pinned rate is called out, with the fee it actually implies', async () => {
    const html = await card({ ...PPS, rate_source: 'override', effective_fee_bps: 644.3 });
    assert.match(html, /pinned/);
    assert.match(html, /6\.44%/);
    assert.match(html, /\/health/);
});

test('pps-classic before the first template does not claim a rate of zero', async () => {
    const html = await card({ ...PPS, rate_sats_per_diff: 0, gross_sats_per_diff: 0 });
    assert.match(html, /has not published a rate yet/);
    assert.doesNotMatch(html, /0\.00\s*sats/);
});

test('an unknown mode describes both and commits to neither', async () => {
    for (const pool of [null, { pool_mode: null, fee_bps: 0 }]) {
        const html = await card(pool);
        assert.match(html, /has not published its mode/);
        /* Both named, so a miner knows what to ask the operator — but no
         * username form is asserted, because guessing costs them time. */
        assert.match(html, /solo/);
        assert.match(html, /pps-classic/);
        assert.doesNotMatch(html, /your-Thunder-address/);
        assert.doesNotMatch(html, /your-bitcoin-address/);
    }
});

test('the overview still renders end to end with the partial in place', async () => {
    /* Against real stats output rather than a hand-written `ov`, so this
     * catches the partial breaking the page it lives on without going stale
     * every time the overview grows a field. */
    const file = path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'sp-about-')), 'shares.db');
    const sdb = new Database(file);
    sdb.exec(fs.readFileSync(path.resolve(__dirname, '../../schema.sql'), 'utf8'));
    sdb.close();
    const db = openDb(file);

    const html = await ejs.renderFile(path.join(VIEWS, 'index.ejs'), {
        ...fmt.all, pool: PPS, stratumUrl: URL_, sidechainId: 9,
        health: null, active: 'overview',
        ov:     stats.overview(db),
        lb:     stats.leaderboard(db),
        lbAddr: stats.leaderboardByAddress(db),
        blocks: stats.recentBlocks(db, 5),
        node:   stats.nodeStatus(db),
        fmtHashrate: stats.fmtHashrate,
        fmtBtc:      stats.fmtBtc,
    }, { views: [VIEWS] });
    assert.match(html, /About the numbers on this page/);
    assert.match(html, /2,783\.22/);
});
