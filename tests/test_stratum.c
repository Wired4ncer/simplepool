#include "../src/stratum.h"
#include "../src/share.h"
#include "../src/cjson/cJSON.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static int g_pass = 0;
static int g_fail = 0;
#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

/* Observers + state. */
typedef struct {
    int    shares;
    int    rejects;
    int    blocks;
    int    last_is_block;
    double last_difficulty;   /* what the share was CREDITED at */
    char   last_worker[64];
    char   last_reason[128];
} obs_t;

/* The callbacks are invoked from whichever thread handled the share, and
 * test_job_rotation_races_submits drives several at once. Guard the observer
 * so TSan reports races in the CODE UNDER TEST rather than in the harness
 * watching it. Single-threaded tests pay one uncontended lock per callback. */
static pthread_mutex_t obs_mu = PTHREAD_MUTEX_INITIALIZER;

static void on_share(void *ctx, const char *w, const char *addr,
                     uint64_t ts, double d,
                     int is_block, const char *blk) {
    (void)ts; (void)blk; (void)addr;
    obs_t *o = ctx;
    pthread_mutex_lock(&obs_mu);
    o->shares++;
    o->last_difficulty = d;
    o->last_is_block = is_block;
    if (is_block) o->blocks++;
    snprintf(o->last_worker, sizeof(o->last_worker), "%s", w ? w : "");
    pthread_mutex_unlock(&obs_mu);
}
static void on_reject(void *ctx, const char *w, uint64_t ts, const char *r) {
    (void)ts; (void)w;
    obs_t *o = ctx;
    pthread_mutex_lock(&obs_mu);
    o->rejects++;
    snprintf(o->last_reason, sizeof(o->last_reason), "%s", r ? r : "");
    pthread_mutex_unlock(&obs_mu);
}
/* Returns 0 = the node accepted it, so the existing tests keep exercising
 * the on_block_found path. A test for the rejected case sets its own. */
static int on_block(void *ctx, const char *hex) { (void)ctx; (void)hex; return 0; }

/* Helper: parse the first line of an output buffer. Mutates buf (NUL terminator). */
static cJSON *parse_first_line(char *buf) {
    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';
    return cJSON_Parse(buf);
}

/* usleep is gone from POSIX.1-2008 (which the Makefile requests), so glibc
 * hides its declaration; nanosleep is the conforming replacement. */
static void sleep_ms(long ms) {
    struct timespec ts = { .tv_sec = ms / 1000,
                           .tv_nsec = (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* Helper: count newline-delimited messages. */
static int count_lines(const char *buf, size_t len) {
    int n = 0;
    for (size_t i = 0; i < len; ++i) if (buf[i] == '\n') n++;
    return n;
}

/* Standard regtest P2WPKH used in fixtures so the per-connection coinbase
 * renderer can produce a valid scriptPubKey. */
#define TEST_ADDR "bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080"

/* Build a tiny job for tests. The coinbase is rendered per-connection at
 * notify/submit time using the miner's address, so the job only carries
 * template-level data. */
/* extranonce2 for submits, sized to the advertised width. The static assert
 * is the point: widen STRATUM_EXTRANONCE2_SIZE without updating this and the
 * tests fail to COMPILE, rather than a wrong-width coinbase reaching a real
 * block and being rejected by the network. */
#define TEST_EN2 "deadbeefdeadbeef"
_Static_assert(sizeof(TEST_EN2) - 1 == STRATUM_EXTRANONCE2_SIZE * 2,
               "TEST_EN2 must be STRATUM_EXTRANONCE2_SIZE bytes of hex");

static stratum_job_t *make_test_job(const char *job_id,
                                    const uint8_t *network_target_be) {
    uint8_t prev[32] = {0};
    return stratum_job_new(job_id, 1, prev,
                           /*value_sats*/ 5000000000LL,
                           /*wc_hex*/ NULL,
                           /*en1*/ STRATUM_EXTRANONCE1_SIZE,
                           /*en2*/ STRATUM_EXTRANONCE2_SIZE,
                           NULL, 0, 0x1d00ffffu, 0x60000000u,
                           network_target_be, 800000, NULL, 0,
                           /*coinbasetxn_hex*/ NULL,
                           /*coinbase_has_witness*/ 0);
}

static void test_subscribe(void) {
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1, .initial_diff = 1.0 };
    /* Don't start the server — we exercise just the handler. */
    stratum_server_t *s = NULL;
    /* Hack: synthesize a server by calling start with port 0 -> kernel
     * picks one. */
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    int rc = stratum_server_start(&cfg, &s);
    CHECK(rc == 0); CHECK(s != NULL);
    if (!s) return;

    stratum_conn_t *c = stratum_conn_new_for_test(s);
    char *out = NULL; size_t olen = 0;
    rc = stratum_handle_message(s, c,
        "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[\"x\"]}",
        &out, &olen);
    CHECK(rc == 0);
    CHECK(out != NULL && olen > 0);
    cJSON *resp = parse_first_line(out);
    CHECK(resp != NULL);
    if (resp) {
        cJSON *result = cJSON_GetObjectItem(resp, "result");
        CHECK(cJSON_IsArray(result));
        CHECK(cJSON_GetArraySize(result) == 3);
        cJSON *subs = cJSON_GetArrayItem(result, 0);
        CHECK(cJSON_IsArray(subs) && cJSON_GetArraySize(subs) == 2);
        cJSON *ex1 = cJSON_GetArrayItem(result, 1);
        CHECK(cJSON_IsString(ex1) &&
              strlen(ex1->valuestring) == STRATUM_EXTRANONCE1_SIZE * 2);
        cJSON *ex2sz = cJSON_GetArrayItem(result, 2);
        /* The width marketplaces gate on. Braiins Hashpower rejects a pool
         * URL advertising < 7 outright, before a share is ever submitted. */
        CHECK(cJSON_IsNumber(ex2sz) &&
              ex2sz->valueint == STRATUM_EXTRANONCE2_SIZE);
        CHECK(ex2sz->valueint >= 7);
        cJSON_Delete(resp);
    }
    free(out);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

static void test_authorize_triggers_setdiff_notify(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1, .initial_diff = 1.0,
                           .ctx = &obs };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    int rc = stratum_server_start(&cfg, &s);
    CHECK(rc == 0);

    /* Provide a job so notify can be sent. */
    uint8_t net[32]; memset(net, 0xff, 32);
    stratum_server_set_job(s, make_test_job("0001", net));

    stratum_conn_t *c = stratum_conn_new_for_test(s);
    char *out = NULL; size_t olen = 0;
    /* Subscribe first to get extranonce1. */
    stratum_handle_message(s, c, "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}",
                           &out, &olen);
    free(out); out = NULL; olen = 0;

    rc = stratum_handle_message(s, c,
        "{\"id\":2,\"method\":\"mining.authorize\","
         "\"params\":[\"" TEST_ADDR ".w1\",\"x\"]}",
        &out, &olen);
    CHECK(rc == 0);
    /* Expect 3 lines: response(true), set_difficulty, notify. */
    int n = count_lines(out, olen);
    CHECK(n == 3);
    CHECK(strstr(out, "mining.set_difficulty") != NULL);
    CHECK(strstr(out, "mining.notify") != NULL);
    CHECK(strcmp(stratum_conn_worker_name_for_test(c),
                 TEST_ADDR ".w1") == 0);
    CHECK(strcmp(stratum_conn_payout_address_for_test(c), TEST_ADDR) == 0);
    free(out);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

static void test_submit_unknown_job(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1, .initial_diff = 1.0,
                           .ctx = &obs, .on_share = on_share,
                           .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);
    /* No job set. */
    stratum_conn_t *c = stratum_conn_new_for_test(s);
    char *out = NULL; size_t olen = 0;
    stratum_handle_message(s, c, "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}",
                           &out, &olen); free(out); out=NULL; olen=0;
    stratum_handle_message(s, c,
        "{\"id\":2,\"method\":\"mining.authorize\","
         "\"params\":[\"" TEST_ADDR "\",\"x\"]}",
        &out, &olen); free(out); out=NULL; olen=0;

    int rc = stratum_handle_message(s, c,
        "{\"id\":3,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"NOPE\",\"00000000\",\"60000000\",\"00000000\"]}",
        &out, &olen);
    CHECK(rc == 0);
    CHECK(obs.rejects == 1);
    CHECK(strstr(obs.last_reason, "stale") != NULL);
    /* response should carry an error array */
    CHECK(strstr(out, "\"error\"") != NULL);
    free(out);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

static void test_submit_share_and_dedupe(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1,
                           /* tiny diff -> worker target = max -> any hash passes */
                           .initial_diff = 1e-12,
                           .ctx = &obs, .on_share = on_share,
                           .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);

    /* Network target = all zeros -> never a block. */
    uint8_t net[32] = {0};
    stratum_server_set_job(s, make_test_job("J1", net));

    stratum_conn_t *c = stratum_conn_new_for_test(s);
    char *out = NULL; size_t olen = 0;
    stratum_handle_message(s, c, "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}",
                           &out, &olen); free(out); out=NULL; olen=0;
    stratum_handle_message(s, c,
        "{\"id\":2,\"method\":\"mining.authorize\","
         "\"params\":[\"" TEST_ADDR "\",\"x\"]}",
        &out, &olen); free(out); out=NULL; olen=0;

    int rc = stratum_handle_message(s, c,
        "{\"id\":3,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J1\",\"" TEST_EN2 "\",\"60000000\",\"00000001\"]}",
        &out, &olen);
    CHECK(rc == 0);
    CHECK(obs.shares == 1);
    CHECK(obs.last_is_block == 0);
    CHECK(obs.blocks == 0);
    free(out); out=NULL; olen=0;

    /* Duplicate: same parameters again. */
    rc = stratum_handle_message(s, c,
        "{\"id\":4,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J1\",\"" TEST_EN2 "\",\"60000000\",\"00000001\"]}",
        &out, &olen);
    CHECK(rc == 0);
    CHECK(obs.shares == 1);  /* not incremented */
    CHECK(obs.rejects >= 1);
    CHECK(strstr(obs.last_reason, "duplicate") != NULL);
    free(out);

    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

/* An extranonce2 of any width other than the 4 bytes advertised at subscribe
 * must be refused.
 *
 * The coinbase reserves exactly en1_size + en2_size bytes inside a scriptSig
 * whose length prefix is already committed, so a different width produces a
 * transaction that cannot be deserialised. Such a share still hashes and can
 * still beat the target — it would be credited, and if it solved the network
 * target the pool would submit a malformed block and lose the reward.
 *
 * The first assertion is the load-bearing one: it proves this fixture reaches
 * the accept path at all, so the rejections below are the length check firing
 * and not some earlier guard quietly eating every submit. */
static void test_submit_rejects_wrong_extranonce2_size(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1,
                           .initial_diff = 1e-12,   /* any hash passes */
                           .ctx = &obs, .on_share = on_share,
                           .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);

    uint8_t net[32] = {0};
    stratum_server_set_job(s, make_test_job("J1", net));

    stratum_conn_t *c = stratum_conn_new_for_test(s);
    char *out = NULL; size_t olen = 0;
    stratum_handle_message(s, c, "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}",
                           &out, &olen); free(out); out=NULL; olen=0;
    stratum_handle_message(s, c,
        "{\"id\":2,\"method\":\"mining.authorize\","
         "\"params\":[\"" TEST_ADDR "\",\"x\"]}",
        &out, &olen); free(out); out=NULL; olen=0;

    /* Precondition, and it has to hold for the rejections below to mean
     * anything: the ADVERTISED width is accepted. Sized off the constant, so
     * this stays a real precondition if the width changes again. */
    int rc = stratum_handle_message(s, c,
        "{\"id\":3,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J1\",\"" TEST_EN2 "\",\"60000000\",\"00000001\"]}",
        &out, &olen);
    CHECK(rc == 0);
    CHECK(obs.shares == 1);
    CHECK(obs.rejects == 0);
    free(out); out=NULL; olen=0;

    /* One byte too short (7 bytes). Deliberately just under the width
     * rather than wildly wrong: 7 is what the marketplaces floor at, so it is
     * the width a miscalibrated proxy is most likely to send. */
    rc = stratum_handle_message(s, c,
        "{\"id\":4,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J1\",\"aabbccddeeff00\",\"60000000\",\"00000002\"]}",
        &out, &olen);
    CHECK(rc == 0);
    CHECK(obs.shares == 1);                 /* not credited */
    CHECK(obs.rejects == 1);
    CHECK(strstr(obs.last_reason, "extranonce2") != NULL);
    /* Match the error PAYLOAD, not the "error" key — the key is present on a
     * successful response too, so the old check passed either way. Stratum
     * errors are the array form [code, message, null], not an object. */
    CHECK(strstr(out, "\"error\":[") != NULL);
    CHECK(strstr(out, "\"result\":true") == NULL);
    free(out); out=NULL; olen=0;

    /* One byte too long (9 bytes). */
    rc = stratum_handle_message(s, c,
        "{\"id\":5,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J1\",\"aabbccddeeff001122\",\"60000000\",\"00000003\"]}",
        &out, &olen);
    CHECK(rc == 0);
    CHECK(obs.shares == 1);
    CHECK(obs.rejects == 2);
    CHECK(strstr(obs.last_reason, "extranonce2") != NULL);
    free(out); out=NULL; olen=0;

    /* Odd-length hex is still caught by the decoder, before the size check. */
    rc = stratum_handle_message(s, c,
        "{\"id\":6,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J1\",\"aabbc\",\"60000000\",\"00000004\"]}",
        &out, &olen);
    CHECK(rc == 0);
    CHECK(obs.shares == 1);
    free(out);

    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

/* ---- concurrency: tip watcher vs. submitting connections ----------------
 *
 * The direct proof for the two use-after-frees this suite could not previously
 * reach. One thread rotates the job (as the tip watcher does), which walks
 * every registered connection and re-renders its coinbase — freeing cb1/cb2 —
 * and retires the old job, which frees it once the ring wraps. Meanwhile N
 * threads submit shares, each of which borrows a job pointer from find_job()
 * and reads cb1/cb2 to assemble a coinbase.
 *
 * Before the fixes this reliably tripped ASan (heap-use-after-free in
 * handle_submit's memcpy, or inside assemble_block_hex) and TSan (data race on
 * cb1/cb1_len and on the vardiff fields). It is a stress test, not a
 * deterministic one: run it under -fsanitize=address or =thread for it to mean
 * anything. Without a sanitizer it still exercises the paths and must not
 * crash. */
#define RACE_CONNS   4
#define RACE_SUBMITS 400
#define RACE_JOBS    300

typedef struct {
    stratum_server_t *s;
    stratum_conn_t   *c;
    int               id;
} race_arg_t;

static atomic_int race_stop;

static void *race_submit_thread(void *arg) {
    race_arg_t *a = (race_arg_t *)arg;
    char msg[256];
    for (int i = 0; i < RACE_SUBMITS && !atomic_load(&race_stop); ++i) {
        char *out = NULL; size_t olen = 0;
        /* Alternate between the job most likely to be current and an older
         * one, so both the current_job and recent-ring paths of find_job()
         * get exercised against a concurrent retire. */
        snprintf(msg, sizeof msg,
                 "{\"id\":9,\"method\":\"mining.submit\","
                 "\"params\":[\"w\",\"J%d\",\"%08x\",\"60000000\",\"%08x\"]}",
                 (i % 2) ? (i % RACE_JOBS) : ((i + 3) % RACE_JOBS),
                 (unsigned)(a->id * 1000 + i), (unsigned)i);
        stratum_handle_message(a->s, a->c, msg, &out, &olen);
        free(out);
    }
    return NULL;
}

/* Drain the miner side of each socketpair. Without a reader the kernel buffer
 * fills, the broadcast's write blocks, and the whole test wedges — which is
 * precisely the head-of-line stall SO_SNDTIMEO now bounds in production. Here
 * we want the race, not the stall, so we act like a miner that reads. */
static void *race_drain_thread(void *arg) {
    int fd = *(int *)arg;
    char sink[4096];
    while (!atomic_load(&race_stop)) {
        ssize_t n = recv(fd, sink, sizeof sink, MSG_DONTWAIT);
        if (n > 0) continue;
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 200000L };
        nanosleep(&ts, NULL);
    }
    return NULL;
}

static void *race_job_thread(void *arg) {
    stratum_server_t *s = (stratum_server_t *)arg;
    uint8_t net[32];
    memset(net, 0xff, sizeof net);   /* everything is a "block" -> assembles */
    for (int i = 0; i < RACE_JOBS && !atomic_load(&race_stop); ++i) {
        char jid[16];
        snprintf(jid, sizeof jid, "J%d", i);
        stratum_job_t *j = make_test_job(jid, net);
        if (!j) break;
        stratum_server_set_job(s, j);
    }
    return NULL;
}

static void test_job_rotation_races_submits(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = RACE_CONNS,
                           .initial_diff = 1.0,
                           .vardiff_enabled = 1, .vardiff_window_sec = 1,
                           .vardiff_target_spm = 60, .vardiff_min = 0.001,
                           .vardiff_max = 1e6,
                           .ctx = &obs, .on_share = on_share,
                           .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);
    atomic_store(&race_stop, 0);

    uint8_t net[32];
    memset(net, 0xff, sizeof net);
    stratum_server_set_job(s, make_test_job("J0", net));

    /* Each connection needs a real fd and a place in the broadcast list, or
     * set_job skips it and the race under test never happens. socketpair
     * gives us a writable fd that nothing has to read. */
    stratum_conn_t *conns[RACE_CONNS];
    int             fds[RACE_CONNS][2];
    race_arg_t      args[RACE_CONNS];
    pthread_t       subs[RACE_CONNS], jobthr;

    for (int i = 0; i < RACE_CONNS; ++i) {
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds[i]) == 0);
        conns[i] = stratum_conn_new_for_test(s);
        char *out = NULL; size_t olen = 0;
        stratum_handle_message(s, conns[i],
            "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}",
            &out, &olen); free(out); out = NULL; olen = 0;
        stratum_handle_message(s, conns[i],
            "{\"id\":2,\"method\":\"mining.authorize\","
             "\"params\":[\"" TEST_ADDR "\",\"x\"]}",
            &out, &olen); free(out);
        /* Precondition: without an authorized, subscribed conn the broadcast
         * loop skips it and this test would prove nothing. */
        CHECK(stratum_conn_authorized_for_test(conns[i]) == 1);
        CHECK(stratum_conn_subscribed_for_test(conns[i]) == 1);
        stratum_conn_register_for_test(s, conns[i], fds[i][0]);
        args[i].s = s; args[i].c = conns[i]; args[i].id = i;
    }

    pthread_t drains[RACE_CONNS];
    int       peer[RACE_CONNS];
    for (int i = 0; i < RACE_CONNS; ++i) {
        peer[i] = fds[i][1];
        pthread_create(&drains[i], NULL, race_drain_thread, &peer[i]);
    }

    pthread_create(&jobthr, NULL, race_job_thread, s);
    for (int i = 0; i < RACE_CONNS; ++i)
        pthread_create(&subs[i], NULL, race_submit_thread, &args[i]);

    for (int i = 0; i < RACE_CONNS; ++i) pthread_join(subs[i], NULL);
    atomic_store(&race_stop, 1);
    pthread_join(jobthr, NULL);
    for (int i = 0; i < RACE_CONNS; ++i) pthread_join(drains[i], NULL);

    /* Some work has to have landed, or the threads raced past each other
     * without ever meeting and the test is vacuous. */
    CHECK(obs.shares + obs.rejects > 0);

    for (int i = 0; i < RACE_CONNS; ++i) {
        close(fds[i][0]);
        close(fds[i][1]);
    }
    stratum_server_free(s);   /* frees the registered conns' jobs; conns below */
    for (int i = 0; i < RACE_CONNS; ++i) stratum_conn_free_for_test(conns[i]);
}

/* Invalid Bitcoin address as the username must be rejected outright. */
static void test_authorize_rejects_non_address(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1, .initial_diff = 1.0,
                           .ctx = &obs, .on_reject = on_reject };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);
    stratum_conn_t *c = stratum_conn_new_for_test(s);
    char *out = NULL; size_t olen = 0;
    stratum_handle_message(s, c,
        "{\"id\":1,\"method\":\"mining.authorize\","
         "\"params\":[\"alice.w1\",\"x\"]}",
        &out, &olen);
    CHECK(!stratum_conn_authorized_for_test(c));
    CHECK(obs.rejects == 1);
    /* JSON-RPC response carries an error array. */
    CHECK(strstr(out, "\"error\"") != NULL);
    free(out);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

/* Valid address with a funky label: address is preserved verbatim,
 * worker_name contains the full username (sanitized chars allowed
 * already), payout_address is exactly the address portion. */
static void test_authorize_address_with_label(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1, .initial_diff = 1.0,
                           .ctx = &obs };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);
    stratum_conn_t *c = stratum_conn_new_for_test(s);
    char *out = NULL; size_t olen = 0;
    stratum_handle_message(s, c,
        "{\"id\":1,\"method\":\"mining.authorize\","
         "\"params\":[\"" TEST_ADDR ".rig-007\",\"x\"]}",
        &out, &olen);
    CHECK(stratum_conn_authorized_for_test(c));
    CHECK(strcmp(stratum_conn_payout_address_for_test(c), TEST_ADDR) == 0);
    CHECK(strcmp(stratum_conn_worker_name_for_test(c),
                 TEST_ADDR ".rig-007") == 0);
    free(out);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

/* A hash that beats the network target but not the share target must be
 * accepted and submitted as a block, never rejected as low difficulty.
 * Regression: worker diff 1e12 makes the worker target ~0 so every hash
 * fails the share check, while an all-ff network target makes every hash
 * a block. The job is set after authorize so the authorize-time clamp
 * (no job yet) leaves the huge difficulty in place. */
static void test_block_wins_over_low_difficulty(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1,
                           .initial_diff = 1e12,
                           .ctx = &obs, .on_share = on_share,
                           .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);

    stratum_conn_t *c = stratum_conn_new_for_test(s);
    char *out = NULL; size_t olen = 0;
    stratum_handle_message(s, c, "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}",
                           &out, &olen); free(out); out=NULL; olen=0;
    stratum_handle_message(s, c,
        "{\"id\":2,\"method\":\"mining.authorize\","
         "\"params\":[\"" TEST_ADDR "\",\"x\"]}",
        &out, &olen); free(out); out=NULL; olen=0;

    uint8_t net[32]; memset(net, 0xff, 32);
    stratum_server_set_job(s, make_test_job("J1", net));

    int rc = stratum_handle_message(s, c,
        "{\"id\":3,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J1\",\"" TEST_EN2 "\",\"60000000\",\"00000001\"]}",
        &out, &olen);
    CHECK(rc == 0);
    CHECK(obs.rejects == 0);
    CHECK(obs.shares == 1);
    CHECK(obs.blocks == 1);
    CHECK(obs.last_is_block == 1);
    free(out);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

/* Vardiff must not raise the share difficulty past the network difficulty.
 * vardiff_min = 1e12 would floor the retarget at 1e12, but the job's
 * network target is DIFF1 (difficulty 1), so the emitted set_difficulty
 * must be clamped to 1. */
static void test_vardiff_clamped_to_network_diff(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1,
                           .initial_diff = 1e-12,
                           .vardiff_enabled = 1,
                           .vardiff_target_spm = 0.001,
                           .vardiff_min = 1e12,
                           .vardiff_max = 1e15,
                           .vardiff_window_sec = 1,
                           .ctx = &obs, .on_share = on_share,
                           .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);

    /* DIFF1 target = difficulty 1.0. */
    uint8_t net[32] = {0};
    net[4] = 0xff; net[5] = 0xff;
    stratum_server_set_job(s, make_test_job("J1", net));

    stratum_conn_t *c = stratum_conn_new_for_test(s);
    char *out = NULL; size_t olen = 0;
    stratum_handle_message(s, c, "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}",
                           &out, &olen); free(out); out=NULL; olen=0;
    stratum_handle_message(s, c,
        "{\"id\":2,\"method\":\"mining.authorize\","
         "\"params\":[\"" TEST_ADDR "\",\"x\"]}",
        &out, &olen); free(out); out=NULL; olen=0;

    /* Let the vardiff window elapse, then submit: the observed rate blows
     * past target_spm, the retarget floors at vardiff_min, and the network
     * clamp must pull it back down to 1. */
    sleep_ms(1100);
    stratum_handle_message(s, c,
        "{\"id\":3,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J1\",\"" TEST_EN2 "\",\"60000000\",\"00000001\"]}",
        &out, &olen);
    CHECK(out != NULL);
    CHECK(strstr(out, "mining.set_difficulty") != NULL);
    CHECK(strstr(out, "\"params\":[1]") != NULL);
    free(out);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

/* After a retarget raises the difficulty, shares mined against the old
 * difficulty must stay acceptable for the grace period (the miner only
 * applies set_difficulty on a later job). */
static void test_vardiff_grace_accepts_old_diff_shares(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1,
                           .initial_diff = 1e-12,
                           .vardiff_enabled = 1,
                           .vardiff_target_spm = 0.001,
                           .vardiff_min = 1e12,
                           .vardiff_max = 1e15,
                           .vardiff_window_sec = 1,
                           .ctx = &obs, .on_share = on_share,
                           .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);

    /* All-zero network target: nothing is ever a block, so acceptance can
     * only come from the share-difficulty path. */
    uint8_t net[32] = {0};
    stratum_server_set_job(s, make_test_job("J1", net));

    stratum_conn_t *c = stratum_conn_new_for_test(s);
    char *out = NULL; size_t olen = 0;
    stratum_handle_message(s, c, "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}",
                           &out, &olen); free(out); out=NULL; olen=0;
    stratum_handle_message(s, c,
        "{\"id\":2,\"method\":\"mining.authorize\","
         "\"params\":[\"" TEST_ADDR "\",\"x\"]}",
        &out, &olen); free(out); out=NULL; olen=0;

    /* Share #1 after the window elapses triggers a retarget to 1e12
     * (no network clamp: the all-zero target has infinite difficulty). */
    sleep_ms(1100);
    stratum_handle_message(s, c,
        "{\"id\":3,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J1\",\"" TEST_EN2 "\",\"60000000\",\"00000001\"]}",
        &out, &olen);
    CHECK(strstr(out, "mining.set_difficulty") != NULL);
    CHECK(obs.shares == 1);
    free(out); out=NULL; olen=0;

    /* Share #2 fails the new 1e12 target but met the old 1e-12 one — the
     * grace window must accept it. */
    stratum_handle_message(s, c,
        "{\"id\":4,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J1\",\"" TEST_EN2 "\",\"60000000\",\"00000002\"]}",
        &out, &olen);
    CHECK(obs.shares == 2);
    CHECK(obs.rejects == 0);
    free(out);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

/* Idle-socket reaper: verify the accepted-socket setup path applies
 * SO_RCVTIMEO derived from idle_timeout_sec. We can't cheaply test the
 * "silent client gets dropped" path in a unit test — that would need a
 * real 3s+ sleep — but confirming SO_RCVTIMEO is present is enough to
 * guarantee the recv loop wakes and gets to check last_activity_ms. */
static void test_socket_setup_applies_rcvtimeo(void) {
    int sp[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0);
    /* idle_timeout=45 → poll interval clamped to 30s (SO_RCVTIMEO ceiling). */
    CHECK(stratum_socket_setup_for_test(sp[0], 45) == 0);
    struct timeval tv = {0};
    socklen_t len = sizeof tv;
    CHECK(getsockopt(sp[0], SOL_SOCKET, SO_RCVTIMEO, &tv, &len) == 0);
    CHECK(tv.tv_sec == 30);
    /* idle_timeout=5 → poll interval matches (below the 30s clamp). */
    CHECK(stratum_socket_setup_for_test(sp[1], 5) == 0);
    struct timeval tv2 = {0};
    socklen_t len2 = sizeof tv2;
    CHECK(getsockopt(sp[1], SOL_SOCKET, SO_RCVTIMEO, &tv2, &len2) == 0);
    CHECK(tv2.tv_sec == 5);
    close(sp[0]);
    close(sp[1]);
}

/* idle_timeout_sec=0 leaves SO_RCVTIMEO unset — legacy blocking recv. */
static void test_socket_setup_disabled(void) {
    int sp[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0);
    CHECK(stratum_socket_setup_for_test(sp[0], 0) == 0);
    struct timeval tv = {0};
    socklen_t len = sizeof tv;
    CHECK(getsockopt(sp[0], SOL_SOCKET, SO_RCVTIMEO, &tv, &len) == 0);
    CHECK(tv.tv_sec == 0 && tv.tv_usec == 0);
    close(sp[0]);
    close(sp[1]);
}

/* Every connection must get a distinct extranonce1. Two connections sharing
 * one render identical coinbases, so they mine the same header and submit the
 * same hash — half the hashrate is wasted and PPS credits the share twice.
 *
 * The old allocator was `seq ^ now_ms()`, which collides whenever the delta in
 * the clock equals the delta in the counter: an even seq at an even
 * millisecond and the next seq a millisecond later produce the same value.
 * Subscribing in a tight loop, as a miner opening several connections at once
 * does, reproduces it. */
static void test_extranonce1_unique_across_connections(void) {
    enum { N = 64 };
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = N, .initial_diff = 1.0 };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    CHECK(stratum_server_start(&cfg, &s) == 0);
    if (!s) return;

    char seen[N][9];
    stratum_conn_t *conns[N];
    int collected = 0;

    for (int i = 0; i < N; ++i) {
        conns[i] = stratum_conn_new_for_test(s);
        char *out = NULL; size_t olen = 0;
        stratum_handle_message(s, conns[i],
            "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}",
            &out, &olen);
        if (out) {
            cJSON *resp = parse_first_line(out);
            if (resp) {
                cJSON *ex1 = cJSON_GetArrayItem(
                    cJSON_GetObjectItem(resp, "result"), 1);
                if (cJSON_IsString(ex1)) {
                    snprintf(seen[collected], sizeof(seen[0]), "%s",
                             ex1->valuestring);
                    collected++;
                }
                cJSON_Delete(resp);
            }
            free(out);
        }
        /* Straddle millisecond boundaries so the old clock-XOR allocator is
         * actually given the chance to collide. */
        if (i % 8 == 0) sleep_ms(1);
    }
    CHECK(collected == N);

    int dupes = 0;
    for (int i = 0; i < collected; ++i)
        for (int j = i + 1; j < collected; ++j)
            if (strcmp(seen[i], seen[j]) == 0) dupes++;
    CHECK(dupes == 0);

    for (int i = 0; i < N; ++i) stratum_conn_free_for_test(conns[i]);
    stratum_server_free(s);
}

/* stratum_server_stop() must not return while a connection thread is alive.
 *
 * Connection threads are detached and park in recv() for a whole SO_RCVTIMEO,
 * so without the drain in stratum_server_stop they outlive the server: main.c
 * goes straight on to free the server, close the store and destroy the server
 * context, while a thread is still able to unregister itself through a
 * destroyed conns_lock and hand a share to a closed store. Shutting each
 * socket down wakes those threads, and conn_count reaching zero is the proof
 * they are finished. */
static void test_stop_waits_for_connection_threads(void) {
    enum { PORT = 39337 };
    stratum_cfg_t cfg = { .bind_port = PORT, .max_conns = 8,
                          .initial_diff = 1.0, .idle_timeout_sec = 30 };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    if (stratum_server_start(&cfg, &s) != 0 || !s) {
        /* Fixed port busy on this machine: nothing to assert, and failing the
         * run for someone else's listener would be noise. */
        printf("skip: stop drains connection threads (port %d unavailable)\n", PORT);
        return;
    }

    int cfd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(cfd >= 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(PORT);
    CHECK(inet_pton(AF_INET, "127.0.0.1", &a.sin_addr) == 1);
    CHECK(connect(cfd, (struct sockaddr *)&a, sizeof a) == 0);

    /* Wait for accept() so a connection thread really is running. */
    for (int i = 0; i < 500 && stratum_server_conn_count_for_test(s) == 0; ++i)
        sleep_ms(2);
    CHECK(stratum_server_conn_count_for_test(s) == 1);

    /* The client sends nothing, so that thread is parked in recv() with 30s of
     * timeout ahead of it. stop() has to wake it and wait, not walk away. */
    stratum_server_stop(s);
    CHECK(stratum_server_conn_count_for_test(s) == 0);

    close(cfd);
    stratum_server_free(s);
    printf("ok: stop drains connection threads before returning\n");
}

/* The per-connection ring keys on (job_id|en2|ntime|nonce|version), so the
 * same solution resubmitted under a *different* job id slips past it. When
 * both jobs carry the same template the header — and therefore the hash — is
 * identical, and it must still be credited only once. The server-wide ring
 * keys on the hash itself, which is what makes this hold. */
static void test_dedupe_same_hash_across_job_ids(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1,
                          .initial_diff = 1e-12,
                          .ctx = &obs, .on_share = on_share,
                          .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);

    uint8_t net[32] = {0};
    stratum_server_set_job(s, make_test_job("J1", net));

    stratum_conn_t *c = stratum_conn_new_for_test(s);
    char *out = NULL; size_t olen = 0;
    stratum_handle_message(s, c, "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}",
                           &out, &olen); free(out); out=NULL; olen=0;
    stratum_handle_message(s, c,
        "{\"id\":2,\"method\":\"mining.authorize\","
         "\"params\":[\"" TEST_ADDR "\",\"x\"]}",
        &out, &olen); free(out); out=NULL; olen=0;

    stratum_handle_message(s, c,
        "{\"id\":3,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J1\",\"" TEST_EN2 "\",\"60000000\",\"00000001\"]}",
        &out, &olen);
    CHECK(obs.shares == 1);
    free(out); out=NULL; olen=0;

    /* Same template, new id. Identical header -> identical hash. */
    stratum_server_set_job(s, make_test_job("J2", net));
    stratum_handle_message(s, c,
        "{\"id\":4,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J2\",\"" TEST_EN2 "\",\"60000000\",\"00000001\"]}",
        &out, &olen);
    CHECK(obs.shares == 1);  /* still one */
    CHECK(obs.rejects >= 1);
    CHECK(strstr(obs.last_reason, "duplicate") != NULL);
    free(out);

    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}


/* A worker with history must be handed that difficulty at authorize, not
 * initial_diff. Regression: the first version of this gated the lookup on
 * `c->difficulty <= 0`, but the connection constructor has already assigned
 * initial_diff, so the hook was never called and the fix was inert on the
 * live pool while looking correct in the source. */
static double hint_cb(void *ctx, const char *worker) {
    (void)worker;
    int *called = (int *)ctx;
    (*called)++;
    return 4096.0;
}
static double hint_none_cb(void *ctx, const char *worker) {
    (void)ctx; (void)worker; return 0.0;
}

static void test_authorize_resumes_difficulty_from_hint(void) {
    int called = 0;
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1, .initial_diff = 1.0,
                          .ctx = &called, .on_difficulty_hint = hint_cb };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    CHECK(stratum_server_start(&cfg, &s) == 0);
    if (!s) return;
    /* No job yet: the authorize-time network clamp has nothing to clamp
     * against, so the seeded difficulty survives. Same trick as
     * test_block_wins_over_low_difficulty. */
    stratum_conn_t *c = stratum_conn_new_for_test(s);
    char *out = NULL; size_t olen = 0;
    stratum_handle_message(s, c, "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}",
                           &out, &olen); free(out); out=NULL; olen=0;
    stratum_handle_message(s, c,
        "{\"id\":2,\"method\":\"mining.authorize\",\"params\":[\"" TEST_ADDR "\",\"x\"]}",
        &out, &olen);
    CHECK(called == 1);
    CHECK(out != NULL);
    /* The miner must be told 4096, not the initial_diff of 1. */
    CHECK(out && strstr(out, "\"params\":[4096]") != NULL);
    CHECK(out && strstr(out, "\"params\":[1]") == NULL);
    free(out);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
    printf("ok: authorize resumes difficulty from history\n");
}

/* No history: initial_diff still applies. */
static void test_authorize_without_hint_uses_initial(void) {
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1, .initial_diff = 7.0,
                          .on_difficulty_hint = hint_none_cb };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    CHECK(stratum_server_start(&cfg, &s) == 0);
    if (!s) return;
    stratum_conn_t *c = stratum_conn_new_for_test(s);
    char *out = NULL; size_t olen = 0;
    stratum_handle_message(s, c, "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}",
                           &out, &olen); free(out); out=NULL; olen=0;
    stratum_handle_message(s, c,
        "{\"id\":2,\"method\":\"mining.authorize\",\"params\":[\"" TEST_ADDR "\",\"x\"]}",
        &out, &olen);
    CHECK(out && strstr(out, "\"params\":[7]") != NULL);
    free(out);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
    printf("ok: no history falls back to initial_diff\n");
}

/* ---------- pool_mode=proportional (P.3) ---------- */

/* The enforcer-shaped template from test_coinbase.c: segwit, one 50 BTC
 * spendable output between two OP_RETURN commitments. */
static const char *PROP_COINBASE_HEX =
    "02000000"
    "0001"
    "01"
    "0000000000000000000000000000000000000000000000000000000000000000"
    "ffffffff"
    "04" "0300350c"
    "ffffffff"
    "03"
    "0000000000000000" "06" "6a04deadbeef"
    "00f2052a01000000" "16" "0014" "1111111111111111111111111111111111111111"
    "0000000000000000" "26" "6a24aa21a9ed"
        "2222222222222222222222222222222222222222222222222222222222222222"
    "0120" "0000000000000000000000000000000000000000000000000000000000000000"
    "00000000";

/* Two real P2WPKH regtest addresses, hash160 = 0x33*20 and 0x44*20. Chosen so
 * neither their scripts nor the template's own recipient (0x11*20) nor its
 * witness commitment (0x22*32) can be confused for one another in the
 * serialized coinbase. */
#define PROP_ADDR_A "bcrt1qxvenxvenxvenxvenxvenxvenxvenxvenztev8a"
#define PROP_ADDR_B "bcrt1qg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zyay3npn"

static stratum_job_t *make_prop_job(const char *job_id,
                                    const uint8_t *network_target_be) {
    uint8_t prev[32] = {0};
    return stratum_job_new(job_id, 1, prev,
                           /*value_sats*/ 5000000000LL,
                           /*wc_hex*/ NULL,
                           /*en1*/ STRATUM_EXTRANONCE1_SIZE,
                           /*en2*/ STRATUM_EXTRANONCE2_SIZE,
                           NULL, 0, 0x1d00ffffu, 0x60000000u,
                           network_target_be, 800000, NULL, 0,
                           PROP_COINBASE_HEX,
                           /*coinbase_has_witness*/ 1);
}

/* Pull the coinbase1/coinbase2 halves out of a mining.notify line. */
static int notify_coinbase(const char *out, char *cb1, size_t cb1n,
                           char *cb2, size_t cb2n) {
    const char *p = strstr(out, "mining.notify");
    if (!p) return -1;
    cJSON *root = NULL;
    /* Each notification is one JSON line; find the one carrying notify. */
    const char *line = out;
    while (line && *line) {
        const char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line) : strlen(line);
        char *buf = (char *)malloc(len + 1);
        if (!buf) return -1;
        memcpy(buf, line, len); buf[len] = '\0';
        if (strstr(buf, "mining.notify")) {
            root = cJSON_Parse(buf);
            free(buf);
            break;
        }
        free(buf);
        line = nl ? nl + 1 : NULL;
    }
    if (!root) return -1;
    cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
    /* params: [job_id, prevhash, coinb1, coinb2, merkle, ver, nbits, ntime, clean] */
    cJSON *c1 = cJSON_GetArrayItem(params, 2);
    cJSON *c2 = cJSON_GetArrayItem(params, 3);
    int ok = -1;
    if (cJSON_IsString(c1) && cJSON_IsString(c2)) {
        snprintf(cb1, cb1n, "%s", c1->valuestring);
        snprintf(cb2, cb2n, "%s", c2->valuestring);
        ok = 0;
    }
    cJSON_Delete(root);
    return ok;
}

/* Extract extranonce1 from a mining.subscribe reply. */
static int subscribe_extranonce1(const char *out, char *en1, size_t n) {
    cJSON *root = cJSON_Parse(out);
    if (!root) return -1;
    cJSON *res = cJSON_GetObjectItemCaseSensitive(root, "result");
    cJSON *e   = cJSON_GetArrayItem(res, 1);
    int ok = -1;
    if (cJSON_IsString(e)) { snprintf(en1, n, "%s", e->valuestring); ok = 0; }
    cJSON_Delete(root);
    return ok;
}

/* The P.3 gate: in proportional mode two miners must be handed the SAME
 * coinbase — paying the window's shareholders, not either miner — while their
 * extranonce1 values still differ, and both must be able to submit an accepted
 * share against it. */
static void test_proportional_shared_coinbase(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 2, .initial_diff = 1.0,
                          .ctx = &obs, .on_share = on_share,
                          .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    snprintf(cfg.pool_mode, sizeof(cfg.pool_mode), "proportional");
    stratum_server_t *s = NULL;
    CHECK(stratum_server_start(&cfg, &s) == 0);
    if (!s) return;

    /* A window split 60/40 between two addresses, summing to the full reward
     * so the builder's conservation check passes. */
    coinbase_payout_t payouts[2] = {
        { PROP_ADDR_A, 3000000000LL },
        { PROP_ADDR_B, 2000000000LL },
    };
    uint8_t net[32]; memset(net, 0xff, 32);
    stratum_job_t *job = make_prop_job("P1", net);
    CHECK(job != NULL);
    CHECK(stratum_job_set_payouts(job, payouts, 2) == 0);
    stratum_server_set_job(s, job);

    char cb1[2][4096] = {{0}}, cb2[2][4096] = {{0}}, en1[2][64] = {{0}};
    stratum_conn_t *c[2];
    for (int i = 0; i < 2; i++) {
        c[i] = stratum_conn_new_for_test(s);
        char *out = NULL; size_t olen = 0;
        stratum_handle_message(s, c[i],
            "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}", &out, &olen);
        CHECK(out && subscribe_extranonce1(out, en1[i], sizeof en1[i]) == 0);
        free(out); out = NULL; olen = 0;

        /* Each miner authorizes with its OWN address — which proportional mode
         * must then ignore when building the coinbase. */
        char auth[256];
        snprintf(auth, sizeof auth,
            "{\"id\":2,\"method\":\"mining.authorize\",\"params\":[\"%s\",\"x\"]}",
            i == 0 ? PROP_ADDR_A : PROP_ADDR_B);
        stratum_handle_message(s, c[i], auth, &out, &olen);
        CHECK(out != NULL);
        CHECK(notify_coinbase(out, cb1[i], sizeof cb1[i],
                              cb2[i], sizeof cb2[i]) == 0);
        free(out);
    }

    /* Same coinbase for both miners. */
    CHECK(cb1[0][0] != '\0' && cb2[0][0] != '\0');
    CHECK(strcmp(cb1[0], cb1[1]) == 0);
    CHECK(strcmp(cb2[0], cb2[1]) == 0);
    /* Distinct extranonce1, so their search spaces do not collide. */
    CHECK(strcmp(en1[0], en1[1]) != 0);
    /* Both payout scripts are in the shared coinbase, and the template's own
     * recipient (0x11*20) is gone — the reward output really was replaced. */
    CHECK(strstr(cb2[0], "3333333333333333333333333333333333333333") != NULL);
    CHECK(strstr(cb2[0], "4444444444444444444444444444444444444444") != NULL);
    CHECK(strstr(cb2[0], "1111111111111111111111111111111111111111") == NULL);

    /* Both miners submit, both accepted. */
    for (int i = 0; i < 2; i++) {
        char *out = NULL; size_t olen = 0;
        char sub[256];
        snprintf(sub, sizeof sub,
            "{\"id\":3,\"method\":\"mining.submit\","
            "\"params\":[\"w%d\",\"P1\",\"" TEST_EN2 "\",\"60000000\",\"0000000%d\"]}",
            i, i + 1);
        CHECK(stratum_handle_message(s, c[i], sub, &out, &olen) == 0);
        free(out);
    }
    CHECK(obs.shares == 2);
    CHECK(obs.rejects == 0);

    for (int i = 0; i < 2; i++) stratum_conn_free_for_test(c[i]);
    stratum_server_free(s);
    printf("ok: proportional shares one coinbase across miners\n");
}

/* With no payout set attached — no PPLNS window yet — proportional mode must
 * fall back to paying the connection's own address rather than dropping the
 * job. Each miner then gets a DIFFERENT coinbase, as in solo. */
static void test_proportional_falls_back_without_window(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 2, .initial_diff = 1.0,
                          .ctx = &obs, .on_share = on_share,
                          .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    snprintf(cfg.pool_mode, sizeof(cfg.pool_mode), "proportional");
    stratum_server_t *s = NULL;
    CHECK(stratum_server_start(&cfg, &s) == 0);
    if (!s) return;

    /* No stratum_job_set_payouts call at all. */
    uint8_t net[32]; memset(net, 0xff, 32);
    stratum_server_set_job(s, make_prop_job("P2", net));

    char cb2[2][4096] = {{0}}, cb1[2][4096] = {{0}};
    stratum_conn_t *c[2];
    for (int i = 0; i < 2; i++) {
        c[i] = stratum_conn_new_for_test(s);
        char *out = NULL; size_t olen = 0;
        stratum_handle_message(s, c[i],
            "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}", &out, &olen);
        free(out); out = NULL; olen = 0;
        char auth[256];
        snprintf(auth, sizeof auth,
            "{\"id\":2,\"method\":\"mining.authorize\",\"params\":[\"%s\",\"x\"]}",
            i == 0 ? PROP_ADDR_A : PROP_ADDR_B);
        stratum_handle_message(s, c[i], auth, &out, &olen);
        CHECK(out != NULL);
        CHECK(notify_coinbase(out, cb1[i], sizeof cb1[i],
                              cb2[i], sizeof cb2[i]) == 0);
        free(out);
    }
    /* Jobs still went out, and each pays its own miner. */
    CHECK(cb2[0][0] != '\0' && cb2[1][0] != '\0');
    CHECK(strcmp(cb2[0], cb2[1]) != 0);
    CHECK(strstr(cb2[0], "3333333333333333333333333333333333333333") != NULL);
    CHECK(strstr(cb2[0], "4444444444444444444444444444444444444444") == NULL);
    CHECK(strstr(cb2[1], "4444444444444444444444444444444444444444") != NULL);

    for (int i = 0; i < 2; i++) stratum_conn_free_for_test(c[i]);
    stratum_server_free(s);
    printf("ok: proportional falls back to per-miner without a window\n");
}

/* Helper: subscribe one fresh connection and return its extranonce1 as an
 * unsigned int. Returns 0 on any failure, which no real allocation produces
 * often enough to mask a bug (the counter is clock-seeded). */
static unsigned subscribe_get_en1(stratum_server_t *s, stratum_conn_t **out_c) {
    stratum_conn_t *c = stratum_conn_new_for_test(s);
    *out_c = c;
    char *out = NULL; size_t olen = 0;
    stratum_handle_message(s, c,
        "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}", &out, &olen);
    unsigned v = 0;
    if (out) {
        cJSON *resp = parse_first_line(out);
        if (resp) {
            cJSON *ex1 = cJSON_GetArrayItem(cJSON_GetObjectItem(resp, "result"), 1);
            if (cJSON_IsString(ex1)) v = (unsigned)strtoul(ex1->valuestring, NULL, 16);
            cJSON_Delete(resp);
        }
        free(out);
    }
    return v;
}

/* Two servers sharing a stratum_shared_t must draw extranonce1 from ONE
 * sequence.
 *
 * This is the invariant the rental port rests on. Two connections handed the
 * same extranonce1 render identical coinbases, mine identical headers, and
 * find the same hash from the same nonce — the share is credited twice and
 * half the hashrate is wasted. Each server left to allocate its own counter
 * seeds it from the clock at startup, so two started in the same process seed
 * within a millisecond of each other and their sequences overlap almost
 * entirely.
 *
 * Asserting only "the values differ" would pass by luck whenever the two
 * clock seeds happened to differ, so this asserts the stronger and fully
 * deterministic property: consecutive subscribes across the two servers are
 * CONSECUTIVE, which can only hold if one counter is feeding both. */
static void test_shared_extranonce1_sequence_across_servers(void) {
    stratum_shared_t *shared = stratum_shared_new();
    CHECK(shared != NULL);
    if (!shared) return;

    stratum_cfg_t cfg_a = { .bind_port = 0, .max_conns = 4, .initial_diff = 1.0,
                            .shared = shared };
    stratum_cfg_t cfg_b = { .bind_port = 0, .max_conns = 4, .initial_diff = 1.0,
                            .shared = shared };
    snprintf(cfg_a.bind_addr, sizeof(cfg_a.bind_addr), "127.0.0.1");
    snprintf(cfg_b.bind_addr, sizeof(cfg_b.bind_addr), "127.0.0.1");

    stratum_server_t *a = NULL, *b = NULL;
    CHECK(stratum_server_start(&cfg_a, &a) == 0);
    CHECK(stratum_server_start(&cfg_b, &b) == 0);
    if (!a || !b) { stratum_shared_free(shared); return; }

    stratum_conn_t *c1, *c2, *c3, *c4;
    unsigned e1 = subscribe_get_en1(a, &c1);   /* public port  */
    unsigned e2 = subscribe_get_en1(b, &c2);   /* rental port  */
    unsigned e3 = subscribe_get_en1(a, &c3);
    unsigned e4 = subscribe_get_en1(b, &c4);

    /* One counter, so the four are strictly consecutive regardless of which
     * server each subscribe landed on. */
    CHECK(e2 == e1 + 1);
    CHECK(e3 == e1 + 2);
    CHECK(e4 == e1 + 3);

    /* The property all of that exists to guarantee. */
    CHECK(e1 != e2 && e1 != e3 && e1 != e4);
    CHECK(e2 != e3 && e2 != e4 && e3 != e4);

    stratum_conn_free_for_test(c1); stratum_conn_free_for_test(c2);
    stratum_conn_free_for_test(c3); stratum_conn_free_for_test(c4);
    stratum_server_free(a);
    stratum_server_free(b);
    /* Freeing the servers must not have freed state they only borrowed. */
    stratum_shared_free(shared);
    printf("ok: extranonce1 is one sequence across both servers\n");
}

/* A server given no shared state allocates its own, so the single-server
 * case and every other test keep working unchanged. */
static void test_server_without_shared_still_allocates_one(void) {
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 2, .initial_diff = 1.0 };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    CHECK(stratum_server_start(&cfg, &s) == 0);
    if (!s) return;
    stratum_conn_t *c1, *c2;
    unsigned e1 = subscribe_get_en1(s, &c1);
    unsigned e2 = subscribe_get_en1(s, &c2);
    CHECK(e2 == e1 + 1);
    stratum_conn_free_for_test(c1);
    stratum_conn_free_for_test(c2);
    stratum_server_free(s);   /* must free the counter it owns */
    printf("ok: a lone server allocates its own shared state\n");
}

static double hint_low(void *ctx, const char *worker) {
    (void)ctx; (void)worker; return 7.0;
}

/* The rental port's floor has to survive a returning worker.
 *
 * The difficulty hint replays whatever this worker name converged to before,
 * and it knows nothing about which port earned it. A miner that mined the
 * public port at difficulty 7 and then points a rented fleet at the rental
 * port would otherwise be seeded at 7 — under the marketplace minimum, which
 * fails the order for invalid shares rather than merely mis-sizing shares. */
static void test_hint_below_vardiff_min_is_floored(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1,
                           .initial_diff = 500000.0,
                           .vardiff_enabled = 1,
                           .vardiff_target_spm = 12,
                           .vardiff_min = 500000.0,
                           .vardiff_max = 1e12,
                           .vardiff_window_sec = 30,
                           .ctx = &obs, .on_share = on_share,
                           .on_difficulty_hint = hint_low,
                           .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    CHECK(stratum_server_start(&cfg, &s) == 0);
    if (!s) return;

    /* A HIGH-difficulty network target (~16.7M: the difficulty-1 target
     * shifted three bytes right). The network clamp deliberately wins over
     * the floor, so an easy target here would pull the difficulty back under
     * vardiff_min and the test would report the clamp's work as the floor's.
     * → feedback_tests-that-pass-for-the-wrong-reason */
    uint8_t net[32] = {0};
    net[7] = 0xff; net[8] = 0xff;
    stratum_server_set_job(s, make_test_job("J1", net));

    stratum_conn_t *c = stratum_conn_new_for_test(s);
    char *out = NULL; size_t olen = 0;
    stratum_handle_message(s, c, "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}",
                           &out, &olen); free(out); out=NULL; olen=0;
    stratum_handle_message(s, c,
        "{\"id\":2,\"method\":\"mining.authorize\","
         "\"params\":[\"" TEST_ADDR "\",\"x\"]}",
        &out, &olen);

    /* Floored to vardiff_min, not seeded at the hint's 7. */
    CHECK(out != NULL);
    CHECK(strstr(out, "\"params\":[500000]") != NULL);
    CHECK(strstr(out, "\"params\":[7]") == NULL);
    free(out);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
    printf("ok: a hint below vardiff_min is raised to the floor\n");
}

/* ...but the floor is deliberately NOT applied to initial_diff. A server
 * whose initial_diff sits below its vardiff_min is a valid configuration —
 * it starts easy and lets the first retarget lift it — and that is what
 * test_vardiff_grace_accepts_old_diff_shares relies on. Pinning this so the
 * hint floor above never quietly widens into initial_diff. */
static void test_initial_diff_below_vardiff_min_is_not_floored(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1,
                           .initial_diff = 1.0,
                           .vardiff_enabled = 1,
                           .vardiff_target_spm = 12,
                           .vardiff_min = 500000.0,
                           .vardiff_max = 1e12,
                           .vardiff_window_sec = 30,
                           .ctx = &obs, .on_share = on_share,
                           .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    CHECK(stratum_server_start(&cfg, &s) == 0);
    if (!s) return;
    /* Same high-difficulty target, same reason. */
    uint8_t net[32] = {0};
    net[7] = 0xff; net[8] = 0xff;
    stratum_server_set_job(s, make_test_job("J1", net));

    stratum_conn_t *c = stratum_conn_new_for_test(s);
    char *out = NULL; size_t olen = 0;
    stratum_handle_message(s, c, "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}",
                           &out, &olen); free(out); out=NULL; olen=0;
    stratum_handle_message(s, c,
        "{\"id\":2,\"method\":\"mining.authorize\","
         "\"params\":[\"" TEST_ADDR "\",\"x\"]}",
        &out, &olen);
    CHECK(out != NULL);
    CHECK(strstr(out, "\"params\":[1]") != NULL);
    free(out);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
    printf("ok: initial_diff below vardiff_min is left alone\n");
}

/* Build a server + one authorized connection sitting on job "J1", with the
 * connection's difficulty recorded against that job at `issued_diff`.
 *
 * All-zero network target throughout: nothing is ever a block, so acceptance
 * can only come from the share-difficulty path, and vardiff's
 * network-difficulty clamp cannot interfere.
 */
static stratum_server_t *setup_job_diff_conn(obs_t *obs, double issued_diff,
                                             stratum_conn_t **out_c) {
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1,
                           .initial_diff = issued_diff,
                           .vardiff_enabled = 1,
                           .vardiff_target_spm = 12,
                           .vardiff_min = 0.0,
                           .vardiff_max = 1e15,
                           .vardiff_window_sec = 30,
                           .ctx = obs, .on_share = on_share,
                           .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    if (stratum_server_start(&cfg, &s) != 0 || !s) return NULL;

    uint8_t net[32] = {0};
    stratum_server_set_job(s, make_test_job("J1", net));

    stratum_conn_t *c = stratum_conn_new_for_test(s);
    char *out = NULL; size_t olen = 0;
    stratum_handle_message(s, c, "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}",
                           &out, &olen); free(out); out=NULL; olen=0;
    /* authorize emits set_difficulty + notify, and the notify is what records
     * J1 against this connection's current difficulty. */
    stratum_handle_message(s, c,
        "{\"id\":2,\"method\":\"mining.authorize\","
         "\"params\":[\"" TEST_ADDR "\",\"x\"]}",
        &out, &olen); free(out);
    *out_c = c;
    return s;
}

/* THE marketplace defect: a submit naming an OLD job must be judged at the
 * difficulty that job was issued at, not at whatever the connection has
 * retargeted to since.
 *
 * The server keeps STRATUM_RECENT_JOBS jobs solvable — minutes of history —
 * while a vardiff window is seconds, so a miner can legitimately return work
 * for a job that predates several retargets. Judging it at the current
 * difficulty throws away work the miner performed exactly as instructed.
 * Marketplaces report this as the single most common way a pool integration
 * fails: it passes the extranonce check, then collapses the first time
 * difficulty moves.
 *
 * `prev` is set to another high value on purpose: that models a SECOND
 * retarget having overwritten the original difficulty, which is precisely the
 * case the one-deep prev_difficulty grace cannot cover. The grace window is
 * left wide open (diff_changed_ms = now), so a pass here is the per-job record
 * working and not the grace expiring. */
static void test_submit_judged_at_the_jobs_own_difficulty(void) {
    obs_t obs = {0};
    stratum_conn_t *c = NULL;
    stratum_server_t *s = setup_job_diff_conn(&obs, 1e-12, &c);
    CHECK(s != NULL);
    if (!s) return;

    /* Two retargets have happened since J1 went out: 1e-12 -> 1e6 -> 4e6. */
    stratum_conn_force_difficulty_for_test(c, 4e6, 1e6);

    char *out = NULL; size_t olen = 0;
    int rc = stratum_handle_message(s, c,
        "{\"id\":3,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J1\",\"" TEST_EN2 "\",\"60000000\",\"00000001\"]}",
        &out, &olen);
    CHECK(rc == 0);

    /* Accepted, because J1 was issued at 1e-12 and the hash clears it. */
    CHECK(obs.shares == 1);
    CHECK(obs.rejects == 0);
    /* And CREDITED at the job's difficulty — this is the PPLNS weight, so
     * crediting it at 4e6 would misprice the share as well as accept it. */
    CHECK(obs.last_difficulty == 1e-12);
    /* "error" is a KEY in every JSON-RPC response, success included, so match
     * the result rather than the key's presence. */
    CHECK(out != NULL && strstr(out, "\"result\":true") != NULL);
    free(out);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
    printf("ok: a submit is judged at its own job's difficulty\n");
}

/* The converse, so the rule above cannot be satisfied by simply accepting
 * everything: a share that fails even the difficulty its job was issued at is
 * still rejected. */
static void test_submit_below_the_jobs_own_difficulty_is_rejected(void) {
    obs_t obs = {0};
    stratum_conn_t *c = NULL;
    /* J1 issued at a difficulty a random hash cannot meet. */
    stratum_server_t *s = setup_job_diff_conn(&obs, 1e9, &c);
    CHECK(s != NULL);
    if (!s) return;

    /* Current difficulty far LOWER than the job's, and no grace to lean on —
     * so nothing but the job's own difficulty can decide this. */
    stratum_conn_force_difficulty_for_test(c, 1e-12, 0.0);

    char *out = NULL; size_t olen = 0;
    stratum_handle_message(s, c,
        "{\"id\":3,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J1\",\"" TEST_EN2 "\",\"60000000\",\"00000002\"]}",
        &out, &olen);

    CHECK(obs.shares == 0);
    CHECK(obs.rejects == 1);
    CHECK(strstr(obs.last_reason, "low difficulty") != NULL);
    free(out);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
    printf("ok: a share under its own job's difficulty is still rejected\n");
}

/* A job this connection was never sent has no record, so the old
 * current-difficulty-plus-grace path must still apply rather than the share
 * being accepted by default. */
static void test_unknown_job_record_falls_back_to_grace(void) {
    obs_t obs = {0};
    stratum_conn_t *c = NULL;
    stratum_server_t *s = setup_job_diff_conn(&obs, 1e-12, &c);
    CHECK(s != NULL);
    if (!s) return;

    /* Rotate to a job this connection is never notified of (its fd is -1, so
     * the broadcast skips it) — J2 therefore has no per-job record here. */
    uint8_t net[32] = {0};
    stratum_server_set_job(s, make_test_job("J2", net));

    /* Current difficulty unreachable, and the grace holds the reachable one. */
    stratum_conn_force_difficulty_for_test(c, 1e9, 1e-12);

    char *out = NULL; size_t olen = 0;
    stratum_handle_message(s, c,
        "{\"id\":3,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J2\",\"" TEST_EN2 "\",\"60000000\",\"00000003\"]}",
        &out, &olen);

    /* Accepted via the grace, and credited at the grace difficulty. */
    CHECK(obs.shares == 1);
    CHECK(obs.rejects == 0);
    CHECK(obs.last_difficulty == 1e-12);
    free(out);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
    printf("ok: no per-job record falls back to the grace path\n");
}

/* Re-notifying the SAME job must update its record in place rather than
 * consume a ring slot, or a long-lived connection on a slow chain evicts jobs
 * that are still solvable and silently returns to the old behaviour. */
static void test_rerecording_a_job_does_not_consume_ring_slots(void) {
    obs_t obs = {0};
    stratum_conn_t *c = NULL;
    stratum_server_t *s = setup_job_diff_conn(&obs, 1e-12, &c);
    CHECK(s != NULL);
    if (!s) return;

    /* Re-authorize many more times than the ring is deep. Each one re-notifies
     * the same current job, so J1's record must survive all of them. */
    for (int i = 0; i < 64; ++i) {
        char *out = NULL; size_t olen = 0;
        stratum_handle_message(s, c,
            "{\"id\":9,\"method\":\"mining.authorize\","
             "\"params\":[\"" TEST_ADDR "\",\"x\"]}",
            &out, &olen);
        free(out);
    }

    stratum_conn_force_difficulty_for_test(c, 4e6, 1e6);

    char *out = NULL; size_t olen = 0;
    stratum_handle_message(s, c,
        "{\"id\":3,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J1\",\"" TEST_EN2 "\",\"60000000\",\"00000004\"]}",
        &out, &olen);

    CHECK(obs.shares == 1);
    CHECK(obs.rejects == 0);
    CHECK(obs.last_difficulty == 1e-12);
    free(out);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
    printf("ok: re-notifying a job does not evict it from the ring\n");
}

int main(void) {
    test_subscribe();
    test_authorize_triggers_setdiff_notify();
    test_submit_unknown_job();
    test_submit_share_and_dedupe();
    test_submit_rejects_wrong_extranonce2_size();
    test_job_rotation_races_submits();
    test_authorize_rejects_non_address();
    test_authorize_address_with_label();
    test_block_wins_over_low_difficulty();
    test_vardiff_clamped_to_network_diff();
    test_vardiff_grace_accepts_old_diff_shares();
    test_socket_setup_applies_rcvtimeo();
    test_socket_setup_disabled();
    test_extranonce1_unique_across_connections();
    test_stop_waits_for_connection_threads();
    test_dedupe_same_hash_across_job_ids();
    test_authorize_resumes_difficulty_from_hint();
    test_authorize_without_hint_uses_initial();
    test_proportional_shared_coinbase();
    test_proportional_falls_back_without_window();
    test_shared_extranonce1_sequence_across_servers();
    test_server_without_shared_still_allocates_one();
    test_hint_below_vardiff_min_is_floored();
    test_initial_diff_below_vardiff_min_is_not_floored();
    test_submit_judged_at_the_jobs_own_difficulty();
    test_submit_below_the_jobs_own_difficulty_is_rejected();
    test_unknown_job_record_falls_back_to_grace();
    test_rerecording_a_job_does_not_consume_ring_slots();
    printf("test_stratum: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
