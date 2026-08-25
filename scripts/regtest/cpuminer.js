/* Tiny CPU stratum miner for the regtest validation walk-through.
 *
 * Connects to simplepool on the configured stratum port, subscribes
 * with a Thunder-shaped username, iterates nonce until it finds a
 * hash that beats the network target (i.e. mines a block), submits,
 * and exits when the proxy reports a block was found.
 *
 * NOT a real miner — pure JS, no SIMD, no optimisation. Fine for
 * regtest because the regtest network target is enormous (~2^254);
 * a few thousand nonces are enough.
 *
 * Required pool config to make this work quickly:
 *   (no `listener` line)             # a port's min_diff overrides the
 *                                    # network-difficulty clamp and would
 *                                    # put every share out of CPU reach
 *   initial_diff       = 0.0000001   # share target > network target so
 *   vardiff_enabled    = 0           # any block-finding nonce also
 *                                    # satisfies the share check
 *
 * Usage:
 *   node scripts/regtest/cpuminer.js \
 *     --host 127.0.0.1 --port 13334 \
 *     --user <thunder_base58_address>
 */

import net from 'node:net';
import { createHash } from 'node:crypto';

/* ---- arg parsing ---- */
/* Base58 maps each leading '1' to a leading zero byte; 20 ones decode
 * to a 20-zero-byte Thunder address — structurally valid. The pool
 * only checks the format, not whether the address exists on the
 * Thunder side. */
const DEFAULT_THUNDER_USER = '11111111111111111111';

function parseArgs(argv) {
    const out = {
        host: '127.0.0.1', port: 13334,
        user: DEFAULT_THUNDER_USER,
        timeoutSec: 60,
    };
    for (let i = 2; i < argv.length; i++) {
        const a = argv[i];
        if (a === '--host')  out.host = argv[++i];
        else if (a === '--port')    out.port = parseInt(argv[++i], 10);
        else if (a === '--user')    out.user = argv[++i];
        else if (a === '--timeout') out.timeoutSec = parseInt(argv[++i], 10);
        else if (a === '-h' || a === '--help') {
            console.log('usage: cpuminer.js [--host H] [--port P] [--user USER] [--timeout SEC]');
            process.exit(0);
        }
    }
    return out;
}

/* ---- crypto helpers ---- */
function dsha256(buf) {
    const h1 = createHash('sha256').update(buf).digest();
    return createHash('sha256').update(h1).digest();
}

/* Convert nbits compact form -> 256-bit target as BigInt. */
function nbitsToTarget(nbitsHex) {
    const nbits = parseInt(nbitsHex, 16) >>> 0;
    const exp = nbits >>> 24;
    const mantissa = BigInt(nbits & 0x7fffff);
    if (exp <= 3) return mantissa >> BigInt(8 * (3 - exp));
    return mantissa << BigInt(8 * (exp - 3));
}

/* Word-swap notify's prev_hash to recover the header-internal LE bytes.
 * (The pool emits each 4-byte word reversed for stratum compat — see
 * make_notify_params in src/stratum.c.) */
function prevHashWireToHeader(hex) {
    const buf = Buffer.from(hex, 'hex');
    const out = Buffer.alloc(32);
    for (let wi = 0; wi < 8; wi++) {
        for (let bi = 0; bi < 4; bi++) {
            out[wi * 4 + bi] = buf[wi * 4 + (3 - bi)];
        }
    }
    return out;
}

/* Reverse a hex-encoded 4-byte big-endian field into a little-endian
 * 4-byte buffer for the block header. */
function be4HexToLE(hex) {
    const b = Buffer.from(hex, 'hex');
    return Buffer.from([b[3], b[2], b[1], b[0]]);
}

/* Interpret a 32-byte buffer as a little-endian uint256 BigInt. */
function leToBigInt(buf) {
    let n = 0n;
    for (let i = buf.length - 1; i >= 0; i--) {
        n = (n << 8n) | BigInt(buf[i]);
    }
    return n;
}

/* Compute the merkle root: start with coinbase txid (LE), combine with
 * each branch using dsha256(current || branch). */
function merkleRoot(coinbaseTxidLE, branchesHex) {
    let h = coinbaseTxidLE;
    for (const br of branchesHex) {
        h = dsha256(Buffer.concat([h, Buffer.from(br, 'hex')]));
    }
    return h;
}

/* ---- stratum client ---- */
class StratumClient {
    constructor({ host, port, user }) {
        this.host = host;
        this.port = port;
        this.user = user;
        this.sock = null;
        this.buf = '';
        this.nextId = 1;
        this.pending = new Map();
        this.en1 = null;
        this.en2Size = 4;
        this.diff = 1.0;
        this.job = null;     // latest mining.notify params (parsed)
        this.onNewJob = () => {};
        this.onSubmit = () => {};
    }

    connect() {
        return new Promise((resolve, reject) => {
            this.sock = net.createConnection({ host: this.host, port: this.port }, () => {
                console.log(`connected to ${this.host}:${this.port}`);
                resolve();
            });
            this.sock.on('data', d => this._onData(d));
            this.sock.on('error', e => reject(e));
            this.sock.on('close', () => console.log('connection closed'));
        });
    }

    _onData(chunk) {
        this.buf += chunk.toString('utf8');
        let i;
        while ((i = this.buf.indexOf('\n')) >= 0) {
            const line = this.buf.slice(0, i).trim();
            this.buf = this.buf.slice(i + 1);
            if (!line) continue;
            try {
                this._dispatch(JSON.parse(line));
            } catch (e) {
                console.error(`bad line: ${line} (${e.message})`);
            }
        }
    }

    _dispatch(msg) {
        if (msg.method === 'mining.notify') {
            this._handleNotify(msg.params);
            return;
        }
        if (msg.method === 'mining.set_difficulty') {
            this.diff = msg.params[0];
            console.log(`set_difficulty ${this.diff}`);
            return;
        }
        if (msg.id != null && this.pending.has(msg.id)) {
            const { resolve, reject, method } = this.pending.get(msg.id);
            this.pending.delete(msg.id);
            if (msg.error) reject(new Error(`${method}: ${JSON.stringify(msg.error)}`));
            else resolve(msg.result);
        }
    }

    _send(method, params) {
        return new Promise((resolve, reject) => {
            const id = this.nextId++;
            this.pending.set(id, { resolve, reject, method });
            this.sock.write(JSON.stringify({ id, method, params }) + '\n');
        });
    }

    _handleNotify(params) {
        const [job_id, prev_hash, cb1, cb2, branches, version, nbits, ntime, clean] = params;
        this.job = { job_id, prev_hash, cb1, cb2, branches, version, nbits, ntime, clean };
        console.log(`notify job=${job_id} clean=${clean} nbits=${nbits} ntime=${ntime}`);
        this.onNewJob(this.job);
    }

    async subscribe() {
        const r = await this._send('mining.subscribe', []);
        /* result = [[subscriptions], extranonce1, en2_size] */
        this.en1 = r[1];
        this.en2Size = r[2];
        console.log(`subscribed en1=${this.en1} en2_size=${this.en2Size}`);
    }

    async authorize() {
        const r = await this._send('mining.authorize', [this.user, 'x']);
        if (r !== true) throw new Error(`authorize rejected: ${JSON.stringify(r)}`);
        console.log(`authorized as ${this.user}`);
    }

    async submit(jobId, en2Hex, ntimeHex, nonceHex) {
        return await this._send('mining.submit', [this.user, jobId, en2Hex, ntimeHex, nonceHex]);
    }

    close() {
        this.sock?.destroy();
    }
}

/* ---- mining loop ---- */
async function mineUntilBlock(client, deadlineMs) {
    while (Date.now() < deadlineMs) {
        if (!client.job) {
            await new Promise(r => setTimeout(r, 50));
            continue;
        }
        const job = client.job;

        /* Build coinbase = cb1 || en1 || en2 || cb2. We hold en2 at a
         * fixed value (zeros) and iterate nonce — there's plenty of nonce
         * space on regtest for a one-block walk-through. */
        const en2 = Buffer.alloc(client.en2Size, 0);
        const cb1 = Buffer.from(job.cb1, 'hex');
        const cb2 = Buffer.from(job.cb2, 'hex');
        const en1 = Buffer.from(client.en1, 'hex');
        const coinbase = Buffer.concat([cb1, en1, en2, cb2]);
        const coinbaseTxid = dsha256(coinbase);

        const mroot = merkleRoot(coinbaseTxid, job.branches);
        const prev  = prevHashWireToHeader(job.prev_hash);
        const verLE = be4HexToLE(job.version);
        const tmLE  = be4HexToLE(job.ntime);
        const bitsLE = be4HexToLE(job.nbits);
        const target = nbitsToTarget(job.nbits);

        /* Header = ver || prev || mroot || ntime || nbits || nonce */
        const header = Buffer.alloc(80);
        verLE.copy(header, 0);
        prev.copy(header, 4);
        mroot.copy(header, 36);
        tmLE.copy(header, 68);
        bitsLE.copy(header, 72);

        let nonce = 0;
        const t0 = Date.now();
        const startedJob = job.job_id;
        while (Date.now() < deadlineMs && client.job?.job_id === startedJob) {
            header.writeUInt32LE(nonce >>> 0, 76);
            const hash = dsha256(header);
            if (leToBigInt(hash) <= target) {
                const en2Hex   = en2.toString('hex');
                const nonceHex = Buffer.from([
                    (nonce >>> 24) & 0xff, (nonce >>> 16) & 0xff,
                    (nonce >>>  8) & 0xff,  nonce         & 0xff,
                ]).toString('hex');
                console.log(`*** candidate: nonce=${nonce} hash_le=${hash.toString('hex')}`);
                try {
                    const ok = await client.submit(job.job_id, en2Hex, job.ntime, nonceHex);
                    console.log(`submit -> ${ok}`);
                    if (ok === true) return true;
                } catch (e) {
                    console.warn(`submit error: ${e.message}`);
                }
                nonce++;
                continue;
            }
            nonce++;
            if ((nonce & 0xffff) === 0) {
                const rate = nonce / Math.max(1, (Date.now() - t0) / 1000);
                process.stdout.write(`\rnonce=${nonce} ${(rate / 1000).toFixed(1)} kH/s`);
            }
            if (nonce >>> 0 === 0xffffffff) {
                console.log('\nnonce space exhausted on this job, waiting for new notify');
                break;
            }
        }
        process.stdout.write('\n');
    }
    return false;
}

/* ---- entry ---- */
const args = parseArgs(process.argv);
const client = new StratumClient(args);
await client.connect();
await client.subscribe();
await client.authorize();

const deadline = Date.now() + args.timeoutSec * 1000;
const found = await mineUntilBlock(client, deadline);
client.close();
if (found) {
    console.log('block found and accepted ✓');
    process.exit(0);
} else {
    console.log(`no block within ${args.timeoutSec}s (timeout)`);
    process.exit(1);
}
