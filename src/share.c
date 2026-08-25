/* Pure share-validation math. No I/O. Mirrors crates/pool-core/src/{share,
 * merkle,job}.rs byte-for-byte. */

#include "share.h"
#include "sha256.h"

#include <math.h>
#include <string.h>

void dsha256(const uint8_t *in, size_t inlen, uint8_t out[32]) {
    uint8_t tmp[32];
    sha256(in, inlen, tmp);
    sha256(tmp, 32, out);
}

void nbits_to_target(uint32_t nbits, uint8_t target_be[32]) {
    memset(target_be, 0, 32);
    uint8_t exp = (uint8_t)(nbits >> 24);
    uint32_t mant = nbits & 0x007fffffu;
    int negative = (nbits & 0x00800000u) != 0;
    if (mant == 0 || negative) return;
    if (exp <= 3) {
        unsigned shift = 8u * (3u - exp);
        uint32_t v = mant >> shift;
        target_be[29] = (uint8_t)((v >> 16) & 0xff);
        target_be[30] = (uint8_t)((v >> 8) & 0xff);
        target_be[31] = (uint8_t)(v & 0xff);
    } else {
        size_t off = 32u - (size_t)exp;
        if (off + 3u <= 32u) {
            target_be[off]     = (uint8_t)((mant >> 16) & 0xff);
            target_be[off + 1] = (uint8_t)((mant >> 8) & 0xff);
            target_be[off + 2] = (uint8_t)(mant & 0xff);
        }
    }
}

/* DIFF1 target: 0x00000000FFFF0000...0000 (BE). Top 128 bits = 0x00000000FFFF0000 0000000000000000. */
static const uint8_t DIFF1_TARGET[32] = {
    0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* __int128 is a compiler extension; the typedef is wrapped in __extension__ so
 * strict -Wpedantic accepts it, and uses of the typedef name don't warn. */
__extension__ typedef unsigned __int128 u128;

static u128 be16_to_u128(const uint8_t b[16]) {
    u128 v = 0;
    for (int i = 0; i < 16; ++i) {
        v = (v << 8) | (u128)b[i];
    }
    return v;
}

static void u128_to_be16(u128 v, uint8_t out[16]) {
    for (int i = 15; i >= 0; --i) {
        out[i] = (uint8_t)(v & 0xffu);
        v >>= 8;
    }
}

double target_to_diff(const uint8_t target_be[32]) {
    u128 hi = be16_to_u128(target_be);
    if (hi == 0) {
        /* Top 128 bits all zero: harder than any real chain target. Callers
         * use this for clamping, so saturate rather than divide by zero. */
        return HUGE_VAL;
    }
    return (double)be16_to_u128(DIFF1_TARGET) / (double)hi;
}

double pps_rate_from_template(int64_t value_sats, double net_diff, int fee_bps) {
    if (value_sats <= 0) return 0.0;
    if (!isfinite(net_diff) || net_diff <= 0.0) return 0.0;
    if (fee_bps < 0 || fee_bps >= 10000) return 0.0;
    double gross = (double)value_sats / net_diff;
    if (!isfinite(gross)) return 0.0;
    double net = gross * (1.0 - (double)fee_bps / 10000.0);
    return net > 0.0 ? net : 0.0;
}

double pps_min_safe_difficulty(double diff_per_sec, int block_interval_sec) {
    if (!isfinite(diff_per_sec) || diff_per_sec <= 0.0) return 0.0;
    if (block_interval_sec <= 0) return 0.0;
    double d = diff_per_sec * (double)block_interval_sec;
    return isfinite(d) ? d : 0.0;
}

double pps_rate_apply_issuance_ceiling(double rate, int64_t value_sats,
                                       double diff_per_sec,
                                       int block_interval_sec) {
    if (!isfinite(rate) || rate <= 0.0) return rate;
    if (value_sats <= 0) return rate;
    if (!isfinite(diff_per_sec) || diff_per_sec <= 0.0) return rate;
    if (block_interval_sec <= 0) return rate;
    /* Sats the chain can mint per second, divided by the difficulty the pool
     * presents per second: the most any single unit of difficulty can be
     * worth without the pool promising more than exists. */
    double issuance_per_sec = (double)value_sats / (double)block_interval_sec;
    double ceiling = issuance_per_sec / diff_per_sec;
    if (!isfinite(ceiling) || ceiling < 0.0) return rate;
    return rate < ceiling ? rate : ceiling;
}

void worker_diff_to_target(double diff, uint8_t target_be[32]) {
    memset(target_be, 0, 32);
    if (!isfinite(diff) || diff <= 0.0) {
        memset(target_be, 0xff, 32);
        return;
    }
    u128 hi = be16_to_u128(DIFF1_TARGET);
    double scaled = (double)hi / diff;
    if (scaled < 0.0) scaled = 0.0;
    /* Saturate rather than convert.
     *
     * The previous clamp was `if (scaled >= 2^128) scaled = 2^128 - 1.0`,
     * which does nothing: a double has 53 bits of mantissa, so near 2^128 the
     * spacing between representable values is 2^75 and 2^128 - 1 rounds
     * straight back to 2^128. The conversion that followed was then out of
     * range for u128 — undefined behaviour, and in practice a target of
     * either zero (every share rejected as low difficulty) or all-ones
     * (every share accepted), decided by the compiler.
     *
     * Reachable whenever the worker difficulty is below ~2.3e-10, which a
     * configured initial_diff or vardiff_min can be. Write the easiest
     * possible target directly instead, which is what a difficulty that small
     * means and what the diff <= 0 branch above already does. */
    if (!(scaled < ldexp(1.0, 128))) {
        memset(target_be, 0xff, 32);
        return;
    }
    u128 hi_scaled = (u128)scaled;
    u128_to_be16(hi_scaled, target_be);
}

void merkle_root_from_branches(const uint8_t leaf_le[32],
                               const uint8_t (*branches)[32], size_t n,
                               uint8_t root_le[32]) {
    uint8_t cur[32];
    uint8_t buf[64];
    memcpy(cur, leaf_le, 32);
    for (size_t i = 0; i < n; ++i) {
        memcpy(buf, cur, 32);
        memcpy(buf + 32, branches[i], 32);
        dsha256(buf, 64, cur);
    }
    memcpy(root_le, cur, 32);
}

static void put_u32_le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

void build_header(int32_t version,
                  const uint8_t prev_hash_le[32],
                  const uint8_t merkle_root_le[32],
                  uint32_t ntime, uint32_t nbits, uint32_t nonce,
                  uint8_t header_out[80]) {
    put_u32_le(header_out + 0, (uint32_t)version);
    memcpy(header_out + 4, prev_hash_le, 32);
    memcpy(header_out + 36, merkle_root_le, 32);
    put_u32_le(header_out + 68, ntime);
    put_u32_le(header_out + 72, nbits);
    put_u32_le(header_out + 76, nonce);
}

int be32_cmp(const uint8_t a[32], const uint8_t b[32]) {
    for (int i = 0; i < 32; ++i) {
        if (a[i] < b[i]) return -1;
        if (a[i] > b[i]) return 1;
    }
    return 0;
}

void hash_header(const uint8_t header[80], uint8_t hash_be[32]) {
    uint8_t hash_le[32];
    dsha256(header, 80, hash_le);
    /* Reverse to BE. */
    for (int i = 0; i < 32; ++i) {
        hash_be[i] = hash_le[31 - i];
    }
}
