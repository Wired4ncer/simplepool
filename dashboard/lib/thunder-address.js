/* Thunder address validation, mirroring src/thunder.c's thunder_address_decode.
 *
 * The proxy validates a miner's Thunder address at authorize time so a typo
 * cannot accrue an unpayable balance. The admin deposit form had no equivalent
 * — it checked only that the recipient was a string of 8..128 characters — so
 * a mistyped address there sent real BTC to a sidechain destination nobody
 * holds a key for, with no second confirmation. Same rules, same rejections.
 *
 * Kept deliberately dependency-free and byte-for-byte aligned with the C:
 *   - bare base58 only, decoding to exactly 20 bytes (a hash160)
 *   - the `s<n>_<base58>_<hex6>` deposit-format wrapper is rejected by name,
 *     because it LOOKS like an address but Thunder's wallet and its OP_RETURN
 *     parser do not recognise it at the byte level
 */

const B58 = '123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz';

/* Plain base58 (no checksum) -> bytes, or null if any character is outside
 * the alphabet. */
function b58Decode(s) {
    let zeros = 0;
    while (zeros < s.length && s[zeros] === '1') zeros++;

    const size = Math.floor(s.length * 733 / 1000) + 1;
    const b = new Uint8Array(size);
    for (const ch of s) {
        let carry = B58.indexOf(ch);
        if (carry < 0) return null;
        for (let j = size - 1; j >= 0; j--) {
            carry += 58 * b[j];
            b[j] = carry & 0xff;
            carry >>= 8;
        }
        if (carry !== 0) return null;
    }
    let skip = 0;
    while (skip < size && b[skip] === 0) skip++;
    const out = new Uint8Array(zeros + (size - skip));
    out.set(b.subarray(skip), zeros);
    return out;
}

/* Returns { ok: true } or { ok: false, msg }. */
export function validateThunderAddress(addr) {
    if (typeof addr !== 'string' || addr.length === 0) {
        return { ok: false, msg: 'empty thunder address' };
    }
    if (addr.includes('_')) {
        return { ok: false, msg:
            "thunder address contains '_' — that is the 's<n>_<base58>_<hex6>' " +
            'deposit-format wrapper, which Thunder cannot spend to. Use the bare ' +
            'base58 form (thunder-cli get-new-address).' };
    }
    if (addr.length < 20 || addr.length > 40) {
        return { ok: false, msg:
            `thunder address base58 length ${addr.length} out of range (20..40)` };
    }
    const dec = b58Decode(addr);
    if (!dec) return { ok: false, msg: 'thunder address base58 decode failed' };
    if (dec.length !== 20) {
        return { ok: false, msg:
            `thunder address decoded to ${dec.length} bytes (expected 20)` };
    }
    return { ok: true };
}
