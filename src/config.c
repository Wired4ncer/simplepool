#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include "coinbase.h"
#include "log.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static void set_err(char *errbuf, size_t errlen, const char *fmt, ...) {
    if (!errbuf || errlen == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(errbuf, errlen, fmt, ap);
    va_end(ap);
}

void proxy_config_defaults(proxy_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->listen_addr, sizeof cfg->listen_addr, "%s", "0.0.0.0");
    cfg->listen_port  = 3334;
    cfg->max_conns    = 500;
    /* 0 = defer to STRATUM_DEFAULT_BACKLOG in stratum.c. Kept as a literal so
     * config.c needs no dependency on the stratum header. */
    cfg->listen_backlog = 0;
    cfg->initial_diff = 1.0;

    snprintf(cfg->bitcoind_url,  sizeof cfg->bitcoind_url,  "%s", "http://127.0.0.1:18443");
    /* No default credentials: when bitcoind_user/bitcoind_pass are omitted the
     * RPC client connects without basic auth (for backends that don't require
     * it). memset above already leaves both as empty strings. */
    cfg->bitcoind_user[0] = '\0';
    cfg->bitcoind_pass[0] = '\0';
    cfg->bitcoind_poll_interval_ms = 30000;

    cfg->operator_address[0] = '\0';
    cfg->fee_bps = 100;  /* 1% */
    snprintf(cfg->coinbase_tag, sizeof cfg->coinbase_tag, "%s", "/simplepool/");

    snprintf(cfg->db_path, sizeof cfg->db_path, "%s", "./data/shares.db");
    cfg->commit_window_ms  = 100;
    cfg->commit_max_shares = 100;
    cfg->templates_retention_days = 30;

    cfg->log_level = 1; /* info */

    cfg->vardiff_enabled    = 1;
    cfg->vardiff_target_spm = 12.0;   /* ~1 share every 5s per connection */
    cfg->vardiff_min        = 1.0;
    cfg->static_diff_enabled = 0;   /* `sd=` pins are OFF until asked for */
    cfg->static_diff_min     = 16384;  /* pin floor, independent of vardiff_min */
    cfg->vardiff_max        = 1e12;
    cfg->vardiff_window_sec = 30;
    cfg->vardiff_min_samples     = 20;
    cfg->max_suggested_diff      = 5e7;
    cfg->vardiff_max_window_mult = 8;
    cfg->vardiff_idle_step       = 2.0;
    cfg->clean_jobs_on_refresh = 1;
    cfg->idle_timeout_sec   = 600;    /* 10 min silent recv → reap */
    cfg->idle_timeout_authorized_sec = 3600;  /* authorized miners: 1 h */

    /* Rental port off unless configured. 500000 clears both Braiins (>=1024,
     * recommends 65536) and NiceHash (500000) with one listener. */
    /* ⛔ OFF, deliberately — upstream defaults this to 20000. Refusing a
     * submit is a miner-visible action, and no real miner's burst rate has
     * been measured against a ceiling here yet. It gets opened like every
     * other gate key: written explicitly, one stage at a time, after
     * measurement. */
    cfg->max_submits_per_sec = 0;
    /* ON by default, unlike the submit ceiling: this refuses nothing a
     * correct miner does, it only bounds how long a client may keep failing.
     * Three failures, then a minute — a misconfigured miner still gets its
     * error message on every reconnect once the minute has passed. */
    cfg->auth_max_failures     = 3;
    cfg->auth_fail_lockout_sec = 60;
    cfg->listener_count = 0;
    cfg->rental_listen_port = 0;
    cfg->rental_min_diff    = 500000.0;
    cfg->rental_max_conns   = 0;      /* 0 → inherit max_conns */

    cfg->redis_url[0] = '\0';
    cfg->redis_publish_timeout_ms   = 200;
    cfg->redis_reconnect_backoff_ms = 2000;

    snprintf(cfg->pool_mode, sizeof cfg->pool_mode, "%s", "solo");
    cfg->pool_btc_address[0] = '\0';
    cfg->pps_sats_per_diff = 0.0;

    cfg->prop_window_k = 3.0;
    cfg->prop_min_payout_sats = 1000000LL;  /* ~0.01 ECX at current subsidy */
    cfg->prop_max_outputs = 12;
    cfg->prop_carry_slots = 0;   /* off: an upgrade never silently reprices */
    /* 0 = off, so an upgrade never silently changes how many miners a block
     * pays. The operator opts in with a measured number. */
    cfg->prop_max_coinbase_bytes = 0;
    cfg->prop_window_min_sec = 600;         /* 10 minutes */
    cfg->pps_min_network_difficulty = 0.0;
    cfg->block_interval_sec = 600;
    cfg->pps_refuse_shares_below_min = 1;
}

static char *strtrim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    if (!*s) return s;
    char *e = s + strlen(s) - 1;
    while (e > s && isspace((unsigned char)*e)) { *e = '\0'; e--; }
    return s;
}

/* Truncate the line at a comment introducer.
 *
 * `#` only starts a comment at the start of the line or after whitespace, and
 * never inside double quotes. The old rule was "the first # anywhere", applied
 * before the key/value split and before unquote() — so a password of p#ssw0rd
 * silently became p, quoting did not help, and the pool then failed RPC auth
 * with nothing in the log to say why. coinbase_tag and the address fields had
 * the same exposure. Inline comments (` # like this`) still work, which is
 * what proxy.conf.example documents. */
static void strip_comment(char *line) {
    int in_quotes = 0;
    for (char *p = line; *p; ++p) {
        if (*p == '"') { in_quotes = !in_quotes; continue; }
        if (*p == '#' && !in_quotes &&
            (p == line || p[-1] == ' ' || p[-1] == '\t')) {
            *p = '\0';
            return;
        }
    }
}

static void unquote(char *s) {
    size_t n = strlen(s);
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
        memmove(s, s + 1, n - 2);
        s[n - 2] = '\0';
    }
}

static int parse_log_level(const char *v) {
    if (strcasecmp(v, "debug") == 0) return 0;
    if (strcasecmp(v, "info")  == 0) return 1;
    if (strcasecmp(v, "warn")  == 0 || strcasecmp(v, "warning") == 0) return 2;
    if (strcasecmp(v, "error") == 0) return 3;
    /* Numeric. */
    char *end = NULL;
    long n = strtol(v, &end, 10);
    if (end && *end == '\0' && n >= 0 && n <= 3) return (int)n;
    return -1;
}

static void copy_str(char *dst, size_t cap, const char *src) {
    snprintf(dst, cap, "%s", src);
}

/* Parse one `listener = port=3335 min_diff=65536 label=braiins` line into
 * `out`. Fields are separated by whitespace or commas and may appear in any
 * order; `port` is the only required one, and anything left unset falls back
 * to the server-wide default at accept time.
 *
 * mode=solo makes this a SOLO port: connections arriving here pay their own
 * coinbase (minus the operator fee) instead of the shared PPLNS payout set,
 * and their shares are recorded solo=1 so they never enter anyone's window.
 * Absent or mode=proportional keeps the pool-wide behaviour.
 *
 * min_diff sets the vardiff floor and, unless initial_diff says otherwise,
 * the starting difficulty too. That pairing is the whole point of a rental
 * port: the miner has to arrive already at the floor, because vardiff cannot
 * climb to it fast enough to matter. Returns 0 on success. */
static int parse_listener(const char *v, stratum_listener_t *out,
                          char *errbuf, size_t errlen) {
    char buf[512];
    snprintf(buf, sizeof buf, "%s", v);
    memset(out, 0, sizeof *out);

    double min_diff = 0.0, initial = 0.0;
    char *save = NULL;
    for (char *tok = strtok_r(buf, " \t,", &save); tok;
         tok = strtok_r(NULL, " \t,", &save)) {
        char *eq = strchr(tok, '=');
        if (!eq) {
            set_err(errbuf, errlen, "listener field '%s' is not key=value", tok);
            return -1;
        }
        *eq = '\0';
        const char *fk = tok, *fv = eq + 1;
        if      (strcmp(fk, "port")         == 0) out->port = atoi(fv);
        else if (strcmp(fk, "min_diff")     == 0) min_diff = atof(fv);
        else if (strcmp(fk, "initial_diff") == 0) initial = atof(fv);
        else if (strcmp(fk, "max_diff")     == 0) out->vardiff_max = atof(fv);
        else if (strcmp(fk, "label")        == 0) copy_str(out->label, sizeof out->label, fv);
        else if (strcmp(fk, "mode")         == 0) {
            /* Only the two that mean something here. "proportional" is spelled
             * out rather than treated as the absence of mode=solo so an
             * operator can state the intent explicitly on the public port. */
            if      (strcmp(fv, "solo")         == 0) out->solo = 1;
            else if (strcmp(fv, "proportional") == 0) out->solo = 0;
            else {
                set_err(errbuf, errlen,
                        "listener mode must be 'solo' or 'proportional', got '%s'", fv);
                return -1;
            }
        }
        else {
            set_err(errbuf, errlen, "unknown listener field '%s'", fk);
            return -1;
        }
    }
    if (out->port <= 0 || out->port > 65535) {
        set_err(errbuf, errlen, "listener needs a port between 1 and 65535");
        return -1;
    }
    /* The label is published to the DB inside a JSON blob and rendered into
     * the dashboard. Constraining it here means neither of those has to
     * escape it, and an operator typo fails at startup rather than producing
     * a banner that silently breaks. */
    for (const char *q = out->label; *q; ++q) {
        if (!isalnum((unsigned char)*q) && *q != '-' && *q != '_') {
            set_err(errbuf, errlen,
                    "listener port %d: label may only contain letters, "
                    "digits, '-' and '_'", out->port);
            return -1;
        }
    }
    out->vardiff_min  = min_diff;
    /* Recorded separately from vardiff_min so the server can tell "this port
     * was explicitly asked for a floor" from "this port inherited the
     * server-wide rate-loop bound". Both are still subject to the
     * network-difficulty ceiling -- see stratum_listener_t.min_diff for why
     * that is deliberate here and differs from upstream. */
    out->min_diff     = min_diff;
    out->initial_diff = initial > 0.0 ? initial : min_diff;
    if (out->vardiff_max > 0.0 && out->initial_diff > out->vardiff_max) {
        set_err(errbuf, errlen,
                "listener port %d: initial difficulty %g is above max_diff %g",
                out->port, out->initial_diff, out->vardiff_max);
        return -1;
    }
    return 0;
}

int proxy_config_load(const char *path, proxy_config_t *cfg,
                      char *errbuf, size_t errlen) {
    proxy_config_defaults(cfg);

    FILE *f = fopen(path, "r");
    if (!f) {
        set_err(errbuf, errlen, "cannot open config '%s': %s", path, strerror(errno));
        return -1;
    }

    char line[2048];
    int lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        strip_comment(line);

        char *trimmed = strtrim(line);
        if (*trimmed == '\0') continue;

        char *eq = strchr(trimmed, '=');
        if (!eq) {
            LOG_WARN("config: line %d: no '=' separator, skipping", lineno);
            continue;
        }
        *eq = '\0';
        char *k = strtrim(trimmed);
        char *v = strtrim(eq + 1);
        unquote(v);

        if      (strcmp(k, "listen_addr")               == 0) copy_str(cfg->listen_addr, sizeof cfg->listen_addr, v);
        else if (strcmp(k, "listen_port")               == 0) cfg->listen_port = atoi(v);
        else if (strcmp(k, "max_conns")                 == 0) cfg->max_conns = atoi(v);
        else if (strcmp(k, "listen_backlog")            == 0) cfg->listen_backlog = atoi(v);
        else if (strcmp(k, "initial_diff")              == 0) cfg->initial_diff = atof(v);
        else if (strcmp(k, "bitcoind_url")              == 0) copy_str(cfg->bitcoind_url, sizeof cfg->bitcoind_url, v);
        else if (strcmp(k, "bitcoind_user")             == 0) copy_str(cfg->bitcoind_user, sizeof cfg->bitcoind_user, v);
        else if (strcmp(k, "bitcoind_pass")             == 0) copy_str(cfg->bitcoind_pass, sizeof cfg->bitcoind_pass, v);
        else if (strcmp(k, "bitcoind_poll_interval_ms") == 0) cfg->bitcoind_poll_interval_ms = atoi(v);
        else if (strcmp(k, "operator_address")          == 0) copy_str(cfg->operator_address, sizeof cfg->operator_address, v);
        else if (strcmp(k, "fee_bps")                   == 0) cfg->fee_bps = atoi(v);
        else if (strcmp(k, "payout_address")            == 0) {
            set_err(errbuf, errlen,
                    "config: 'payout_address' is no longer supported; "
                    "rename to 'operator_address' (recipient of the "
                    "fee_bps cut; miners are paid directly via the "
                    "stratum username address)");
            fclose(f);
            return -3;
        }
        else if (strcmp(k, "coinbase_tag")              == 0) copy_str(cfg->coinbase_tag, sizeof cfg->coinbase_tag, v);
        else if (strcmp(k, "vardiff_enabled")           == 0) cfg->vardiff_enabled = atoi(v);
        else if (strcmp(k, "vardiff_target_spm")        == 0) cfg->vardiff_target_spm = atof(v);
        else if (strcmp(k, "vardiff_min")               == 0) cfg->vardiff_min = atof(v);
        else if (strcmp(k, "vardiff_max")               == 0) cfg->vardiff_max = atof(v);
        else if (strcmp(k, "vardiff_window_sec")        == 0) cfg->vardiff_window_sec = atoi(v);
        else if (strcmp(k, "vardiff_min_samples")       == 0) cfg->vardiff_min_samples = atoi(v);
        else if (strcmp(k, "max_suggested_diff")        == 0) cfg->max_suggested_diff = atof(v);
        else if (strcmp(k, "static_diff_enabled")      == 0) cfg->static_diff_enabled = atoi(v);
        else if (strcmp(k, "static_diff_min")          == 0) cfg->static_diff_min = atoi(v);
        else if (strcmp(k, "vardiff_max_window_mult")   == 0) cfg->vardiff_max_window_mult = atoi(v);
        else if (strcmp(k, "vardiff_idle_step")         == 0) cfg->vardiff_idle_step = atof(v);
        else if (strcmp(k, "clean_jobs_on_refresh")     == 0) cfg->clean_jobs_on_refresh = atoi(v);
        else if (strcmp(k, "idle_timeout_authorized_sec") == 0) cfg->idle_timeout_authorized_sec = atoi(v);
        else if (strcmp(k, "max_submits_per_sec")       == 0) cfg->max_submits_per_sec = atoi(v);
        else if (strcmp(k, "auth_max_failures")         == 0) cfg->auth_max_failures = atoi(v);
        else if (strcmp(k, "auth_fail_lockout_sec")     == 0) cfg->auth_fail_lockout_sec = atoi(v);
        else if (strcmp(k, "listener")                  == 0) {
            char lerr[160];
            if (cfg->listener_count >= STRATUM_MAX_LISTENERS) {
                set_err(errbuf, errlen, "config: line %d: at most %d listeners",
                        lineno, STRATUM_MAX_LISTENERS);
                fclose(f);
                return -1;
            }
            stratum_listener_t l;
            if (parse_listener(v, &l, lerr, sizeof lerr) != 0) {
                set_err(errbuf, errlen, "config: line %d: %s", lineno, lerr);
                fclose(f);
                return -1;
            }
            cfg->listeners[cfg->listener_count++] = l;
        }
        else if (strcmp(k, "rental_listen_port")        == 0) cfg->rental_listen_port = atoi(v);
        else if (strcmp(k, "rental_min_diff")           == 0) cfg->rental_min_diff = atof(v);
        else if (strcmp(k, "rental_max_conns")          == 0) cfg->rental_max_conns = atoi(v);
        else if (strcmp(k, "idle_timeout_sec")          == 0) cfg->idle_timeout_sec = atoi(v);
        else if (strcmp(k, "db_path")                   == 0) copy_str(cfg->db_path, sizeof cfg->db_path, v);
        else if (strcmp(k, "commit_window_ms")          == 0) cfg->commit_window_ms = atoi(v);
        else if (strcmp(k, "commit_max_shares")         == 0) cfg->commit_max_shares = atoi(v);
        else if (strcmp(k, "templates_retention_days")  == 0) cfg->templates_retention_days = atoi(v);
        else if (strcmp(k, "redis_url")                 == 0) copy_str(cfg->redis_url, sizeof cfg->redis_url, v);
        else if (strcmp(k, "redis_publish_timeout_ms")  == 0) cfg->redis_publish_timeout_ms = atoi(v);
        else if (strcmp(k, "redis_reconnect_backoff_ms")== 0) cfg->redis_reconnect_backoff_ms = atoi(v);
        else if (strcmp(k, "pool_mode")                 == 0) copy_str(cfg->pool_mode, sizeof cfg->pool_mode, v);
        else if (strcmp(k, "pool_btc_address")          == 0) copy_str(cfg->pool_btc_address, sizeof cfg->pool_btc_address, v);
        else if (strcmp(k, "pps_sats_per_diff")         == 0) cfg->pps_sats_per_diff = atof(v);
        else if (strcmp(k, "prop_window_k")             == 0) cfg->prop_window_k = atof(v);
        else if (strcmp(k, "prop_min_payout_sats")      == 0) cfg->prop_min_payout_sats = (int64_t)atoll(v);
        else if (strcmp(k, "prop_max_outputs")          == 0) cfg->prop_max_outputs = atoi(v);
        else if (strcmp(k, "prop_carry_slots")          == 0) cfg->prop_carry_slots = atoi(v);
        else if (strcmp(k, "prop_max_coinbase_bytes")   == 0) cfg->prop_max_coinbase_bytes = atoi(v);
        else if (strcmp(k, "prop_window_min_sec")       == 0) cfg->prop_window_min_sec = atoi(v);
        else if (strcmp(k, "pps_min_network_difficulty") == 0) cfg->pps_min_network_difficulty = atof(v);
        else if (strcmp(k, "block_interval_sec")        == 0) cfg->block_interval_sec = atoi(v);
        else if (strcmp(k, "pps_refuse_shares_below_min") == 0) cfg->pps_refuse_shares_below_min = atoi(v);
        /* Retired with pool_mode=pps (the drivechain-in-coinbase build).
         * Accepted and ignored so an existing proxy.conf keeps loading;
         * the Thunder reserve address now lives only on the dashboard,
         * which owns the deposit flow. */
        else if (strcmp(k, "pool_thunder_reserve_address") == 0 ||
                 strcmp(k, "thunder_sidechain_number")     == 0 ||
                 strcmp(k, "thunder_op_return_hex")        == 0) {
            LOG_WARN("config: line %d: '%s' is obsolete and ignored "
                     "(pool_mode=pps was removed)", lineno, k);
        }
        else if (strcmp(k, "log_level")                 == 0) {
            int lv = parse_log_level(v);
            if (lv < 0) {
                LOG_WARN("config: line %d: unknown log_level '%s'", lineno, v);
            } else {
                cfg->log_level = lv;
            }
        }
        else {
            LOG_WARN("config: line %d: unknown key '%s'", lineno, k);
        }
    }

    fclose(f);

    if (cfg->operator_address[0] == '\0') {
        set_err(errbuf, errlen, "config: 'operator_address' is required");
        return -2;
    }
    if (cfg->fee_bps < 0 || cfg->fee_bps > 1000) {
        set_err(errbuf, errlen,
                "config: 'fee_bps' must be in [0, 1000] (0%% to 10%%), got %d",
                cfg->fee_bps);
        return -4;
    }
    if (strcmp(cfg->pool_mode, "pps") == 0) {
        set_err(errbuf, errlen,
                "config: 'pool_mode = pps' was removed — the BIP300 enforcer "
                "does not credit coinbase outputs as drivechain deposits, so "
                "that mode stranded the block reward. Use 'pps-classic'.");
        return -5;
    }
    if (strcmp(cfg->pool_mode, "solo")        != 0 &&
        strcmp(cfg->pool_mode, "pps-classic") != 0 &&
        strcmp(cfg->pool_mode, "proportional") != 0) {
        set_err(errbuf, errlen,
                "config: 'pool_mode' must be 'solo', 'pps-classic' or 'proportional', got '%s'",
                cfg->pool_mode);
        return -5;
    }
    if (strcmp(cfg->pool_mode, "pps-classic") == 0) {
        /* pps_sats_per_diff is no longer required: unset means the rate is
         * derived per-template from coinbasevalue, network difficulty and
         * fee_bps. A negative value is still a typo worth rejecting. */
        if (cfg->pps_sats_per_diff < 0.0) {
            set_err(errbuf, errlen,
                    "config: 'pps_sats_per_diff' must be > 0 when set "
                    "(omit it to derive the rate from the block template)");
            return -8;
        }
        if (cfg->pool_btc_address[0] == '\0') {
            set_err(errbuf, errlen,
                    "config: 'pool_btc_address' is required when pool_mode=pps-classic");
            return -9;
        }
        if (cfg->pps_min_network_difficulty < 0.0) {
            set_err(errbuf, errlen,
                    "config: 'pps_min_network_difficulty' cannot be negative "
                    "(0 disables the check)");
            return -10;
        }
    }
    if (cfg->block_interval_sec <= 0) {
        set_err(errbuf, errlen,
                "config: 'block_interval_sec' must be > 0 (600 for Bitcoin)");
        return -11;
    }
    if (strcmp(cfg->pool_mode, "proportional") == 0) {
        if (cfg->prop_window_k <= 0.0) {
            set_err(errbuf, errlen,
                    "config: 'prop_window_k' must be > 0");
            return -10;
        }
        if (cfg->prop_min_payout_sats < COINBASE_DUST_SATS) {
            set_err(errbuf, errlen,
                    "config: 'prop_min_payout_sats' must be at least %d (dust)",
                    COINBASE_DUST_SATS);
            return -11;
        }
        if (cfg->prop_max_outputs < 1 || cfg->prop_max_outputs > 64) {
            set_err(errbuf, errlen,
                    "config: 'prop_max_outputs' must be in [1, 64]");
            return -12;
        }
        /* Must leave the largest claim a slot. The selection clamps this per
         * template anyway (the cap moves block to block with the byte budget),
         * but a config that reads "reserve 12 of 12" says something the pool
         * will never do, and a value that silently means something else is how
         * a knob becomes folklore. Refuse it here where it can be corrected. */
        if (cfg->prop_carry_slots < 0 ||
            cfg->prop_carry_slots >= cfg->prop_max_outputs) {
            set_err(errbuf, errlen,
                    "config: 'prop_carry_slots' must be in [0, prop_max_outputs "
                    "- 1] -- it reserves slots WITHIN prop_max_outputs, and the "
                    "largest claim always keeps one");
            return -18;
        }
        /* The floor is not 0-or-anything: a budget too small to hold the
         * template's own coinbase plus one payout cannot be satisfied by
         * dropping outputs, so it would silently mean "pay one miner" forever.
         * 400 B is below anything this pool has ever produced (702 B at 16
         * payouts) and still well above a bare template. */
        if (cfg->prop_max_coinbase_bytes != 0 &&
            (cfg->prop_max_coinbase_bytes < 400 ||
             cfg->prop_max_coinbase_bytes > 100000)) {
            set_err(errbuf, errlen,
                    "config: 'prop_max_coinbase_bytes' must be 0 (off) or in "
                    "[400, 100000]");
            return -17;
        }
        if (cfg->prop_window_min_sec < 0) {
            set_err(errbuf, errlen,
                    "config: 'prop_window_min_sec' must be >= 0 "
                    "(0 disables the floor, which collapses the window at a "
                    "difficulty reset — see config.h)");
            return -13;
        }
    }
    if (cfg->max_submits_per_sec < 0) {
        set_err(errbuf, errlen,
                "config: 'max_submits_per_sec' cannot be negative "
                "(0 disables the ceiling)");
        return -14;
    }
    if (cfg->auth_max_failures < 0) {
        set_err(errbuf, errlen,
                "config: 'auth_max_failures' cannot be negative (0 disables)");
        return -14;
    }
    if (cfg->auth_max_failures > 0 && cfg->auth_fail_lockout_sec <= 0) {
        /* A budget with no window is a per-connection limit only, and a
         * client dodges that by reconnecting. Refuse to boot half-armed. */
        set_err(errbuf, errlen,
                "config: 'auth_fail_lockout_sec' must be > 0 when "
                "'auth_max_failures' is set");
        return -14;
    }

    /* 🔴 vardiff_min must be a real number > 0, because `sd=` floors a pinned
     * connection at it. Left unvalidated, `vardiff_min = 0` (or any malformed
     * value, since atof yields 0) makes the floor check `vd_min > 0.0` skip
     * entirely — the guard reads as present in the source while doing nothing,
     * and the only symptom is a miner pinning arbitrarily low.
     * Same shape as the max_submits_per_sec check above: refuse to boot rather
     * than run with a guard that is silently switched off. */
    /* ⛔ GATED ON THE FEATURE THAT NEEDS IT. Unconditional, this refuses to
     * boot on a config we cannot read from here — and the failure mode is the
     * pool not starting, discovered at restart, inside a rental-free window,
     * with stratum down. Nothing about `sd=` is on by default, so a default
     * deploy would be taking that risk for no benefit. Gated, the check bites
     * when someone turns pins ON, which is the right place for it. */
    /* The pin floor is static_diff_min, NOT vardiff_min — deliberately, so the
     * bound belongs to this feature rather than to a value it does not own.
     * Validate the one that decides it. */
    if (cfg->static_diff_enabled && cfg->static_diff_min <= 0) {
        set_err(errbuf, errlen,
                "config: 'static_diff_enabled' requires 'static_diff_min' "
                "greater than 0 — it is the floor a static-difficulty request "
                "is clamped to, and a 0 floor silently disables that clamp");
        return -18;
    }
    /* Two listeners on one port, or a listener on listen_port. Caught here
     * because the alternative is a bind() that fails at startup with EADDRINUSE
     * and no indication of which of the operator's two lines was the mistake. */
    for (int i = 0; i < cfg->listener_count; ++i) {
        if (cfg->listeners[i].port == cfg->listen_port) {
            set_err(errbuf, errlen,
                    "config: listener port %d is already listen_port — give "
                    "the extra listener a different port",
                    cfg->listeners[i].port);
            return -15;
        }
        if (cfg->rental_listen_port > 0 &&
            cfg->listeners[i].port == cfg->rental_listen_port) {
            set_err(errbuf, errlen,
                    "config: listener port %d is already rental_listen_port — "
                    "use one or the other, not both",
                    cfg->listeners[i].port);
            return -15;
        }
        for (int j = 0; j < i; ++j) {
            if (cfg->listeners[i].port == cfg->listeners[j].port) {
                set_err(errbuf, errlen,
                        "config: two listeners both use port %d",
                        cfg->listeners[i].port);
                return -16;
            }
        }
    }
    /* A rental port whose floor is under the marketplace threshold is legal —
     * an operator may have a private customer with other requirements — but
     * it is the setting that gets a public port refused, so name it rather
     * than let it pass unremarked. Braiins wants 1024 minimum and 65536
     * recommended; NiceHash wants 500000. */
    for (int i = 0; i < cfg->listener_count; ++i) {
        if (cfg->listeners[i].min_diff > 0.0 &&
            cfg->listeners[i].min_diff < 1024.0) {
            /* %g, not %.0f: a difficulty is not necessarily >= 1. On a
             * forknet these are values like 3e-10, and %.0f renders every
             * one of them as "0" — a warning naming the wrong number is
             * worse than no warning. */
            LOG_WARN("config: listener port %d promises min_diff %g, below "
                     "the 1024 floor rented-hashrate marketplaces require; a "
                     "port advertised for rental at this level can be refused",
                     cfg->listeners[i].port, cfg->listeners[i].min_diff);
        }
    }
    return 0;
}
