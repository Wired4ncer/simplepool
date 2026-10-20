/* Parse an HTTP Basic `Authorization` header into { user, pass }.
 *
 * RFC 7617 §2: the user-id cannot contain ':', the password can, so the
 * credentials split on the FIRST colon only. `.split(':')` with array
 * destructuring dropped everything after a second colon: a password with a
 * ':' in it could never match, and `user:pass:anything` was accepted as
 * `user:pass`. Returns null for a missing / non-Basic / colon-less header. */
export function parseBasicAuth(header) {
    const h = String(header || '');
    if (!h.startsWith('Basic ')) return null;
    const raw = Buffer.from(h.slice(6), 'base64').toString('utf8');
    const sep = raw.indexOf(':');
    if (sep < 0) return null;
    return { user: raw.slice(0, sep), pass: raw.slice(sep + 1) };
}
