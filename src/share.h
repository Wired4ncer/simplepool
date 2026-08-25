#ifndef SIMPLEPOOL_SHARE_H
#define SIMPLEPOOL_SHARE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Double-SHA256. */
void dsha256(const uint8_t *in, size_t inlen, uint8_t out[32]);

/* Convert compact nbits -> 32-byte big-endian target. */
void nbits_to_target(uint32_t nbits, uint8_t target_be[32]);

/* Worker difficulty -> 32-byte big-endian target.
 * target = floor( DIFF1_TARGET / diff ) where DIFF1_TARGET corresponds to
 * nbits 0x1d00ffff. Implementation precision: ~1-2 ulp at usual diffs (we
 * compute the top 128 bits in f64 then place them, mirroring the Rust
 * pool-core::share implementation). */
void worker_diff_to_target(double diff, uint8_t target_be[32]);

/* 32-byte big-endian target -> difficulty (DIFF1_TARGET / target), from the
 * top 128 bits in f64. Saturates to HUGE_VAL when those bits are all zero. */
double target_to_diff(const uint8_t target_be[32]);

/* Fair PPS rate for a block template: sats to credit per unit of share
 * difficulty, net of the operator fee.
 *
 *   gross = value_sats / net_diff
 *   net   = gross * (1 - fee_bps/10000)
 *
 * A share of difficulty D represents D * 2^32 expected hashes, and the chain
 * pays `value_sats` per `net_diff` * 2^32 hashes, so D * gross is exactly what
 * that share is worth in expectation. Deriving this per template is what keeps
 * the fee honest as difficulty moves — a hand-set rate goes stale, and can
 * invert into paying miners more than each share earns.
 *
 * Returns 0.0 for a template it cannot price (non-positive value or
 * difficulty, non-finite difficulty, or a fee that consumes the whole
 * reward), so callers disable accrual rather than credit a guess. */
double pps_rate_from_template(int64_t value_sats, double net_diff, int fee_bps);

/* The difficulty below which PPS fair value stops being fair.
 *
 * pps_rate_from_template pays block_value/difficulty per share, which is the
 * share's expected value only while the pool's solutions can actually become
 * blocks. A chain accepts one block per interval no matter how fast work
 * arrives, so once the pool's own difficulty throughput exceeds one block's
 * worth per interval, the formula promises blocks the chain will never mint.
 *
 * The boundary is where the pool alone would find exactly one block per
 * interval: min_difficulty = diff_per_sec * block_interval_sec. Pass the
 * pool's observed share-difficulty throughput. Returns 0 when there is no
 * measurement yet, which callers must read as "unknown", not "safe". */
double pps_min_safe_difficulty(double diff_per_sec, int block_interval_sec);

/* Cap a rate at what the chain can actually issue.
 *
 * The chain mints one block_value per block_interval_sec across every miner
 * in existence, so no pool can earn faster than that — crediting faster is
 * promising money that will not exist. Given the pool's observed difficulty
 * throughput, the highest defensible rate is (value / interval) / throughput.
 *
 * Returns `rate` unchanged when it is already below the ceiling or when there
 * is no throughput measurement to judge against. This is a backstop, not the
 * primary guard: it needs history, so it cannot protect the first shares after
 * a restart. pps_min_network_difficulty is what covers that. */
double pps_rate_apply_issuance_ceiling(double rate, int64_t value_sats,
                                       double diff_per_sec,
                                       int block_interval_sec);

/* Fold coinbase txid (LE 32-byte) into a merkle root using branches
 * (each LE 32-byte). Always cur||branch order (Stratum: coinbase at idx 0). */
void merkle_root_from_branches(const uint8_t leaf_le[32],
                               const uint8_t (*branches)[32], size_t n,
                               uint8_t root_le[32]);

/* Build the 80-byte block header (LE on the wire).
 * prev_hash_le and merkle_root_le are in their NATURAL bytes-as-stored
 * little-endian form (i.e. the form that goes directly into the header). */
void build_header(int32_t version,
                  const uint8_t prev_hash_le[32],
                  const uint8_t merkle_root_le[32],
                  uint32_t ntime, uint32_t nbits, uint32_t nonce,
                  uint8_t header_out[80]);

/* Compare two 32-byte big-endian numbers. -1 if a<b, 0 if eq, +1 if a>b. */
int be32_cmp(const uint8_t a[32], const uint8_t b[32]);

/* Hash header, return 32-byte big-endian hash. */
void hash_header(const uint8_t header[80], uint8_t hash_be[32]);

#endif
