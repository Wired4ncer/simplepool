/* Formatting helpers shared across views. server.js drops them onto
 * res.locals so every EJS render sees them without a per-view
 * <% const fmt... %> preamble. */

export const fmtN = (n) =>
    new Intl.NumberFormat('en-US').format(n || 0);

export const fmtF = (n, d = 4) =>
    (typeof n === 'number' ? n.toFixed(d) : '—');

export const fmtTs = (t) =>
    t ? new Date(t * 1000).toISOString().replace('T', ' ').slice(0, 19) + 'Z' : '—';

export const ago = (t) => {
    if (!t) return '—';
    const s = Math.max(0, Math.floor(Date.now() / 1000) - t);
    if (s < 60)    return s + 's ago';
    if (s < 3600)  return Math.floor(s / 60)    + 'm ago';
    if (s < 86400) return Math.floor(s / 3600)  + 'h ago';
    return Math.floor(s / 86400) + 'd ago';
};

export const fmtSats = (sats) => {
    if (!sats) return '0 sats';
    const btc = sats / 1e8;
    if (btc >= 0.01) return fmtN(sats) + ' sats (' + btc.toFixed(8) + ' BTC)';
    return fmtN(sats) + ' sats';
};

/* Adaptive-precision percent — keeps small miners visible instead of
 * rendering them as 0.00%. Used by the public overview. */
export const fmtPct = (p) => {
    if (p == null || !isFinite(p)) return '—';
    if (p === 0) return '0%';
    const abs = Math.abs(p);
    if (abs >= 1)     return p.toFixed(2)  + '%';
    if (abs >= 0.01)  return p.toFixed(3)  + '%';
    if (abs >= 0.0001) return p.toFixed(4) + '%';
    return p.toPrecision(2) + '%';
};

/* The verdict on a block candidate, as a small piece of HTML.
 *
 * Never render a candidate's status as blank or absent. A row in blocks_found
 * is a candidate until something verifies it, and a page that quietly shows
 * all of them as blocks is exactly how a pool that had mined nothing appeared
 * to have mined thousands. `pending` is not a transient — against a backend
 * that serves only getblocktemplate and submitblock there may be nothing able
 * to verify a block for a while — so it gets its own honest wording rather
 * than being rounded up to "found".
 *
 * Escapes the status before interpolating: it comes from the DB, and this is
 * emitted unescaped by the views. */
export const blockStatus = (b) => {
    const raw = (b && b.status) || 'pending';
    const st = String(raw).replace(/[^a-z]/gi, '').toLowerCase();
    const confs = Number((b && b.confirmations) || 0);
    if (st === 'confirmed') {
        const via = b && b.checked_via === 'tips' ? ' (from observed tips)' : '';
        return `<span class="blk-ok" title="in the chain${via}">in chain${
            confs > 0 ? ' · ' + fmtN(confs) + ' conf' : ''}</span>`;
    }
    if (st === 'orphaned') {
        return '<span class="blk-bad" title="was submitted, but the chain went' +
               ' another way — it pays nothing">orphaned</span>';
    }
    if (st === 'rejected') {
        const why = b && b.submit_error ? String(b.submit_error) : '';
        return `<span class="blk-bad" title="the node refused this block${
            why ? ': ' + why.replace(/[<>&"]/g, '') : ''}">rejected</span>`;
    }
    return '<span class="muted" title="submitted and accepted, but not yet' +
           ' verified to be in the chain — counts as nothing until it is">' +
           'unverified</span>';
};

/* Convenience bundle for res.locals middleware. */
export const all = { fmtN, fmtF, fmtTs, ago, fmtSats, fmtPct, blockStatus };
