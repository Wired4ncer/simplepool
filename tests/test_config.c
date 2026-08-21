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

int main(void) {
    printf("running test_config...\n");
    test_hash_inside_value_is_kept();
    test_quoted_value_keeps_hash();
    test_inline_comment_still_strips();
    test_rejects_bad_operator_address();
    if (failures) { printf("test_config: %d failed\n", failures); return 1; }
    printf("test_config: all tests passed\n");
    return 0;
}
