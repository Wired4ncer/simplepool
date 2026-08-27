/* Config parsing. Focused on the comment/quoting rules, because getting them
 * wrong is silent: the pool boots, then fails to authenticate to bitcoind with
 * nothing in the log that points at the config file. */

#include "../src/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

static const char *VALID_ADDR = "bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4";

/* Write `body` to a temp file and load it. Returns the load's return code. */
static int load_text(const char *body, proxy_config_t *cfg,
                     char *err, size_t errlen) {
    char path[] = "/tmp/simplepool_test_conf_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { printf("FAIL: mkstemp\n"); failures++; return -99; }
    FILE *f = fdopen(fd, "w");
    fputs(body, f);
    fclose(f);
    int rc = proxy_config_load(path, cfg, err, errlen);
    unlink(path);
    return rc;
}

/* A '#' inside a value is data, not a comment introducer.
 *
 * The old parser cut the line at the FIRST '#' anywhere, before the key/value
 * split and before unquoting — so this password silently became "p", and
 * quoting did not help either. */
static void test_hash_inside_value_is_kept(void) {
    proxy_config_t cfg; char err[256] = {0};
    char body[512];
    snprintf(body, sizeof body,
             "operator_address = %s\n"
             "bitcoind_pass = p#ssw0rd\n"
             "bitcoind_user = a#b#c\n",
             VALID_ADDR);
    int rc = load_text(body, &cfg, err, sizeof err);
    CHECK(rc == 0);
    CHECK(strcmp(cfg.bitcoind_pass, "p#ssw0rd") == 0);
    CHECK(strcmp(cfg.bitcoind_user, "a#b#c") == 0);
}

/* Quoted values keep everything inside the quotes, including whitespace-led
 * '#' that would otherwise start a comment. */
static void test_quoted_value_keeps_hash(void) {
    proxy_config_t cfg; char err[256] = {0};
    char body[512];
    snprintf(body, sizeof body,
             "operator_address = %s\n"
             "coinbase_tag = \"tag #7 rules\"\n",
             VALID_ADDR);
    int rc = load_text(body, &cfg, err, sizeof err);
    CHECK(rc == 0);
    CHECK(strcmp(cfg.coinbase_tag, "tag #7 rules") == 0);
}

/* Inline comments still work — proxy.conf.example documents them, so this is
 * the behaviour the fix had to preserve. */
static void test_inline_comment_still_strips(void) {
    proxy_config_t cfg; char err[256] = {0};
    char body[512];
    snprintf(body, sizeof body,
             "# a whole-line comment\n"
             "operator_address = %s\n"
             "listen_port = 3333   # which port to bind\n"
             "coinbase_tag = plain # trailing\n",
             VALID_ADDR);
    int rc = load_text(body, &cfg, err, sizeof err);
    CHECK(rc == 0);
    CHECK(cfg.listen_port == 3333);
    CHECK(strcmp(cfg.coinbase_tag, "plain") == 0);
}

/* An operator_address that is not a decodable Bitcoin address must not reach
 * the mining path — every coinbase render would fail at the first share. */
static void test_rejects_bad_operator_address(void) {
    proxy_config_t cfg; char err[256] = {0};
    int rc = load_text("operator_address = not-an-address\n",
                       &cfg, err, sizeof err);
    /* config_load itself accepts the string; main.c is where it is decoded.
     * What must hold here is that the value arrives INTACT, so that check can
     * do its job rather than validating a truncated string. */
    CHECK(rc == 0);
    CHECK(strcmp(cfg.operator_address, "not-an-address") == 0);
}

/* The rental port defaults to OFF, so an existing config file keeps behaving
 * exactly as it did. The floor defaults to a value that clears both
 * marketplaces (Braiins >= 1024/recommends 65536, NiceHash 500000) but is
 * inert until a port is actually set. */
static void test_rental_port_defaults_off(void) {
    proxy_config_t cfg; char err[256] = {0};
    char body[512];
    snprintf(body, sizeof body,
             "operator_address = %s\n"
             "listen_port = 3334\n", VALID_ADDR);
    int rc = load_text(body, &cfg, err, sizeof err);
    CHECK(rc == 0);
    CHECK(cfg.rental_listen_port == 0);
    CHECK(cfg.rental_min_diff == 500000.0);
    CHECK(cfg.rental_max_conns == 0);
}

static void test_rental_port_parses(void) {
    proxy_config_t cfg; char err[256] = {0};
    char body[512];
    snprintf(body, sizeof body,
             "operator_address = %s\n"
             "listen_port = 3334\n"
             "rental_listen_port = 3335\n"
             "rental_min_diff = 65536\n"
             "rental_max_conns = 32\n", VALID_ADDR);
    int rc = load_text(body, &cfg, err, sizeof err);
    CHECK(rc == 0);
    CHECK(cfg.rental_listen_port == 3335);
    CHECK(cfg.rental_min_diff == 65536.0);
    CHECK(cfg.rental_max_conns == 32);
    /* The public port is untouched by any of it. */
    CHECK(cfg.listen_port == 3334);
    CHECK(cfg.initial_diff == 1.0);
}

/* The backlog defaults to 0, which stratum.c reads as "use
 * STRATUM_DEFAULT_BACKLOG". 0 is the sentinel on purpose: config.c must not
 * depend on the stratum header just to name a default. */
static void test_listen_backlog_defaults_to_sentinel(void) {
    proxy_config_t cfg; char err[256] = {0};
    char body[512];
    snprintf(body, sizeof body,
             "operator_address = %s\n"
             "listen_port = 3334\n", VALID_ADDR);
    int rc = load_text(body, &cfg, err, sizeof err);
    CHECK(rc == 0);
    CHECK(cfg.listen_backlog == 0);
}

/* An explicit backlog is what lets a marketplace burst be absorbed without a
 * rebuild. It was hardcoded at 64, and TcpExt:ListenOverflows reached 769 in
 * production before that was noticed -- a dropped SYN produces no accept()
 * error, so there is nothing in the pool's own logs to find. */
static void test_listen_backlog_parses(void) {
    proxy_config_t cfg; char err[256] = {0};
    char body[512];
    snprintf(body, sizeof body,
             "operator_address = %s\n"
             "listen_port = 3334\n"
             "listen_backlog = 2048\n", VALID_ADDR);
    int rc = load_text(body, &cfg, err, sizeof err);
    CHECK(rc == 0);
    CHECK(cfg.listen_backlog == 2048);
    /* Nothing else moved. */
    CHECK(cfg.max_conns == 500);
    CHECK(cfg.listen_port == 3334);
}

/* The new listener spelling parses, and every field lands where the accept
 * path reads it. min_diff sets three things at once — the rate-loop floor, the
 * starting difficulty, and the PROMISE — and the last of those is what
 * survives the network-difficulty ceiling, so it is recorded separately. */
static void test_listener_line_parses(void) {
    proxy_config_t cfg; char err[256] = {0};
    char body[512];
    snprintf(body, sizeof body,
             "operator_address = %s\n"
             "listen_port = 3334\n"
             "listener = port=3335 min_diff=500000 label=braiins\n", VALID_ADDR);
    int rc = load_text(body, &cfg, err, sizeof err);
    CHECK(rc == 0);
    CHECK(cfg.listener_count == 1);
    CHECK(cfg.listeners[0].port == 3335);
    /* `min_diff=` drives THREE fields, and the two that matter operationally
     * are vardiff_min and initial_diff — that pairing is the whole point of a
     * rental port, because the miner has to arrive already at the floor.
     * min_diff itself only records that the operator asked, for the
     * marketplace warning and the startup log. ⛔ It is NOT a second
     * enforcement path that outranks the network clamp, which is where
     * upstream differs — see stratum_listener_t.min_diff. */
    CHECK(cfg.listeners[0].vardiff_min == 500000.0);
    CHECK(cfg.listeners[0].initial_diff == 500000.0);
    CHECK(cfg.listeners[0].min_diff == 500000.0);
    CHECK(strcmp(cfg.listeners[0].label, "braiins") == 0);
    /* The public port is untouched by any of it. */
    CHECK(cfg.listen_port == 3334);
    CHECK(cfg.initial_diff == 1.0);
}

/* mode=solo marks the listener, and — the part that actually matters — a
 * listener WITHOUT it stays proportional. Asserting only the solo port would
 * pass on a build that set solo=1 unconditionally. */
static void test_listener_mode_solo(void) {
    proxy_config_t cfg; char err[256] = {0};
    char body[640];
    snprintf(body, sizeof body,
             "operator_address = %s\n"
             "listen_port = 3334\n"
             "listener = port=3335 min_diff=500000 label=braiins\n"
             "listener = port=3336 mode=solo label=solo\n", VALID_ADDR);
    int rc = load_text(body, &cfg, err, sizeof err);
    CHECK(rc == 0);
    CHECK(cfg.listener_count == 2);
    /* The rental port did NOT become solo just because a solo port exists. */
    CHECK(cfg.listeners[0].port == 3335);
    CHECK(cfg.listeners[0].solo == 0);
    CHECK(cfg.listeners[1].port == 3336);
    CHECK(cfg.listeners[1].solo == 1);
    /* mode does not disturb the difficulty policy it shares a line with. */
    CHECK(cfg.listeners[1].vardiff_min == 0.0);
    CHECK(strcmp(cfg.listeners[1].label, "solo") == 0);
}

/* mode=proportional is accepted and means what it says, so an operator can
 * state the intent on the public port rather than relying on the absence of
 * mode=solo. */
static void test_listener_mode_proportional_explicit(void) {
    proxy_config_t cfg; char err[256] = {0};
    char body[512];
    snprintf(body, sizeof body,
             "operator_address = %s\n"
             "listener = port=3337 mode=proportional\n", VALID_ADDR);
    CHECK(load_text(body, &cfg, err, sizeof err) == 0);
    CHECK(cfg.listeners[0].solo == 0);
}

/* An unknown mode is refused AT STARTUP rather than silently defaulting.
 * Silently treating "Solo" or "sole" as proportional would put a miner who
 * asked for solo into the shared payout window without anyone noticing. */
static void test_listener_mode_typo_is_refused(void) {
    proxy_config_t cfg; char err[256] = {0};
    char body[512];
    snprintf(body, sizeof body,
             "operator_address = %s\n"
             "listener = port=3336 mode=sollo\n", VALID_ADDR);
    int rc = load_text(body, &cfg, err, sizeof err);
    CHECK(rc != 0);
    CHECK(strstr(err, "mode") != NULL);
}

/* A config that names no listener has none — which is what makes installing
 * this binary a no-op for an operator still on the rental_* keys. */
static void test_listeners_default_to_none(void) {
    proxy_config_t cfg; char err[256] = {0};
    char body[512];
    snprintf(body, sizeof body,
             "operator_address = %s\n"
             "listen_port = 3334\n", VALID_ADDR);
    int rc = load_text(body, &cfg, err, sizeof err);
    CHECK(rc == 0);
    CHECK(cfg.listener_count == 0);
}

/* Two ports that collide must be refused at config time. The alternative is a
 * bind() failing at startup with EADDRINUSE and no indication of WHICH of the
 * operator's two lines was the mistake. */
static void test_listener_colliding_with_listen_port_is_refused(void) {
    proxy_config_t cfg; char err[256] = {0};
    char body[512];
    snprintf(body, sizeof body,
             "operator_address = %s\n"
             "listen_port = 3334\n"
             "listener = port=3334 min_diff=500000\n", VALID_ADDR);
    int rc = load_text(body, &cfg, err, sizeof err);
    CHECK(rc != 0);
    CHECK(strstr(err, "3334") != NULL);
}

/* The same, for an operator mid-migration who has written both spellings of
 * the rental port. Silently honouring one and dropping the other is how a
 * rental port ends up bound at the wrong difficulty. */
static void test_listener_colliding_with_rental_port_is_refused(void) {
    proxy_config_t cfg; char err[256] = {0};
    char body[512];
    snprintf(body, sizeof body,
             "operator_address = %s\n"
             "listen_port = 3334\n"
             "rental_listen_port = 3335\n"
             "listener = port=3335 min_diff=500000\n", VALID_ADDR);
    int rc = load_text(body, &cfg, err, sizeof err);
    CHECK(rc != 0);
    CHECK(strstr(err, "3335") != NULL);
}

/* ⛔ The submit ceiling ships OFF. Upstream defaults it to 20000; refusing a
 * submit is miner-visible and gets opened by measurement, not by a default. */
static void test_max_submits_per_sec_defaults_off(void) {
    proxy_config_t cfg; char err[256] = {0};
    char body[512];
    snprintf(body, sizeof body,
             "operator_address = %s\n"
             "listen_port = 3334\n", VALID_ADDR);
    int rc = load_text(body, &cfg, err, sizeof err);
    CHECK(rc == 0);
    CHECK(cfg.max_submits_per_sec == 0);
}

/* An operator typo in a label must fail at startup rather than reach the DB
 * and the dashboard unescaped. */
static void test_listener_label_is_constrained(void) {
    proxy_config_t cfg; char err[256] = {0};
    char body[512];
    snprintf(body, sizeof body,
             "operator_address = %s\n"
             "listen_port = 3334\n"
             "listener = port=3335 min_diff=500000 label=bad\"quote\n", VALID_ADDR);
    int rc = load_text(body, &cfg, err, sizeof err);
    CHECK(rc != 0);
}

int main(void) {
    printf("running test_config...\n");
    test_hash_inside_value_is_kept();
    test_quoted_value_keeps_hash();
    test_inline_comment_still_strips();
    test_rejects_bad_operator_address();
    test_rental_port_defaults_off();
    test_rental_port_parses();
    test_listen_backlog_defaults_to_sentinel();
    test_listen_backlog_parses();
    test_listener_line_parses();
    test_listener_mode_solo();
    test_listener_mode_proportional_explicit();
    test_listener_mode_typo_is_refused();
    test_listeners_default_to_none();
    test_listener_colliding_with_listen_port_is_refused();
    test_listener_colliding_with_rental_port_is_refused();
    test_max_submits_per_sec_defaults_off();
    test_listener_label_is_constrained();
    if (failures) { printf("test_config: %d failed\n", failures); return 1; }
    printf("test_config: all tests passed\n");
    return 0;
}
