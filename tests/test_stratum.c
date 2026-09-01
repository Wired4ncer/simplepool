#include <math.h>
#include "../src/stratum.h"
#include "../src/share.h"
#include "../src/cjson/cJSON.h"

#include <assert.h>
#include <math.h>
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
    double sum_share_diff;    /* the same, summed over the run */
    char   last_worker[64];
    char   last_reason[128];
    /* Block-candidate accounting. submit_rejects makes the stubbed
     * submitblock refuse, which is the common case on a low-difficulty
     * chain and the one that used to be recorded as a block anyway. */
    int    submit_rejects;
    int    submits;
    int    found_calls;
    int    last_accepted;
    char   last_submit_error[128];
    /* Which payout scheme the observed shares arrived under. Asserting on
     * these is what makes a solo test non-vacuous: "a share arrived" would
     * pass even if the listener's mode were dropped on the floor. */
    int    last_solo;
    int    solo_shares;
    int    last_block_solo;
    /* Reject instrumentation. Asserting on these is what keeps the
     * classification honest: "a stale reject arrived" would pass on a build
     * that labelled every one of them the same way. */
    char    last_peer_ip[64];
    char    last_kind[32];
    int64_t last_job_age_ms;
} obs_t;

/* The callbacks are invoked from whichever thread handled the share, and
 * test_job_rotation_races_submits drives several at once. Guard the observer
 * so TSan reports races in the CODE UNDER TEST rather than in the harness
 * watching it. Single-threaded tests pay one uncontended lock per callback. */
static pthread_mutex_t obs_mu = PTHREAD_MUTEX_INITIALIZER;

static void on_share(void *ctx, const char *w, const char *addr,
                     uint64_t ts, double d,
                     int is_block, const char *blk, int solo) {
    (void)ts; (void)blk; (void)addr;
    obs_t *o = ctx;
    pthread_mutex_lock(&obs_mu);
    /* Recorded so a test can assert WHICH scheme a share was submitted under,
     * not merely that a share arrived. Without this the solo tests below would
     * pass on a build that ignored the listener's mode entirely. */
    o->last_solo = solo;
    if (solo) o->solo_shares++;
    o->shares++;
    o->last_difficulty = d;
    o->sum_share_diff += d;
    o->last_is_block = is_block;
    if (is_block) o->blocks++;
    snprintf(o->last_worker, sizeof(o->last_worker), "%s", w ? w : "");
    pthread_mutex_unlock(&obs_mu);
}
static void on_reject(void *ctx, const char *w, const char *peer_ip,
                      uint64_t ts, const char *r, const char *kind,
                      int64_t job_age_ms) {
    (void)ts; (void)w;
    obs_t *o = ctx;
    pthread_mutex_lock(&obs_mu);
    o->rejects++;
    snprintf(o->last_reason, sizeof(o->last_reason), "%s", r ? r : "");
    snprintf(o->last_peer_ip, sizeof(o->last_peer_ip), "%s",
             peer_ip ? peer_ip : "");
    snprintf(o->last_kind, sizeof(o->last_kind), "%s", kind ? kind : "");
    o->last_job_age_ms = job_age_ms;
    pthread_mutex_unlock(&obs_mu);
}
/* Returns 0 = the node accepted it, so the existing tests keep exercising
 * the on_block_found path. A test for the rejected case sets submit_rejects. */
static int on_block(void *ctx, const char *hex, char *errbuf, size_t errlen) {
    (void)hex;
    obs_t *o = ctx;
    if (!o) return 0;
    o->submits++;
    if (o->submit_rejects) {
        snprintf(errbuf, errlen, "inconclusive");
        return -30;
    }
    return 0;
}
static void on_block_found(void *ctx, const char *w, const char *addr,
                           uint64_t ts, uint32_t height, const char *job_id,
                           const char *hash, int64_t reward, int64_t fee,
                           int accepted, const char *submit_error, int solo) {
    (void)w; (void)addr; (void)ts; (void)height; (void)job_id; (void)hash;
    (void)reward; (void)fee;
    obs_t *o = ctx;
    /* Recorded so a test can assert which SCHEME solved the block, not merely
     * that one was found. The settle gate in main.c reads exactly this flag. */
    o->last_block_solo = solo;
    o->found_calls++;
    o->last_accepted = accepted;
    snprintf(o->last_submit_error, sizeof(o->last_submit_error), "%s",
             submit_error ? submit_error : "");
}

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
    stratum_server_set_job(s, make_test_job("0001", net), 1);

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
    /* "NOPE" is not a job id we could ever have minted, so it must be
     * reported as such and carry no age. Before the three-way split this
     * counted identically to a job the pool really did retire, which is the
     * confusion the retention argument kept running aground on. */
    CHECK(strcmp(obs.last_kind, STRATUM_REJECT_KIND_NEVER_ISSUED) == 0);
    CHECK(obs.last_job_age_ms == STRATUM_JOB_AGE_NONE);
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
    stratum_server_set_job(s, make_test_job("J1", net), 1);

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
    stratum_server_set_job(s, make_test_job("J1", net), 1);

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
        stratum_server_set_job(s, j, 1);
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
    stratum_server_set_job(s, make_test_job("J0", net), 1);

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
    stratum_server_set_job(s, make_test_job("J1", net), 1);

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

/* ---- dual-stack listener ------------------------------------------------ */

/* Connect to a started server over a chosen family and return the fd, or -1.
 * Real sockets on purpose: the whole point is what accept() records, and
 * stratum_conn_new_for_test never goes through accept() at all. */
static int dial(int family, int port) {
    int fd = socket(family, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_storage ss;
    socklen_t len;
    memset(&ss, 0, sizeof ss);
    if (family == AF_INET6) {
        struct sockaddr_in6 *a = (struct sockaddr_in6 *)(void *)&ss;
        a->sin6_family = AF_INET6;
        a->sin6_port = htons((uint16_t)port);
        a->sin6_addr = in6addr_loopback;
        len = sizeof(*a);
    } else {
        struct sockaddr_in *a = (struct sockaddr_in *)(void *)&ss;
        a->sin_family = AF_INET;
        a->sin_port = htons((uint16_t)port);
        a->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        len = sizeof(*a);
    }
    if (connect(fd, (struct sockaddr *)&ss, len) < 0) { close(fd); return -1; }
    return fd;
}

/* Walks a small port range rather than insisting on one number. A fixed port
 * made this suite FLAKY: a back-to-back run (a mutation check, say) can find
 * the previous process's socket still lingering, and the bind fails for a
 * reason that has nothing to do with the code under test. A deploy gate that
 * fails one run in twenty teaches people to re-run it, which is worse than
 * having no gate. Writes the port actually bound back through *port. */
static stratum_server_t *start_on_obs(const char *addr, int *port, obs_t *obs) {
    for (int p = *port; p < *port + 20; p++) {
        stratum_cfg_t cfg = { .bind_port = p, .max_conns = 8,
                              .initial_diff = 1.0, .vardiff_enabled = 0 };
        if (obs) {
            cfg.ctx = obs;
            cfg.on_share = on_share;
            cfg.on_reject = on_reject;
            cfg.on_block = on_block;
        }
        snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "%s", addr);
        stratum_server_t *s = NULL;
        if (stratum_server_start(&cfg, &s) != 0) continue;
        uint8_t net[32]; memset(net, 0xff, sizeof net);
        stratum_server_set_job(s, make_test_job("J1", net), 1);
        *port = p;
        return s;
    }
    return NULL;
}

static stratum_server_t *start_on(const char *addr, int *port) {
    return start_on_obs(addr, port, NULL);
}

/* 🔴 GATE: on a dual-stack listener an IPv4 client MUST still record a dotted
 * quad, not ::ffff:127.0.0.1.
 *
 * ecash-rental-live.sh attributes a worker to the rental port by matching the
 * authorize line's peer IP against what `ss` reports as established, and `ss`
 * prints dotted quad. If accept() started handing up mapped forms, the two
 * would never match and the tool would report ZERO rental workers during a
 * live order — a silent wrong answer feeding the decision that gates restarts.
 * That is why this is a gate and not a comment. */
static void test_dual_stack_ipv4_peer_is_dotted_quad(void) {
    int port = 39334;
    stratum_server_t *s = start_on("::", &port);
    if (!s) { printf("ok: dual-stack skipped (no IPv6 on this host)\n"); return; }

    int fd = dial(AF_INET, port);
    CHECK(fd >= 0);                       /* an IPv4 client must be accepted */
    if (fd >= 0) {
        sleep_ms(150);
        char ip[64] = {0};
        CHECK(stratum_server_last_peer_ip_for_test(s, ip, sizeof ip) == 0);
        /* the assertion the gate exists for */
        CHECK(strncmp(ip, "::ffff:", 7) != 0);
        CHECK(strcmp(ip, "127.0.0.1") == 0);
        close(fd);
    }
    stratum_server_free(s);
    printf("ok: dual-stack records an IPv4 peer as a dotted quad\n");
}

/* ...and an IPv6 client is accepted at all, which is the point of the change. */
static void test_dual_stack_accepts_ipv6(void) {
    int port = 39335;
    stratum_server_t *s = start_on("::", &port);
    if (!s) { printf("ok: dual-stack v6 skipped (no IPv6 on this host)\n"); return; }

    int fd = dial(AF_INET6, port);
    CHECK(fd >= 0);
    if (fd >= 0) {
        sleep_ms(150);
        char ip[64] = {0};
        CHECK(stratum_server_last_peer_ip_for_test(s, ip, sizeof ip) == 0);
        CHECK(strchr(ip, ':') != NULL);   /* a real IPv6 literal */
        close(fd);
    }
    stratum_server_free(s);
    printf("ok: dual-stack accepts an IPv6 client\n");
}

/* ⛔ The config gate: "0.0.0.0" must keep meaning IPv4-only, so installing this
 * binary against production's existing config changes NOTHING. If this test
 * ever passes an IPv6 connect, the gate has leaked and the deploy is no longer
 * staged. */
static void test_ipv4_wildcard_is_still_ipv4_only(void) {
    int port = 39336;
    stratum_server_t *s = start_on("0.0.0.0", &port);
    CHECK(s != NULL);
    if (!s) return;

    int v4 = dial(AF_INET, port);
    CHECK(v4 >= 0);                       /* IPv4 still works */
    if (v4 >= 0) close(v4);

    /* ⛔ Prove IPv6 is usable HERE before trusting a refusal. Without this the
     * test passes on a host with no IPv6 at all, having proven nothing — the
     * same vacuous-pass shape this suite keeps catching elsewhere. The other
     * two dual-stack tests skip via start_on() returning NULL; this one never
     * calls it, so it cannot tell "refused" from "impossible" on its own.
     * ⚠️ And it prints SKIP, not ok: a vacuous pass and a real pass must not
     * read the same. */
    int probe_port = 39400;
    stratum_server_t *probe = start_on("::", &probe_port);
    if (!probe) {
        stratum_server_free(s);
        printf("SKIP: no IPv6 on this host — the 0.0.0.0 gate is UNTESTED\n");
        return;
    }
    int ok6 = dial(AF_INET6, probe_port);
    stratum_server_free(probe);
    if (ok6 < 0) {
        stratum_server_free(s);
        printf("SKIP: no IPv6 loopback — the 0.0.0.0 gate is UNTESTED\n");
        return;
    }
    close(ok6);

    int v6 = dial(AF_INET6, port);
    CHECK(v6 < 0);                        /* IPv6 must be REFUSED */
    if (v6 >= 0) close(v6);

    stratum_server_free(s);
    printf("ok: listen_addr 0.0.0.0 stays IPv4-only (the gate holds)\n");
}

/* ---- vardiff vs. a miner enforcing its own difficulty floor ------------ */

/* Reproduce the hash a submit would produce, so a test can pick nonces that
 * achieve a chosen difficulty. make_test_job carries no merkle branches, so
 * the merkle root is just the coinbase txid. */
static double achieved_diff_for_nonce(stratum_server_t *s, stratum_conn_t *c,
                                      const char *job_id, const char *en2_hex,
                                      uint32_t nonce) {
    const uint8_t *cb1 = NULL, *cb2 = NULL, *en1 = NULL;
    size_t cb1_len = 0, cb2_len = 0;
    if (stratum_conn_coinbase_for_test(s, c, job_id, &cb1, &cb1_len,
                                       &cb2, &cb2_len, &en1) != 0) return 0.0;

    /* Widths come from the header, not literals: this fork runs
     * extranonce2 at 8 bytes for rented hashrate, and a hardcoded 4 here
     * rebuilds a DIFFERENT coinbase than the pool hashed — the predicted
     * difficulty then has nothing to do with the submitted share. */
    uint8_t en2[STRATUM_EXTRANONCE2_SIZE];
    for (int i = 0; i < STRATUM_EXTRANONCE2_SIZE; ++i) {
        unsigned byte = 0;
        sscanf(en2_hex + 2 * i, "%2x", &byte);
        en2[i] = (uint8_t)byte;
    }

    size_t cb_len = cb1_len + STRATUM_EXTRANONCE1_SIZE +
                    STRATUM_EXTRANONCE2_SIZE + cb2_len;
    uint8_t *cb = malloc(cb_len);
    if (!cb) return 0.0;
    size_t off = 0;
    memcpy(cb + off, cb1, cb1_len); off += cb1_len;
    memcpy(cb + off, en1, STRATUM_EXTRANONCE1_SIZE);
    off += STRATUM_EXTRANONCE1_SIZE;
    memcpy(cb + off, en2, STRATUM_EXTRANONCE2_SIZE);
    off += STRATUM_EXTRANONCE2_SIZE;
    memcpy(cb + off, cb2, cb2_len);

    uint8_t cb_txid_le[32], root_le[32], header[80], hash_be[32];
    dsha256(cb, cb_len, cb_txid_le);
    free(cb);
    merkle_root_from_branches(cb_txid_le, NULL, 0, root_le);
    uint8_t prev[32] = {0};
    build_header(1, prev, root_le, 0x60000000u, 0x1d00ffffu, nonce, header);
    hash_header(header, hash_be);
    return target_to_diff(hash_be);
}

/* Find the next nonce at or above `from` whose share achieves at least `want`
 * and less than `want_max`. Returns the achieved difficulty and writes the
 * nonce, or 0.0 if none was found.
 *
 * The upper bound matters because extranonce1 is seeded from the clock, so
 * every run mines a different set of hashes. Without it, how far a share
 * overshoots its target is left to chance, and a test that depends on shares
 * NOT overshooting by some factor becomes a rare flake. */
static double mine_nonce(stratum_server_t *s, stratum_conn_t *c,
                         const char *job_id, const char *en2_hex,
                         double want, double want_max,
                         uint32_t from, uint32_t *out_nonce) {
    for (uint32_t n = from; n < from + 4000000u; ++n) {
        double d = achieved_diff_for_nonce(s, c, job_id, en2_hex, n);
        if (d >= want && d < want_max) { *out_nonce = n; return d; }
    }
    return 0.0;
}

static void submit_nonce(stratum_server_t *s, stratum_conn_t *c,
                         const char *en2_hex, uint32_t nonce,
                         char **out, size_t *olen) {
    char msg[256];
    snprintf(msg, sizeof msg,
             "{\"id\":9,\"method\":\"mining.submit\","
             "\"params\":[\"w\",\"J1\",\"%s\",\"60000000\",\"%08x\"]}",
             en2_hex, nonce);
    stratum_handle_message(s, c, msg, out, olen);
}

/* Pull the difficulty out of a mining.set_difficulty notification. */
static double set_diff_value(const char *out) {
    const char *p = out ? strstr(out, "mining.set_difficulty") : NULL;
    if (!p) return -1.0;
    p = strstr(p, "\"params\":[");
    if (!p) return -1.0;
    return atof(p + strlen("\"params\":["));
}

static void handshake(stratum_server_t *s, stratum_conn_t *c) {
    char *out = NULL; size_t olen = 0;
    stratum_handle_message(s, c, "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}",
                           &out, &olen); free(out); out = NULL; olen = 0;
    stratum_handle_message(s, c,
        "{\"id\":2,\"method\":\"mining.authorize\","
         "\"params\":[\"" TEST_ADDR "\",\"x\"]}",
        &out, &olen); free(out);
}

/* Re-arm the connection's vardiff window.
 *
 * mine_nonce() scans thousands of candidate nonces, rebuilding and hashing a
 * coinbase for each, and that takes real time — around a second for six
 * shares. The window is armed at authorize, so on a tree where mining runs
 * slower than vardiff_window_sec the FIRST submit already finds the window
 * elapsed and retargets off a single sample, before the test has established
 * anything. Upstream sits just under that line; this fork's 8-byte
 * extranonce2 makes each iteration ~30% dearer and pushes it over.
 *
 * Re-authorizing re-arms the window (start, share count and window minimum
 * all reset) without touching the difficulty already in force, so the shares
 * below land in one window as the test intends. */
static void rearm_vardiff_window(stratum_server_t *s, stratum_conn_t *c) {
    char *out = NULL; size_t olen = 0;
    stratum_handle_message(s, c,
        "{\"id\":8,\"method\":\"mining.authorize\","
         "\"params\":[\"" TEST_ADDR "\",\"x\"]}",
        &out, &olen);
    free(out);
}

/* A miner enforcing a local difficulty floor 1000x above what the pool
 * assigned it. Every share it sends clears the floor, but the pool credits
 * each one at the difficulty it assigned and the rate loop sees a share rate
 * inside its deadband, so nothing ever moves it off vardiff_min. Vardiff must
 * notice that every share in the window cleared far more than it asked for,
 * and raise the difficulty to just under the floor it measured.
 *
 * Scaled down from the difficulties this was found at in production (assigned
 * 1, floor 256): difficulty D takes D * 2^32 hashes to mine, so a real 256
 * would be 2^40 hashes per share. The logic is all ratios, so 1e-9 against a
 * 1e-6 floor exercises exactly the same path for ~4300 hashes a share. */
static void test_vardiff_tracks_miner_local_floor(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1,
                           .initial_diff = 1e-9,
                           .vardiff_enabled = 1,
                           .vardiff_target_spm = 250.0,
                           .vardiff_min = 1e-9,
                           .vardiff_max = 1e15,
                           .vardiff_window_sec = 1,
                           .ctx = &obs, .on_share = on_share,
                           .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);

    /* All-zero network target: nothing is ever a block, so acceptance comes
     * only from the share-difficulty path. */
    uint8_t net[32] = {0};
    stratum_server_set_job(s, make_test_job("J1", net), 1);

    stratum_conn_t *c = stratum_conn_new_for_test(s);
    handshake(s, c);
    CHECK(stratum_conn_difficulty_for_test(c) == 1e-9);

    /* Mine six shares that each clear the miner's own floor of 1e-6. */
    const int N = 6;
    uint32_t nonce[6];
    double   achieved[6];
    double   floor_seen = 1e300;
    uint32_t from = 1;
    int mined = 1;
    for (int i = 0; i < N; ++i) {
        achieved[i] = mine_nonce(s, c, "J1", TEST_EN2, 1e-6, HUGE_VAL,
                                 from, &nonce[i]);
        if (achieved[i] <= 0.0) { mined = 0; break; }
        if (achieved[i] < floor_seen) floor_seen = achieved[i];
        from = nonce[i] + 1;
    }
    CHECK(mined == 1);
    if (!mined) { stratum_conn_free_for_test(c); stratum_server_free(s); return; }
    rearm_vardiff_window(s, c);

    char *out = NULL; size_t olen = 0;
    /* First five land inside the window: counted, no retarget yet. */
    for (int i = 0; i < N - 1; ++i) {
        submit_nonce(s, c, TEST_EN2, nonce[i], &out, &olen);
        CHECK(set_diff_value(out) < 0.0);
        free(out); out = NULL; olen = 0;
    }
    CHECK(obs.shares == N - 1);
    CHECK(obs.rejects == 0);
    /* Every one was credited at the assigned 1e-9, not the >=1e-6 it actually
     * achieved: the pool books a thousandth of the work it received, which is
     * the under-crediting this fix is about. */
    CHECK(obs.sum_share_diff > 0.999e-9 * (N - 1) &&
          obs.sum_share_diff < 1.001e-9 * (N - 1));
    CHECK(floor_seen > 500.0 * (obs.sum_share_diff / (N - 1)));

    sleep_ms(1100);
    submit_nonce(s, c, TEST_EN2, nonce[N - 1], &out, &olen);
    CHECK(obs.shares == N);
    CHECK(obs.rejects == 0);

    /* Difficulty must now sit just under the floor we measured. */
    double got = set_diff_value(out);
    double want = floor_seen * 0.95;
    CHECK(got > 0.0);
    CHECK(got > want * 0.999 && got < want * 1.001);
    /* Under the smallest difficulty actually observed, so the retarget never
     * lands somewhere no share in the window would have reached. */
    CHECK(got < floor_seen);
    CHECK(got > 1e-7);                /* and off vardiff_min for good */
    double held = stratum_conn_difficulty_for_test(c);
    CHECK(held > got * 0.999 && held < got * 1.001);
    free(out);

    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

/* The floor detector must obey the SAME sample floor the rate loop obeys.
 *
 * `vardiff_min_samples` extends an under-sampled window rather than acting on
 * it, but the extension is bounded by `vardiff_max_window_mult` — so a slow
 * connection's window still ends holding somewhere between
 * VD_FLOOR_MIN_SAMPLES and vardiff_min_samples shares. Upstream's floor check
 * reads `vd_window_shares` directly, so in that band it fires on as few as
 * five samples no matter how high the sample floor is set.
 *
 * That band is not hypothetical: it is exactly the proxied rental connection
 * `cacc888` was written for, going quiet between bursts. An accepted share
 * achieves at least the difficulty it was accepted at, and P(achieved > 4x
 * assigned) = 1/4 for a uniform hash, so a false trigger costs 0.25^n per
 * window — 1/1024 at five samples, not the 0.25^20 the sample floor is
 * supposed to buy. The correction is uncapped and the recovery is capped, so
 * a false trigger costs more to undo than it cost to cause.
 *
 * Below: a window that ends at the extension cap with five shares, every one
 * of them clearing 1000x the assigned difficulty. The rate loop is inside its
 * deadband, so any difficulty change here is the floor detector's. It must
 * not fire — and then, with the window properly sampled, it must. */
static void test_vardiff_floor_detect_respects_min_samples(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1,
                           .initial_diff = 1e-9,
                           .vardiff_enabled = 1,
                           .vardiff_target_spm = 150.0,
                           .vardiff_min = 1e-9,
                           .vardiff_max = 1e15,
                           .vardiff_window_sec = 1,
                           .vardiff_min_samples = 20,
                           .vardiff_max_window_mult = 2,
                           .vardiff_idle_step = 2.0,
                           .ctx = &obs, .on_share = on_share,
                           .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);

    uint8_t net[32] = {0};
    stratum_server_set_job(s, make_test_job("J1", net), 1);

    stratum_conn_t *c = stratum_conn_new_for_test(s);
    handshake(s, c);
    CHECK(stratum_conn_difficulty_for_test(c) == 1e-9);

    /* Pre-mine every share up front: mining is far slower than submitting,
     * and a window armed before it would elapse during the mining. */
    const int N = 25;
    uint32_t nonce[25];
    double   floor_seen = 1e300;
    uint32_t from = 1;
    int mined = 1;
    for (int i = 0; i < N; ++i) {
        double a = mine_nonce(s, c, "J1", TEST_EN2, 1e-6, HUGE_VAL,
                              from, &nonce[i]);
        if (a <= 0.0) { mined = 0; break; }
        if (a < floor_seen) floor_seen = a;
        from = nonce[i] + 1;
    }
    CHECK(mined == 1);
    if (!mined) { stratum_conn_free_for_test(c); stratum_server_free(s); return; }

    /* ---- under-sampled window: five shares, all clearing 1000x ---------- */
    rearm_vardiff_window(s, c);
    char *out = NULL; size_t olen = 0;
    for (int i = 0; i < 4; ++i) {
        submit_nonce(s, c, TEST_EN2, nonce[i], &out, &olen);
        CHECK(set_diff_value(out) < 0.0);
        free(out); out = NULL; olen = 0;
    }
    /* Past the extension cap (window_sec * max_window_mult = 2s), so the
     * window ends on the next share instead of extending further. */
    sleep_ms(2200);
    submit_nonce(s, c, TEST_EN2, nonce[4], &out, &olen);

    /* The fixture must prove it reached the retarget path at all, or this
     * negative assertion passes for the wrong reason. */
    CHECK(obs.shares == 5);
    CHECK(obs.rejects == 0);
    /* ~136 spm against a 150 target: ratio 0.91, inside the [0.5, 2.0]
     * deadband, so the rate loop proposes nothing and the only thing that
     * could move the difficulty here is the floor detector. */
    CHECK(set_diff_value(out) < 0.0);
    CHECK(stratum_conn_difficulty_for_test(c) == 1e-9);
    free(out); out = NULL; olen = 0;

    /* ---- properly sampled window: the detector must still work ---------- */
    /* The window was reset by the retarget above; fill this one to the
     * sample floor. Same connection, same shares, same ratio — only the
     * sample count differs, which is what proves the guard is a sample
     * floor and not a disabled detector. */
    for (int i = 5; i < 24; ++i) {
        submit_nonce(s, c, TEST_EN2, nonce[i], &out, &olen);
        free(out); out = NULL; olen = 0;
    }
    sleep_ms(1100);
    submit_nonce(s, c, TEST_EN2, nonce[24], &out, &olen);
    CHECK(obs.shares == 25);
    CHECK(obs.rejects == 0);

    double got = set_diff_value(out);
    CHECK(got > 0.0);
    CHECK(got > 1e-7);                       /* off vardiff_min for good */
    CHECK(got < floor_seen);                 /* just UNDER the measured floor */
    free(out);

    stratum_conn_free_for_test(c);
    stratum_server_free(s);
    printf("ok: the floor detector obeys vardiff_min_samples\n");
}

/* The floor detector must not block a legitimate downward retarget. A miner
 * whose shares match its assigned difficulty still achieves slightly more
 * than it on every accepted share, so testing the window minimum against the
 * rate loop's already-cut proposal would fire on every 4x cut and pin the
 * difficulty of every miner that simply slowed down. */
static void test_vardiff_still_lowers_for_a_matched_miner(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1,
                           .initial_diff = 1e-6,
                           .vardiff_enabled = 1,
                           .vardiff_target_spm = 6000.0,
                           .vardiff_min = 1e-12,
                           .vardiff_max = 1e15,
                           .vardiff_window_sec = 1,
                           .ctx = &obs, .on_share = on_share,
                           .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);

    uint8_t net[32] = {0};
    stratum_server_set_job(s, make_test_job("J1", net), 1);

    stratum_conn_t *c = stratum_conn_new_for_test(s);
    handshake(s, c);
    CHECK(stratum_conn_difficulty_for_test(c) == 1e-6);

    /* A matched miner: every share clears the difficulty it was assigned and
     * overshoots by less than 2x, so the window minimum is nowhere near the
     * 4x the floor check triggers on. Bounding the overshoot is what keeps
     * that true on every run rather than almost every run. */
    const int N = 6;
    uint32_t nonce[6];
    uint32_t from = 1;
    int mined = 1;
    for (int i = 0; i < N; ++i) {
        double d = mine_nonce(s, c, "J1", TEST_EN2, 1e-6, 2e-6,
                              from, &nonce[i]);
        if (d <= 0.0) { mined = 0; break; }
        from = nonce[i] + 1;
    }
    CHECK(mined == 1);
    if (!mined) { stratum_conn_free_for_test(c); stratum_server_free(s); return; }
    rearm_vardiff_window(s, c);

    /* The window was armed at authorize, and the mining above ran inside it —
     * 163 ms on a quiet machine, over 8 s under a sanitizer. Crossing the
     * 1 s boundary here spends a retarget this test never asked for, and
     * everything below then measures the wrong window. Re-arm so the test
     * turns on behaviour rather than on how fast the box mines. */
    stratum_conn_rearm_vardiff_for_test(c);

    char *out = NULL; size_t olen = 0;
    for (int i = 0; i < N - 1; ++i) {
        submit_nonce(s, c, TEST_EN2, nonce[i], &out, &olen);
        free(out); out = NULL; olen = 0;
    }
    CHECK(obs.shares == N - 1);
    CHECK(obs.rejects == 0);

    /* Six shares against a 6000/min target is far under rate: the retarget
     * cuts by the 4x step cap, to a quarter of 1e-6. Before the floor check
     * was made to compare against the difficulty actually in force, the
     * window minimum (always at least the assigned difficulty, and here about
     * 1.17e-6) beat this proposal times four, and pinned the difficulty
     * instead of letting it fall. */
    sleep_ms(1100);
    submit_nonce(s, c, TEST_EN2, nonce[N - 1], &out, &olen);
    CHECK(obs.rejects == 0);
    double got = set_diff_value(out);
    CHECK(got > 2.49e-7 && got < 2.51e-7);
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
    stratum_server_set_job(s, make_test_job("J1", net), 1);

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

/* vardiff_min_samples: a window that elapsed but holds too few shares must
 * NOT retarget — it must stay open and keep accumulating.
 *
 * This is the oscillation fix. At target_spm=12 over a 30s window an
 * on-target connection produces six shares, and Poisson noise on six
 * samples (+/-41%) pushes `ratio` outside the [0.5, 2.0] deadband on its
 * own — so the controller chased noise and cycled 500000 -> 1141329 ->
 * 569753 -> 500000 in production on 2026-08-24.
 *
 * ⚠️ The assertion that matters is the NEGATIVE one on shares 1..4. To keep
 * it from passing for the wrong reason, share 5 must then prove a retarget
 * was reachable all along: same connection, same elapsed window, same
 * ratio — the only thing that changed is the sample count. */
static void test_vardiff_waits_for_min_samples(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1,
                           .initial_diff = 1e-12,
                           .vardiff_enabled = 1,
                           .vardiff_target_spm = 0.001,
                           .vardiff_min = 1e-12,
                           .vardiff_max = 1e15,
                           .vardiff_window_sec = 1,
                           .vardiff_min_samples = 5,
                           .vardiff_max_window_mult = 8,
                           .vardiff_idle_step = 2.0,
                           .ctx = &obs, .on_share = on_share,
                           .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);

    /* All-zero network target: nothing is ever a block, and no network
     * clamp can mask the retarget we are looking for. */
    uint8_t net[32] = {0};
    stratum_server_set_job(s, make_test_job("J1", net), 1);

    stratum_conn_t *c = stratum_conn_new_for_test(s);
    char *out = NULL; size_t olen = 0;
    stratum_handle_message(s, c, "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}",
                           &out, &olen); free(out); out=NULL; olen=0;
    stratum_handle_message(s, c,
        "{\"id\":2,\"method\":\"mining.authorize\","
         "\"params\":[\"" TEST_ADDR "\",\"x\"]}",
        &out, &olen); free(out); out=NULL; olen=0;

    /* Let the nominal window elapse. Every submit below is past it. */
    sleep_ms(1100);

    for (int i = 1; i <= 4; ++i) {
        char msg[256];
        snprintf(msg, sizeof msg,
                 "{\"id\":%d,\"method\":\"mining.submit\","
                 "\"params\":[\"w\",\"J1\",\"" TEST_EN2 "\",\"60000000\","
                 "\"0000000%d\"]}", 10 + i, i);
        stratum_handle_message(s, c, msg, &out, &olen);
        CHECK(out != NULL);
        /* Under the sample floor: accepted, but no difficulty change. */
        CHECK(strstr(out, "mining.set_difficulty") == NULL);
        free(out); out=NULL; olen=0;
    }
    /* Precondition for the negative assertions above: those four shares
     * really were ACCEPTED, so they really did land in the vardiff window.
     * Without this the test would pass just as well if every submit had
     * been rejected before ever reaching the retarget path. */
    CHECK(obs.shares == 4);
    CHECK(obs.rejects == 0);

    /* Fifth share meets the floor — now the retarget fires. */
    stratum_handle_message(s, c,
        "{\"id\":15,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J1\",\"" TEST_EN2 "\",\"60000000\",\"00000005\"]}",
        &out, &olen);
    CHECK(out != NULL);
    CHECK(strstr(out, "mining.set_difficulty") != NULL);
    CHECK(obs.shares == 5);
    free(out);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

/* Value of the LAST mining.set_difficulty in a response buffer, or -1. */
static double last_set_difficulty(const char *buf) {
    const char *hit = NULL, *q = buf;
    while ((q = strstr(q, "mining.set_difficulty")) != NULL) { hit = q; q += 8; }
    if (!hit) return -1.0;
    const char *pr = strstr(hit, "\"params\":[");
    if (!pr) return -1.0;
    return strtod(pr + 10, NULL);
}

/* Authorize helper: subscribe then authorize with `pw` as the password. */
static double authorize_with_password(stratum_server_t *s, stratum_conn_t *c,
                                      const char *pw) {
    char *out = NULL; size_t olen = 0;
    stratum_handle_message(s, c, "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}",
                           &out, &olen); free(out); out=NULL; olen=0;
    char msg[256];
    snprintf(msg, sizeof msg,
             "{\"id\":2,\"method\":\"mining.authorize\","
             "\"params\":[\"" TEST_ADDR "\",\"%s\"]}", pw);
    stratum_handle_message(s, c, msg, &out, &olen);
    double d = out ? last_set_difficulty(out) : -1.0;
    free(out);
    return d;
}

/* A miner asking for a HIGHER difficulty via the stratum password gets it. */
static void test_password_diff_raises_difficulty(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 2,
                           .initial_diff = 1000,
                           .vardiff_enabled = 1, .vardiff_target_spm = 12,
                           .vardiff_min = 1, .vardiff_max = 1e15,
                           .vardiff_window_sec = 30,
                           .max_suggested_diff = 5e7,
                           .ctx = &obs, .on_share = on_share,
                           .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);
    uint8_t net[32] = {0};                    /* no network clamp */
    stratum_server_set_job(s, make_test_job("J1", net), 1);

    stratum_conn_t *c = stratum_conn_new_for_test(s);
    CHECK(authorize_with_password(s, c, "d=4657000") == 4657000.0);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

/* ⛔ The security property. A request is a FLOOR, never a pin, so `d=1`
 * cannot be used to force a connection below what the pool chose — that
 * would be ~93,000 shares/sec from a 400 TH/s miner aimed at the share
 * pipeline. The connection must stay where the pool put it. */
static void test_password_diff_cannot_lower_difficulty(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 2,
                           .initial_diff = 1000,
                           .vardiff_enabled = 1, .vardiff_target_spm = 12,
                           .vardiff_min = 1000, .vardiff_max = 1e15,
                           .vardiff_window_sec = 30,
                           .max_suggested_diff = 5e7,
                           .ctx = &obs, .on_share = on_share,
                           .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);
    uint8_t net[32] = {0};
    stratum_server_set_job(s, make_test_job("J1", net), 1);

    stratum_conn_t *c = stratum_conn_new_for_test(s);
    /* Control: the same connection with no request sits at initial_diff. */
    CHECK(authorize_with_password(s, c, "d=1") == 1000.0);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

/* A request above max_suggested_diff is capped, not honoured. */
static void test_password_diff_capped(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 2,
                           .initial_diff = 1000,
                           .vardiff_enabled = 1, .vardiff_target_spm = 12,
                           .vardiff_min = 1, .vardiff_max = 1e15,
                           .vardiff_window_sec = 30,
                           .max_suggested_diff = 1e6,
                           .ctx = &obs, .on_share = on_share,
                           .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);
    uint8_t net[32] = {0};
    stratum_server_set_job(s, make_test_job("J1", net), 1);

    stratum_conn_t *c = stratum_conn_new_for_test(s);
    CHECK(authorize_with_password(s, c, "d=99999999") == 1e6);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

/* The network-difficulty clamp still wins over a miner's request: a share
 * target harder than the network target makes the miner discard valid
 * blocks locally, before the pool ever sees them. */
static void test_requested_diff_clamped_to_network(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 2,
                           .initial_diff = 1e-12,
                           .vardiff_enabled = 1, .vardiff_target_spm = 12,
                           .vardiff_min = 1e-12, .vardiff_max = 1e15,
                           .vardiff_window_sec = 30,
                           .max_suggested_diff = 5e7,
                           .ctx = &obs, .on_share = on_share,
                           .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);
    uint8_t net[32] = {0};                    /* DIFF1 => network diff 1.0 */
    net[4] = 0xff; net[5] = 0xff;
    stratum_server_set_job(s, make_test_job("J1", net), 1);

    stratum_conn_t *c = stratum_conn_new_for_test(s);
    CHECK(authorize_with_password(s, c, "d=4657000") == 1.0);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

/* mining.suggest_difficulty is the formal request channel and must behave
 * exactly like the password form. */
static void test_suggest_difficulty_method(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 2,
                           .initial_diff = 1000,
                           .vardiff_enabled = 1, .vardiff_target_spm = 12,
                           .vardiff_min = 1, .vardiff_max = 1e15,
                           .vardiff_window_sec = 30,
                           .max_suggested_diff = 5e7,
                           .ctx = &obs, .on_share = on_share,
                           .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);
    uint8_t net[32] = {0};
    stratum_server_set_job(s, make_test_job("J1", net), 1);

    stratum_conn_t *c = stratum_conn_new_for_test(s);
    char *out = NULL; size_t olen = 0;
    stratum_handle_message(s, c, "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}",
                           &out, &olen); free(out); out=NULL; olen=0;
    /* Arrives BEFORE authorize, which is where firmware usually sends it. */
    stratum_handle_message(s, c,
        "{\"id\":2,\"method\":\"mining.suggest_difficulty\",\"params\":[2500000]}",
        &out, &olen); free(out); out=NULL; olen=0;
    stratum_handle_message(s, c,
        "{\"id\":3,\"method\":\"mining.authorize\","
        "\"params\":[\"" TEST_ADDR "\",\"x\"]}", &out, &olen);
    CHECK(out != NULL);
    CHECK(last_set_difficulty(out) == 2500000.0);
    free(out);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

/* max_suggested_diff <= 0 DISABLES requests rather than uncapping them --
 * the deploy gate, and the safer reading of "0". */
static void test_requests_disabled_when_cap_is_zero(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 2,
                           .initial_diff = 1000,
                           .vardiff_enabled = 1, .vardiff_target_spm = 12,
                           .vardiff_min = 1, .vardiff_max = 1e15,
                           .vardiff_window_sec = 30,
                           .max_suggested_diff = 0,   /* the gate, shut */
                           .ctx = &obs, .on_share = on_share,
                           .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);
    uint8_t net[32] = {0};
    stratum_server_set_job(s, make_test_job("J1", net), 1);
    stratum_conn_t *c = stratum_conn_new_for_test(s);
    /* Ignored, NOT honoured as an uncapped request. */
    CHECK(authorize_with_password(s, c, "d=4657000") == 1000.0);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

/* The password field is untrusted input from anyone who can open a socket.
 * `d=` is matched only at a token boundary, so a password that merely
 * CONTAINS those characters is not a difficulty request. */
static void test_password_diff_parsing_edge_cases(void) {
    struct { const char *pw; double want; } cases[] = {
        { "x",              1000.0 },    /* the conventional no-op password */
        { "",               1000.0 },
        { "d=4657000",   4657000.0 },
        { "x,d=250000",   250000.0 },    /* among other tokens */
        { "x;d=250000",   250000.0 },
        { "id=7",           1000.0 },    /* NOT a request -- mid-token */
        { "bad=5",          1000.0 },
        { "d=",             1000.0 },    /* no number */
        { "d=abc",          1000.0 },
        { "d=-5",           1000.0 },    /* negative rejected, not clamped */
        { "d=0",            1000.0 },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        obs_t obs = {0};
        stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 2,
                               .initial_diff = 1000,
                               .vardiff_enabled = 1, .vardiff_target_spm = 12,
                               .vardiff_min = 1, .vardiff_max = 1e15,
                               .vardiff_window_sec = 30,
                               .max_suggested_diff = 5e7,
                               .ctx = &obs, .on_share = on_share,
                               .on_reject = on_reject, .on_block = on_block };
        snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
        stratum_server_t *s = NULL;
        stratum_server_start(&cfg, &s);
        uint8_t net[32] = {0};
        stratum_server_set_job(s, make_test_job("J1", net), 1);
        stratum_conn_t *c = stratum_conn_new_for_test(s);
        double got = authorize_with_password(s, c, cases[i].pw);
        if (got != cases[i].want) {
            fprintf(stderr, "  password \"%s\": got %g want %g\n",
                    cases[i].pw, got, cases[i].want);
        }
        CHECK(got == cases[i].want);
        stratum_conn_free_for_test(c);
        stratum_server_free(s);
    }
}

/* Vardiff must not drag a connection back below what the miner asked for.
 * Without the floor the request survives exactly one window: a higher
 * difficulty means a rate under target -- which is the POINT -- and vardiff
 * reads that as "too hard" and lowers it straight back.
 *
 * ⚠️ Two connections on one server, because the assertion for the requesting
 * one is a NEGATIVE (no set_difficulty). The control proves the retarget path
 * really is reached under this config; without it the test would pass just as
 * well if vardiff had never run at all.
 */
static void test_vardiff_cannot_lower_below_request(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 4,
                           .initial_diff = 1e-12,
                           .vardiff_enabled = 1,
                           .vardiff_target_spm = 1e9,   /* always "too slow" */
                           .vardiff_min = 1e-18, .vardiff_max = 1e15,
                           .vardiff_window_sec = 1,
                           .vardiff_min_samples = 0,    /* retarget at once */
                           .max_suggested_diff = 5e7,
                           .ctx = &obs, .on_share = on_share,
                           .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);
    uint8_t net[32] = {0};
    stratum_server_set_job(s, make_test_job("J1", net), 1);

    char *out = NULL; size_t olen = 0;

    /* Control: no request. Vardiff sees a rate far under target and lowers
     * it by the 4x step cap. Difficulties are tiny so a fixed nonce still
     * clears the share target -- at a realistic difficulty no canned submit
     * would be ACCEPTED, and an accepted share is what ticks vardiff. */
    stratum_conn_t *ctl = stratum_conn_new_for_test(s);
    CHECK(authorize_with_password(s, ctl, "x") == 1e-12);
    sleep_ms(1100);
    stratum_handle_message(s, ctl,
        "{\"id\":9,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J1\",\"" TEST_EN2 "\",\"60000000\",\"000000a1\"]}",
        &out, &olen);
    CHECK(out != NULL);
    {   /* the retarget path IS reached: 1e-12 lowered by the 4x cap */
        double got = last_set_difficulty(out);
        CHECK(got > 0.0 && fabs(got - 2.5e-13) < 1e-15);
    }
    free(out); out=NULL; olen=0;

    /* Requesting connection: same server, same window, same ratio -- the
     * only difference is the request. It must not move. */
    stratum_conn_t *req = stratum_conn_new_for_test(s);
    CHECK(authorize_with_password(s, req, "d=1e-12") == 1e-12);
    sleep_ms(1100);
    stratum_handle_message(s, req,
        "{\"id\":10,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J1\",\"" TEST_EN2 "\",\"60000000\",\"000000b2\"]}",
        &out, &olen);
    CHECK(out != NULL);
    /* Precondition: that submit really was accepted, so it really did tick
     * vardiff -- otherwise the negative below proves nothing. */
    CHECK(obs.shares == 2);
    CHECK(obs.rejects == 0);
    CHECK(strstr(out, "mining.set_difficulty") == NULL);
    free(out);

    stratum_conn_free_for_test(ctl);
    stratum_conn_free_for_test(req);
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
    stratum_server_set_job(s, make_test_job("J1", net), 1);

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
    stratum_server_set_job(s, make_test_job("J1", net), 1);

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
    stratum_server_set_job(s, make_test_job("J2", net), 1);
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
    stratum_server_set_job(s, job, 1);

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
    stratum_server_set_job(s, make_prop_job("P2", net), 1);

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

/* extranonce1 must come from ONE sequence for every connection the pool
 * serves, whichever port it arrived on.
 *
 * This is the invariant the rental port rests on. Two connections handed the
 * same extranonce1 render identical coinbases, mine identical headers, and
 * find the same hash from the same nonce — the share is credited twice and
 * half the hashrate is wasted.
 *
 * ⓘ This test used to start TWO servers sharing a stratum_shared_t, because
 * that was the only way to have two ports. Under the one-server/N-listener
 * model the hazard is architecturally impossible: every listener accepts into
 * the same server, so there is exactly one counter and exactly one dedupe
 * ring because there is exactly one server. What is left to assert is that
 * the counter really is per-server and really does advance — the property the
 * shared object existed to provide.
 *
 * Asserting only "the values differ" would pass by luck, so this asserts the
 * stronger, fully deterministic property: consecutive subscribes yield
 * CONSECUTIVE extranonce1 values. */
static void test_extranonce1_is_one_sequence_per_server(void) {
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 8, .initial_diff = 1.0 };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");

    stratum_server_t *s = NULL;
    CHECK(stratum_server_start(&cfg, &s) == 0);
    if (!s) return;

    stratum_conn_t *c1, *c2, *c3, *c4;
    unsigned e1 = subscribe_get_en1(s, &c1);
    unsigned e2 = subscribe_get_en1(s, &c2);
    unsigned e3 = subscribe_get_en1(s, &c3);
    unsigned e4 = subscribe_get_en1(s, &c4);

    CHECK(e2 == e1 + 1);
    CHECK(e3 == e1 + 2);
    CHECK(e4 == e1 + 3);

    /* The property all of that exists to guarantee. */
    CHECK(e1 != e2 && e1 != e3 && e1 != e4);
    CHECK(e2 != e3 && e2 != e4 && e3 != e4);

    stratum_conn_free_for_test(c1); stratum_conn_free_for_test(c2);
    stratum_conn_free_for_test(c3); stratum_conn_free_for_test(c4);
    stratum_server_free(s);
    printf("ok: extranonce1 is one sequence for the whole server\n");
}

/* A listener's difficulty policy must reach the connection that arrived on
 * it. Exercised through the accept path's own helper rather than by binding a
 * fixed port, which in CI races whatever else is on the box.
 *
 * The distinction under test is the one the rental port depends on:
 * `min_diff` is what the port PROMISED a marketplace and is recorded
 * separately from the rate-loop bound, because only the promise survives the
 * network-difficulty ceiling. */
static void test_listener_policy_reaches_the_connection(void) {
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 2, .initial_diff = 1.0 };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    CHECK(stratum_server_start(&cfg, &s) == 0);
    if (!s) return;

    stratum_conn_t *c = stratum_conn_new_for_test(s);
    CHECK(c != NULL);
    if (!c) { stratum_server_free(s); return; }

    /* Born on the server-wide default. */
    CHECK(stratum_conn_difficulty_for_test(c) == 1.0);

    /* ⛔ min_diff is deliberately NOT set here. It is a record of operator
     * intent that drives the config warning and the startup log; it is not a
     * connection-level enforcement path, and setting it in a policy test
     * implies otherwise. The floor a listener actually delivers arrives
     * through vardiff_min and initial_diff, which is what parse_listener sets
     * from `min_diff=`. See stratum_listener_t.min_diff. */
    stratum_listener_t pol = { .port = 3335, .initial_diff = 500000.0,
                               .vardiff_min = 500000.0 };
    snprintf(pol.label, sizeof pol.label, "rental");
    stratum_conn_apply_listener_for_test(c, &pol);

    /* A miner that only subscribed still reports its port's difficulty,
     * rather than the default it was constructed with. */
    CHECK(stratum_conn_difficulty_for_test(c) == 500000.0);

    stratum_conn_free_for_test(c);
    stratum_server_free(s);
    printf("ok: a listener's difficulty policy reaches its connections\n");
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
    stratum_server_set_job(s, make_test_job("J1", net), 1);

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

static double hint_huge(void *ctx, const char *worker) {
    (void)ctx; (void)worker; return 17940000.0;
}

/* The reconnect lockout, observed live 2026-08-26: a worker's history says
 * 17.94M (earned when its fleet was bigger), the shrunken fleet reconnects
 * asking 510k, and floor-only request semantics kept it at 17.94M. The
 * client treated the work as invalid and looped through authorize every 2
 * seconds — vardiff never got a share to correct with. At authorize time an
 * explicit request must win over the replayed hint in BOTH directions. */
static void test_request_lowers_replayed_hint_at_authorize(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1,
                           .initial_diff = 500000.0,
                           .vardiff_enabled = 1,
                           .vardiff_target_spm = 12,
                           .vardiff_min = 500000.0,
                           .vardiff_max = 1e12,
                           .vardiff_window_sec = 30,
                           .max_suggested_diff = 1e9,
                           .ctx = &obs, .on_share = on_share,
                           .on_difficulty_hint = hint_huge,
                           .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    CHECK(stratum_server_start(&cfg, &s) == 0);
    if (!s) return;
    /* High network target so the net-diff clamp cannot be the thing that
     * lowers the difficulty — the request must do it, or the test passes
     * for the wrong reason. */
    uint8_t net[32] = {0};
    net[7] = 0xff; net[8] = 0xff;
    stratum_server_set_job(s, make_test_job("J1", net), 1);

    stratum_conn_t *c = stratum_conn_new_for_test(s);
    char *out = NULL; size_t olen = 0;
    stratum_handle_message(s, c, "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}",
                           &out, &olen); free(out); out=NULL; olen=0;
    stratum_handle_message(s, c,
        "{\"id\":2,\"method\":\"mining.suggest_difficulty\",\"params\":[510000]}",
        &out, &olen); free(out); out=NULL; olen=0;
    stratum_handle_message(s, c,
        "{\"id\":3,\"method\":\"mining.authorize\","
         "\"params\":[\"" TEST_ADDR "\",\"x\"]}",
        &out, &olen);
    CHECK(out != NULL);
    /* Told 510000 — the request — not the replayed 17940000. */
    CHECK(strstr(out, "\"params\":[510000]") != NULL);
    CHECK(strstr(out, "\"params\":[17940000]") == NULL);
    free(out);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
    printf("ok: an explicit request lowers a replayed hint at authorize\n");
}

/* Same shape, but the request undercuts the listener floor: the lowering
 * must stop AT the floor — the marketplace was promised that minimum. */
static void test_request_below_floor_lowers_only_to_floor(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1,
                           .initial_diff = 500000.0,
                           .vardiff_enabled = 1,
                           .vardiff_target_spm = 12,
                           .vardiff_min = 500000.0,
                           .vardiff_max = 1e12,
                           .vardiff_window_sec = 30,
                           .max_suggested_diff = 1e9,
                           .ctx = &obs, .on_share = on_share,
                           .on_difficulty_hint = hint_huge,
                           .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    CHECK(stratum_server_start(&cfg, &s) == 0);
    if (!s) return;
    uint8_t net[32] = {0};
    net[7] = 0xff; net[8] = 0xff;
    stratum_server_set_job(s, make_test_job("J1", net), 1);

    stratum_conn_t *c = stratum_conn_new_for_test(s);
    char *out = NULL; size_t olen = 0;
    stratum_handle_message(s, c, "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}",
                           &out, &olen); free(out); out=NULL; olen=0;
    stratum_handle_message(s, c,
        "{\"id\":2,\"method\":\"mining.suggest_difficulty\",\"params\":[1000]}",
        &out, &olen); free(out); out=NULL; olen=0;
    stratum_handle_message(s, c,
        "{\"id\":3,\"method\":\"mining.authorize\","
         "\"params\":[\"" TEST_ADDR "\",\"x\"]}",
        &out, &olen);
    CHECK(out != NULL);
    /* Lowered from 17.94M, but only to the 500k floor — never to 1000. */
    CHECK(strstr(out, "\"params\":[500000]") != NULL);
    CHECK(strstr(out, "\"params\":[17940000]") == NULL);
    CHECK(strstr(out, "\"params\":[1000]") == NULL);
    free(out);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
    printf("ok: request-lowering stops at the listener floor\n");
}

/* ⛔ THE REGRESSION THE ARCHITECTURE SWAP INTRODUCED, and the test that would
 * have caught it.
 *
 * Under the old two-server model the rental port had its OWN stratum_cfg_t, so
 * s->cfg.vardiff_min *was* the rental floor for a rental connection. One server
 * serving every port means a single cfg, and every bound check that reads it
 * directly silently drops the per-port policy.
 *
 * Driven through the replayed-difficulty hint, which is where a returning
 * worker's floor is decided and which needs no mining to exercise. The server-
 * wide floor here is 1 — a home-miner port — and the hint replays 7. If the
 * per-listener floor is honoured the miner is seeded at 500,000; if the code
 * reads s->cfg.vardiff_min instead, it is seeded at 7 and a rented fleet
 * starts three orders of magnitude under what the port advertised.
 *
 * ⚠️ The network target is deliberately HIGH (~16.7M). The network clamp wins
 * over every floor, so an easy target would pull the result back under the
 * floor and report the clamp's work as the floor's.
 * → feedback_tests-that-pass-for-the-wrong-reason */
static void test_listener_floor_beats_the_server_wide_floor(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1,
                           .initial_diff = 1.0,
                           .vardiff_enabled = 1,
                           .vardiff_target_spm = 12,
                           .vardiff_min = 1.0,        /* the HOME-miner floor */
                           .vardiff_max = 1e12,
                           .vardiff_window_sec = 30,
                           .ctx = &obs, .on_share = on_share,
                           .on_difficulty_hint = hint_low,
                           .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    CHECK(stratum_server_start(&cfg, &s) == 0);
    if (!s) return;

    uint8_t net[32] = {0};
    net[7] = 0xff; net[8] = 0xff;
    stratum_server_set_job(s, make_test_job("J1", net), 1);

    stratum_conn_t *c = stratum_conn_new_for_test(s);
    CHECK(c != NULL);
    if (!c) { stratum_server_free(s); return; }

    /* The miner arrived on the rental port. */
    stratum_listener_t rental = { .port = 3335, .initial_diff = 500000.0,
                                  .vardiff_min = 500000.0, .vardiff_max = 1e12 };
    snprintf(rental.label, sizeof rental.label, "rental");
    stratum_conn_apply_listener_for_test(c, &rental);

    char *out = NULL; size_t olen = 0;
    stratum_handle_message(s, c, "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}",
                           &out, &olen); free(out); out=NULL; olen=0;
    stratum_handle_message(s, c,
        "{\"id\":2,\"method\":\"mining.authorize\","
         "\"params\":[\"" TEST_ADDR "\",\"x\"]}",
        &out, &olen);

    CHECK(out != NULL);
    /* The PORT's floor, not the server-wide 1 and not the hint's 7. */
    CHECK(out && strstr(out, "\"params\":[500000]") != NULL);
    CHECK(out && strstr(out, "\"params\":[7]") == NULL);
    CHECK(out && strstr(out, "\"params\":[1]") == NULL);
    free(out);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
    if (g_fail == 0) printf("ok: a listener's floor outranks the server-wide floor\n");
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
    stratum_server_set_job(s, make_test_job("J1", net), 1);

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
    stratum_server_set_job(s, make_test_job("J1", net), 1);

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

/* ---- submit-rate ceiling (max_submits_per_sec) ------------------------ */

/* Same shape as setup_job_diff_conn, plus a ceiling. Kept separate rather
 * than adding a parameter, so every existing caller keeps ceiling=0 and this
 * feature cannot silently change what those tests exercise. */
static stratum_server_t *setup_ceiling_conn(obs_t *obs, int limit,
                                            stratum_conn_t **out_c) {
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1,
                           .initial_diff = 1e-12,
                           .vardiff_enabled = 0,
                           .vardiff_target_spm = 12,
                           .vardiff_min = 0.0,
                           .vardiff_max = 1e15,
                           .vardiff_window_sec = 30,
                           .max_submits_per_sec = limit,
                           .ctx = obs, .on_share = on_share,
                           .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    if (stratum_server_start(&cfg, &s) != 0 || !s) return NULL;
    uint8_t net[32] = {0};
    stratum_server_set_job(s, make_test_job("J1", net), 1);
    stratum_conn_t *c = stratum_conn_new_for_test(s);
    char *out = NULL; size_t olen = 0;
    stratum_handle_message(s, c, "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}",
                           &out, &olen); free(out); out=NULL; olen=0;
    stratum_handle_message(s, c,
        "{\"id\":2,\"method\":\"mining.authorize\",\"params\":[\"" TEST_ADDR "\",\"x\"]}",
        &out, &olen); free(out);
    *out_c = c;
    return s;
}

/* Each submit must carry a DISTINCT nonce. Replaying one share hits the
 * duplicate-share check first, which never reaches the ceiling — the fixture
 * would then measure dedupe and report it as rate limiting.
 * → feedback_tests-that-pass-for-the-wrong-reason */
static void ceil_submit(stratum_server_t *s, stratum_conn_t *c, int n,
                        char **out, size_t *olen) {
    char msg[256];
    snprintf(msg, sizeof msg,
             "{\"id\":9,\"method\":\"mining.submit\","
             "\"params\":[\"w\",\"J1\",\"" TEST_EN2 "\",\"60000000\",\"%08x\"]}",
             (unsigned)n + 1);
    stratum_handle_message(s, c, msg, out, olen);
}

/* INC-002: one source pushed ~11k diff-1 submits/sec per worker. Past the
 * ceiling a submit must be refused BEFORE any hashing, and the refusal must
 * not write one reject row per refused share — that is what put ~1.95M rows
 * in the live database in an hour. */
static void test_submit_ceiling_refuses_past_the_limit(void) {
    obs_t obs = {0};
    stratum_conn_t *c = NULL;
    stratum_server_t *s = setup_ceiling_conn(&obs, 3, &c);
    CHECK(s != NULL);
    if (!s) return;

    int accepted = 0, refused = 0;
    for (int i = 0; i < 50; i++) {
        char *out = NULL; size_t olen = 0;
        ceil_submit(s, c, i, &out, &olen);
        if (out && strstr(out, "submitting too fast")) refused++;
        else accepted++;
        free(out);
    }
    /* Exactly the ceiling gets through in the window; the rest are refused. */
    CHECK(accepted == 3);
    CHECK(refused == 47);
    /* The share observer never saw the refused ones — they cost no hashing. */
    CHECK(obs.shares == 3);
    /* And 47 refusals produced at most ONE reject row, not 47. */
    CHECK(obs.rejects <= 1);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
    printf("ok: the submit ceiling refuses past the limit, cheaply and quietly\n");
}

/* The ceiling is per second, not per connection lifetime: once the window
 * rolls the allowance comes back. Without this a legitimate miner would be
 * silenced permanently after one burst. */
static void test_submit_ceiling_window_rolls(void) {
    obs_t obs = {0};
    stratum_conn_t *c = NULL;
    stratum_server_t *s = setup_ceiling_conn(&obs, 2, &c);
    CHECK(s != NULL);
    if (!s) return;

    for (int i = 0; i < 5; i++) {
        char *out = NULL; size_t olen = 0;
        ceil_submit(s, c, i, &out, &olen); free(out);
    }
    CHECK(obs.shares == 2);
    /* Roll past the one-second window. */
    struct timespec ts = { .tv_sec = 1, .tv_nsec = 50000000L };
    nanosleep(&ts, NULL);
    char *out = NULL; size_t olen = 0;
    ceil_submit(s, c, 99, &out, &olen);
    CHECK(out != NULL && strstr(out, "submitting too fast") == NULL);
    free(out);
    CHECK(obs.shares == 3);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
    printf("ok: the submit ceiling window rolls each second\n");
}

/* 0 disables the ceiling — the shipped default, and what every other test in
 * this file runs with. Asserting it here means the feature cannot start
 * refusing work for operators who never configured it. */
static void test_submit_ceiling_zero_disables(void) {
    obs_t obs = {0};
    stratum_conn_t *c = NULL;
    stratum_server_t *s = setup_ceiling_conn(&obs, 0, &c);
    CHECK(s != NULL);
    if (!s) return;
    for (int i = 0; i < 20; i++) {
        char *out = NULL; size_t olen = 0;
        ceil_submit(s, c, i, &out, &olen);
        CHECK(out != NULL && strstr(out, "submitting too fast") == NULL);
        free(out);
    }
    CHECK(obs.shares == 20);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
    printf("ok: max_submits_per_sec=0 disables the ceiling\n");
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
    stratum_server_set_job(s, make_test_job("J2", net), 1);

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

/* Read one mining.notify off `fd` and return its clean_jobs flag (-1 if no
 * notify arrived). The flag is params[8]; reading it back off a real socket is
 * the only way to prove what a miner actually receives, since the broadcast
 * loop skips any connection without a live fd. */
static int read_notify_clean_flag(int fd) {
    char buf[65536];
    ssize_t n = recv(fd, buf, sizeof buf - 1, MSG_DONTWAIT);
    if (n <= 0) return -1;
    buf[n] = '\0';
    int flag = -1;
    for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        cJSON *m = cJSON_Parse(line);
        if (!m) continue;
        cJSON *meth = cJSON_GetObjectItem(m, "method");
        if (cJSON_IsString(meth) && strcmp(meth->valuestring, "mining.notify") == 0) {
            cJSON *pr = cJSON_GetObjectItem(m, "params");
            cJSON *cj = cJSON_GetArrayItem(pr, 8);
            if (cJSON_IsBool(cj)) flag = cJSON_IsTrue(cj) ? 1 : 0;
        }
        cJSON_Delete(m);
    }
    return flag;
}

/* clean_jobs must reflect whether work in progress became worthless.
 *
 * A new block invalidates everything a miner is hashing, so the flag is true.
 * A same-tip refresh only adds transactions and moves ntime — the miner's
 * current job is still valid work, and flagging it clean throws away the whole
 * fleet's partial progress. Setting it unconditionally is what gets a pool
 * dropped by hashrate marketplaces, whose proxies flush their fleet on it. */
static void test_clean_jobs_only_on_a_new_block(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 2, .initial_diff = 1.0,
                           .ctx = &obs, .on_share = on_share,
                           .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);

    uint8_t net[32]; memset(net, 0xff, 32);
    stratum_server_set_job(s, make_test_job("J0", net), 1);

    int fds[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    stratum_conn_t *c = stratum_conn_new_for_test(s);
    char *out = NULL; size_t olen = 0;
    stratum_handle_message(s, c,
        "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}",
        &out, &olen); free(out); out = NULL; olen = 0;
    stratum_handle_message(s, c,
        "{\"id\":2,\"method\":\"mining.authorize\","
         "\"params\":[\"" TEST_ADDR "\",\"x\"]}",
        &out, &olen); free(out);

    /* Preconditions: the broadcast loop skips a conn that is not subscribed,
     * authorized and holding a live fd, and then this test would assert
     * against a notify that was never sent. */
    CHECK(stratum_conn_authorized_for_test(c) == 1);
    CHECK(stratum_conn_subscribed_for_test(c) == 1);
    stratum_conn_register_for_test(s, c, fds[0]);

    /* Drain anything the handshake left on the wire so the next read can only
     * see the broadcast under test. */
    (void)read_notify_clean_flag(fds[1]);

    /* A same-tip refresh: still valid work, so do NOT flush. */
    stratum_server_set_job(s, make_test_job("J1", net), 0);
    int refresh_flag = read_notify_clean_flag(fds[1]);
    CHECK(refresh_flag == 0);

    /* A new block: everything in flight is now an orphan, so flush. */
    stratum_server_set_job(s, make_test_job("J2", net), 1);
    int newblock_flag = read_notify_clean_flag(fds[1]);
    CHECK(newblock_flag == 1);

    close(fds[0]);
    close(fds[1]);
    stratum_server_free(s);
    stratum_conn_free_for_test(c);
}

/* A submit that is refused before it can become a share still counts as a
 * reject on the miner's own dashboard, so it has to be recorded here too —
 * otherwise a miner refused on every submit is indistinguishable from one
 * that never connected. */
static void test_unauthorized_submit_is_recorded_as_a_reject(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1, .initial_diff = 1.0,
                           .ctx = &obs, .on_share = on_share,
                           .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);

    stratum_conn_t *c = stratum_conn_new_for_test(s);
    char *out = NULL; size_t olen = 0;
    /* Subscribed but deliberately NOT authorized. */
    stratum_handle_message(s, c,
        "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}",
        &out, &olen); free(out); out = NULL; olen = 0;
    CHECK(stratum_conn_authorized_for_test(c) == 0);

    CHECK(obs.rejects == 0);
    stratum_handle_message(s, c,
        "{\"id\":2,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J1\",\"" TEST_EN2 "\",\"60000000\",\"00000001\"]}",
        &out, &olen); free(out); out = NULL; olen = 0;
    CHECK(obs.rejects == 1);

    /* Malformed params are the other path that used to answer the miner with
     * an error and record nothing. */
    stratum_handle_message(s, c,
        "{\"id\":3,\"method\":\"mining.submit\",\"params\":[\"w\"]}",
        &out, &olen); free(out);
    CHECK(obs.rejects == 2);

    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

/* "stale or unknown job" was one counter over three unrelated events, which
 * is why the acceptance window could never be argued about from the reject
 * table: a job the pool retired at 60 s and a garbage id from a miner that
 * had never been issued anything looked identical. The classifier is pure, so
 * it is tested directly rather than through a server. */
static void test_job_id_classification_is_three_way(void) {
    const uint64_t start = 1000000000000ull;   /* server start, wall-clock ms */
    const uint64_t now   = start + 500000ull;
    int64_t age = 0;
    char id[64];

    /* Nothing that could have come out of our own snprintf. */
    CHECK(strcmp(stratum_classify_job_id(start, now, NULL, &age),
                 STRATUM_REJECT_KIND_NEVER_ISSUED) == 0);
    CHECK(age == STRATUM_JOB_AGE_NONE);
    CHECK(strcmp(stratum_classify_job_id(start, now, "", &age),
                 STRATUM_REJECT_KIND_NEVER_ISSUED) == 0);
    CHECK(strcmp(stratum_classify_job_id(start, now, "NOPE", &age),
                 STRATUM_REJECT_KIND_NEVER_ISSUED) == 0);
    CHECK(strcmp(stratum_classify_job_id(start, now, "-5", &age),
                 STRATUM_REJECT_KIND_NEVER_ISSUED) == 0);
    CHECK(strcmp(stratum_classify_job_id(start, now, "abc-", &age),
                 STRATUM_REJECT_KIND_NEVER_ISSUED) == 0);
    /* The sequence half must be hex too, or "1a-hello" would be read as a
     * timestamp and reported with an age it never had. */
    CHECK(strcmp(stratum_classify_job_id(start, now, "1a-hello", &age),
                 STRATUM_REJECT_KIND_NEVER_ISSUED) == 0);
    /* Long enough to overflow the accumulator: refused, not wrapped into a
     * plausible timestamp. */
    CHECK(strcmp(stratum_classify_job_id(start, now,
                                         "ffffffffffffffffff-1", &age),
                 STRATUM_REJECT_KIND_NEVER_ISSUED) == 0);

    /* A job this run issued and then retired — the case the retention window
     * is actually about, and the only one that carries a real age. */
    snprintf(id, sizeof id, "%llx-%lx", (unsigned long long)(start + 1000ull), 7ul);
    CHECK(strcmp(stratum_classify_job_id(start, now, id, &age),
                 STRATUM_REJECT_KIND_EVICTED) == 0);
    CHECK(age == (int64_t)(now - start - 1000ull));

    /* Well-formed, but minted before this process existed: a previous
     * instance issued it, so "we evicted it" would be a false statement about
     * our own retention. */
    snprintf(id, sizeof id, "%llx-%lx", (unsigned long long)(start - 1ull), 1ul);
    CHECK(strcmp(stratum_classify_job_id(start, now, id, &age),
                 STRATUM_REJECT_KIND_PRE_RESTART) == 0);
    CHECK(age == STRATUM_JOB_AGE_NONE);

    /* Beyond any plausible clock skew ahead of us: not ours. */
    snprintf(id, sizeof id, "%llx-%lx", (unsigned long long)(now + 600000ull), 1ul);
    CHECK(strcmp(stratum_classify_job_id(start, now, id, &age),
                 STRATUM_REJECT_KIND_NEVER_ISSUED) == 0);

    /* Slightly ahead — a wall-clock step between issue and submit. The
     * negative age is passed through DELIBERATELY: clamping it to zero would
     * file a clock artefact as a fresh job and quietly bias the very
     * distribution this column exists to measure. */
    snprintf(id, sizeof id, "%llx-%lx", (unsigned long long)(now + 1000ull), 1ul);
    CHECK(strcmp(stratum_classify_job_id(start, now, id, &age),
                 STRATUM_REJECT_KIND_EVICTED) == 0);
    CHECK(age == -1000);
    CHECK(age != STRATUM_JOB_AGE_NONE);   /* a real measurement, not "absent" */
    printf("ok: job id classification splits evicted / pre-restart / never issued\n");
}

/* End to end through the submit path: a well-formed id for a job the server
 * does not hold comes back with an age, not merely a reason string. */
static void test_stale_reject_carries_a_job_age(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1, .initial_diff = 1.0,
                           .ctx = &obs, .on_share = on_share,
                           .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);

    uint8_t net[32] = {0};
    stratum_server_set_job(s, make_test_job("J1", net), 1);

    stratum_conn_t *c = stratum_conn_new_for_test(s);
    char *out = NULL; size_t olen = 0;
    stratum_handle_message(s, c, "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}",
                           &out, &olen); free(out); out=NULL; olen=0;
    stratum_handle_message(s, c,
        "{\"id\":2,\"method\":\"mining.authorize\","
         "\"params\":[\"" TEST_ADDR "\",\"x\"]}",
        &out, &olen); free(out); out=NULL; olen=0;

    /* Shaped exactly like main.c mints them, stamped now: this run issued it
     * as far as the classifier can tell, and the server no longer has it. */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t now_ms_wall = (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
    char line[256];
    snprintf(line, sizeof line,
             "{\"id\":3,\"method\":\"mining.submit\","
             "\"params\":[\"w\",\"%llx-%lx\",\"" TEST_EN2 "\","
             "\"60000000\",\"00000000\"]}",
             (unsigned long long)now_ms_wall, 3ul);
    stratum_handle_message(s, c, line, &out, &olen); free(out); out=NULL;

    CHECK(obs.rejects == 1);
    CHECK(strstr(obs.last_reason, "stale") != NULL);
    CHECK(strcmp(obs.last_kind, STRATUM_REJECT_KIND_EVICTED) == 0);
    /* An age, and a sane one — the point of the column is that this number
     * exists at all, since without it "would a longer window have saved this
     * share?" cannot be answered from the data. */
    CHECK(obs.last_job_age_ms >= 0);
    CHECK(obs.last_job_age_ms < 60000);

    stratum_conn_free_for_test(c);
    stratum_server_free(s);
    printf("ok: a stale reject carries the age of the job it named\n");
}

/* The reject record has to carry the CONNECTION, not just the worker name: a
 * submit refused before authorize has no worker at all, which is how a
 * 10,776-share burst over 08-28/29 stayed unattributable to anything. Driven
 * over a real socket on purpose — a hand-made test connection has no peer to
 * report, so an in-process test would pass on a build that never populated
 * the field. */
static void test_reject_records_the_peer_ip(void) {
    obs_t obs = {0};
    /* Walks a port range for the reason start_on documents: a fixed port makes
     * the suite flaky, and 39341 sat inside the dual-stack tests' own walk
     * ranges — a collision needing no other process at all. */
    int port = 39400;
    stratum_server_t *s = start_on_obs("127.0.0.1", &port, &obs);
    if (!s) { printf("skip: reject records peer ip (no free port)\n"); return; }

    int cfd = dial(AF_INET, port);
    CHECK(cfd >= 0);
    if (cfd < 0) { stratum_server_stop(s); stratum_server_free(s); return; }

    /* Submit without authorizing — the unattributable case itself. */
    const char *msg =
        "{\"id\":1,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J1\",\"" TEST_EN2 "\",\"60000000\",\"00000001\"]}\n";
    CHECK(write(cfd, msg, strlen(msg)) == (ssize_t)strlen(msg));

    for (int i = 0; i < 500; ++i) {
        pthread_mutex_lock(&obs_mu);
        int done = obs.rejects > 0;
        pthread_mutex_unlock(&obs_mu);
        if (done) break;
        sleep_ms(2);
    }
    pthread_mutex_lock(&obs_mu);
    CHECK(obs.rejects == 1);
    CHECK(strstr(obs.last_reason, "unauthorized") != NULL);
    CHECK(strcmp(obs.last_peer_ip, "127.0.0.1") == 0);
    /* Not a job lookup, so no kind and no age — and they must read as absent
     * rather than as zero. */
    CHECK(obs.last_kind[0] == '\0');
    CHECK(obs.last_job_age_ms == STRATUM_JOB_AGE_NONE);
    pthread_mutex_unlock(&obs_mu);

    close(cfd);
    stratum_server_stop(s);
    stratum_server_free(s);
    printf("ok: a reject records the peer ip it arrived from\n");
}

/* A candidate the node refuses is reported as not accepted, with the node's
 * reason. Before this the submitblock result was discarded and the candidate
 * was recorded as a found block regardless — on a low-difficulty chain that
 * is nearly every candidate, and every one of them credited the pool with a
 * reward that never existed. */
static void test_rejected_candidate_is_not_a_block(void) {
    obs_t obs = {0};
    obs.submit_rejects = 1;
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1,
                           .initial_diff = 1e12,
                           .ctx = &obs, .on_share = on_share,
                           .on_reject = on_reject, .on_block = on_block,
                           .on_block_found = on_block_found };
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
    stratum_server_set_job(s, make_test_job("J1", net), 1);

    int rc = stratum_handle_message(s, c,
        "{\"id\":3,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J1\",\"" TEST_EN2 "\",\"60000000\",\"00000001\"]}",
        &out, &olen);
    CHECK(rc == 0);
    CHECK(obs.submits == 1);          /* it was offered to the node */
    CHECK(obs.found_calls == 1);      /* and still reported, not dropped */
    CHECK(obs.last_accepted == 0);    /* but not as an accepted block */
    CHECK(strstr(obs.last_submit_error, "inconclusive") != NULL);
    /* The share itself is untouched: the miner did the work, and in
     * pps-classic absorbing this variance is exactly what the pool is for. */
    CHECK(obs.rejects == 0);
    CHECK(obs.shares == 1);
    free(out);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

/* The accepting path still reports accepted, with no error text — a
 * candidate the node took is 'pending', never 'rejected'. */
static void test_accepted_candidate_reports_accepted(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1,
                           .initial_diff = 1e12,
                           .ctx = &obs, .on_share = on_share,
                           .on_reject = on_reject, .on_block = on_block,
                           .on_block_found = on_block_found };
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
    stratum_server_set_job(s, make_test_job("J1", net), 1);

    stratum_handle_message(s, c,
        "{\"id\":3,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J1\",\"" TEST_EN2 "\",\"60000000\",\"00000001\"]}",
        &out, &olen);
    CHECK(obs.found_calls == 1);
    CHECK(obs.last_accepted == 1);
    CHECK(obs.last_submit_error[0] == '\0');
    free(out);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

/* A job found by a submit must stay valid for that submit, even though the
 * tip watcher retires and frees jobs on another thread at every new template.
 *
 * This is not hypothetical. find_job used to hand back a borrowed pointer
 * after releasing its lock, and the production pps pool recorded blocks_found
 * rows carrying a freed job's fields — heights 0, 2 and 550 on a chain mining
 * at 963,000+, and two rewards of 1.29 million BTC. A freed job's
 * network_target_be can also make an ordinary hash look like a solved block,
 * which is how those rows came to exist at all.
 *
 * Ring turnover is the trigger: on a chain serving several templates a second
 * the ring recycles in seconds, so a submit only has to be slightly slow to be
 * reading freed memory. Here it is forced deterministically — under ASan this
 * aborts without the reference count.
 *
 * ⚠️ THE PUSH COUNT IS DERIVED FROM STRATUM_RECENT_JOBS, NOT HARDCODED. It was
 * 24 against a ring of 8. When the ring went 8 → 16 on 2026-08-30 that margin
 * fell from 3× to 1.5× and the next raise would have pushed fewer jobs than the
 * ring holds — HELD would still be findable, `gone == NULL` would fail, and had
 * the assertion been the other way round the test would have passed while
 * exercising nothing. A fixture whose validity depends on a constant it does
 * not reference decays silently. → feedback_fixture-invariants-decay-at-scale */
static void test_job_survives_retirement_while_held(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1, .initial_diff = 1.0,
                          .ctx = &obs, .on_share = on_share,
                          .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);

    uint8_t net[32]; memset(net, 0xff, 32);
    stratum_server_set_job(s, make_test_job("HELD", net), 1);

    /* What handle_submit does: take the job, then do slow work with it. */
    stratum_job_t *held = stratum_job_find_for_test(s, "HELD");
    CHECK(held != NULL);
    CHECK(stratum_job_height_for_test(held) == 800000);
    CHECK(stratum_job_value_sats_for_test(held) == 5000000000LL);

    /* Meanwhile the tip watcher churns through enough templates to push HELD
     * out of the retention ring entirely and free it. */
    for (int i = 0; i < (int)(STRATUM_RECENT_JOBS * 3); ++i) {
        char jid[16];
        snprintf(jid, sizeof jid, "J%d", i);
        stratum_server_set_job(s, make_test_job(jid, net), 1);
    }

    /* It is gone from the lookup — correct, a later submit for it is stale. */
    stratum_job_t *gone = stratum_job_find_for_test(s, "HELD");
    CHECK(gone == NULL);

    /* But the holder's copy is still intact, not recycled memory. These are
     * exactly the two fields that reached blocks_found as garbage. */
    CHECK(stratum_job_height_for_test(held) == 800000);
    CHECK(stratum_job_value_sats_for_test(held) == 5000000000LL);

    stratum_job_free(held);
    stratum_server_free(s);
}


/* 🔴 Shares mined under a PREVIOUS difficulty must not drive vardiff.
 *
 * A retarget sends mining.set_difficulty without re-notifying, so after a
 * downward step the miner keeps returning shares for jobs issued at the old,
 * higher difficulty. Counting those as evidence about the NEW difficulty reads
 * as a rate spike and steps difficulty back up -- then the next window reads
 * low and it steps down, forever. Measured in production 2026-08-30: 16 of 98
 * miners cycling, one connection reading 67 spm where ~15 was the truth.
 *
 * The twin below is the whole test: the two halves differ in exactly one
 * thing -- whether the connection's difficulty changed after the job went out.
 * Same job, same nonces, same submit timing. */
static void vardiff_stale_diff_case(int change_difficulty_midway,
                                    double *out_first_setdiff,
                                    double *out_final_diff) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1,
                          .initial_diff = 1e-9,
                          .vardiff_enabled = 1,
                          .vardiff_target_spm = 60.0,
                          .vardiff_min = 1e-12,
                          .vardiff_max = 1e15,
                          .vardiff_window_sec = 1,
                          .vardiff_min_samples = 2,
                          .vardiff_max_window_mult = 1,
                          .ctx = &obs, .on_share = on_share,
                          .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);
    uint8_t net[32] = {0};
    stratum_server_set_job(s, make_test_job("J1", net), 1);
    stratum_conn_t *c = stratum_conn_new_for_test(s);
    handshake(s, c);

    /* Mine five shares against J1, which went out at the connection's
     * difficulty of 1e-9. */
    const int N = 6;   /* 5 to fill the window + 1 FRESH one to trigger evaluation */
    uint32_t nonce[6];
    uint32_t from = 1;
    for (int i = 0; i < N; ++i) {
        double md = mine_nonce(s, c, "J1", TEST_EN2, 1e-9, HUGE_VAL, from, &nonce[i]);
        if (md <= 0.0) {
            printf("DEBUG: mine_nonce failed at i=%d (diff=%g, conn diff=%g)\n",
                   i, md, stratum_conn_difficulty_for_test(c));
            stratum_conn_free_for_test(c); stratum_server_free(s);
            *out_first_setdiff = -2.0; *out_final_diff = -2.0; return;
        }
        from = nonce[i] + 1;
    }

    /* THE ONE DIFFERENCE. Model a downward retarget having happened after J1
     * was issued: the connection is now on 2e-9 while J1's recorded difficulty
     * is still 1e-9, so every share below is a stale-difficulty share. */
    if (change_difficulty_midway) {
        stratum_conn_force_difficulty_for_test(c, 2e-9, 1e-9);
    }
    /* ⛔ NOT rearm_vardiff_window(): that helper re-sends mining.authorize,
     * which emits a fresh notify and RE-RECORDS J1's difficulty at whatever is
     * current. It would overwrite the very mismatch this test exists to create,
     * making judge_diff equal c->difficulty again -- the test then passes or
     * fails for a reason that has nothing to do with the fix. Cost me a real
     * debugging detour; use the direct helper, which only resets the window. */
    stratum_conn_rearm_vardiff_for_test(c);

    char *out = NULL; size_t olen = 0;
    double first = -1.0;
    for (int i = 0; i < N - 1; ++i) {
        submit_nonce(s, c, TEST_EN2, nonce[i], &out, &olen);
        double d = set_diff_value(out);
        if (d > 0.0 && first < 0.0) first = d;
        free(out); out = NULL; olen = 0;
    }
    /* Past the window, then one more submit to trigger evaluation. */
    sleep_ms(1100);
    submit_nonce(s, c, TEST_EN2, nonce[N - 1], &out, &olen);   /* fresh, not a repeat */
    double d = set_diff_value(out);
    if (d > 0.0 && first < 0.0) first = d;
    free(out);

    *out_first_setdiff = first;
    *out_final_diff = stratum_conn_difficulty_for_test(c);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

static void test_stale_difficulty_shares_do_not_drive_vardiff(void) {
    double ctl_set = 0.0, ctl_final = 0.0, stale_set = 0.0, stale_final = 0.0;
    vardiff_stale_diff_case(0, &ctl_set, &ctl_final);
    vardiff_stale_diff_case(1, &stale_set, &stale_final);

    /* ⛔ -2.0 means the PROBE failed to mine, -1.0 means no set_difficulty was
     * emitted. They must not share a sentinel: "no retarget" is the SUCCESS
     * condition here, so folding it in with instrument failure and returning
     * early would make a correct fix assert nothing at all, silently, while
     * still counting as a pass. (claude-11 found exactly that hole.) */
    CHECK(ctl_set > -2.0 && stale_set > -2.0);

    /* Control: shares matching the current difficulty ARE rate evidence, and
     * five in a 1s window against a 60 spm target is a spike -- vardiff must
     * raise. Load-bearing: without it, a test where neither half retargets
     * looks identical to a working fix. */
    CHECK(ctl_set > 1e-9);
    CHECK(ctl_final > 1e-9);

    /* 🔴 THE FIX, ASSERTED AS A NUMBER, NOT AS AN INEQUALITY. The earlier
     * version checked only !(x > 2e-9), which a DOWN-step satisfies -- and a
     * down-step is precisely the runaway the drain guard exists to stop. That
     * assertion passed while printing the failure. The difficulty must be
     * exactly where it was forced, moved neither up nor down. */
    CHECK(stale_final == 2e-9);
    CHECK(stale_set < 0.0);
    printf("ok: stale-difficulty shares neither raise nor lower vardiff "
           "(control raised %g->%g; stale held at %g)\n",
           1e-9, ctl_final, stale_final);
}


/* 🔴 MULTI-CYCLE: the guard must CLEAN UP after itself, not just defer.
 *
 * The single-cycle twin above cannot tell "reset the window state" from "never
 * reset it" -- it ends after one drain. Three mutations survived it, all inside
 * the guard body (claude-11):
 *
 *   - dropping `vd_window_stale_diff_shares = 0` lets the counter accumulate
 *     for the life of the connection, so after ANY drain every later empty
 *     window still sees stale > 0, matches the guard, and a genuinely idle
 *     miner never takes its idle step again. Permanent starvation, reachable by
 *     deleting one line.
 *   - dropping `vd_window_start_ms = now` leaves drain dead-time in `elapsed`,
 *     so the next measurement reads low and steps DOWN -- an attenuated version
 *     of the runaway the guard exists to stop.
 *   - dropping `vd_window_min_achieved = HUGE_VAL` leaks a drain window's
 *     minimum into the next floor check, which raises UNCAPPED.
 *
 * This test runs drain -> idle and drain -> burst, which is what makes those
 * three visible. */
static void test_drain_guard_cleans_up_after_itself(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1,
                          .initial_diff = 1e-9,
                          .vardiff_enabled = 1,
                          .vardiff_target_spm = 60.0,
                          .vardiff_min = 1e-14,
                          .vardiff_max = 1e15,
                          .vardiff_window_sec = 1,
                          .vardiff_min_samples = 2,
                          .vardiff_max_window_mult = 1,
                          .ctx = &obs, .on_share = on_share,
                          .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);
    uint8_t net[32] = {0};
    stratum_server_set_job(s, make_test_job("J1", net), 1);
    stratum_conn_t *c = stratum_conn_new_for_test(s);
    handshake(s, c);
    /* ⛔ The idle path is only reached from the JOB BROADCAST loop, which walks
     * registered connections. An unregistered test conn never sees a broadcast,
     * so vardiff_check_idle never runs and the idle step silently cannot fire --
     * the test would then "pass" a mutation it cannot observe. */
    int fds[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    CHECK(stratum_conn_authorized_for_test(c) == 1);
    CHECK(stratum_conn_subscribed_for_test(c) == 1);
    stratum_conn_register_for_test(s, c, fds[0]);

    uint32_t n[4]; uint32_t from = 1;
    for (int i = 0; i < 4; ++i) {
        if (mine_nonce(s, c, "J1", TEST_EN2, 1e-9, HUGE_VAL, from, &n[i]) <= 0.0) {
            stratum_conn_free_for_test(c); stratum_server_free(s); return;
        }
        from = n[i] + 1;
    }

    /* PHASE 1 -- drain. J1 was issued at 1e-9; move the connection to 2e-9 so
     * every J1 share is stale. The guard must defer. */
    stratum_conn_force_difficulty_for_test(c, 2e-9, 1e-9);
    stratum_conn_rearm_vardiff_for_test(c);
    char *out = NULL; size_t olen = 0;
    for (int i = 0; i < 3; ++i) {
        submit_nonce(s, c, TEST_EN2, n[i], &out, &olen);
        free(out); out = NULL; olen = 0;
    }
    sleep_ms(1100);
    submit_nonce(s, c, TEST_EN2, n[3], &out, &olen);
    CHECK(set_diff_value(out) < 0.0);          /* deferred, as the twin shows */
    free(out); out = NULL; olen = 0;
    CHECK(stratum_conn_difficulty_for_test(c) == 2e-9);

    /* ⚠️ NOT COVERED HERE, and stated rather than left implicit: two lines in
     * the guard body have no test.
     *
     *   - `vd_window_start_ms = now` (the restart). Removing it leaves drain
     *     dead-time in `elapsed`, so the next measurement reads low and steps
     *     DOWN. Catching it needs a post-drain BURST whose rate depends on the
     *     window length -- I could not build one that was not perturbed by the
     *     idle check that set_job's own broadcast runs first.
     *   - `vd_window_min_achieved = HUGE_VAL`. Removing it leaks a drain
     *     window's minimum into the next floor check, which raises uncapped.
     *
     * Both survive mutation today (claude-11 confirmed). They are reasoned, not
     * proven, and that is a real gap in a change that moves every miner's
     * difficulty -- not a formality. Anyone extending this test should start
     * there. */

    /* PHASE 3 -- the miner goes quiet. The idle path must still work. If the
     * guard did not clear its stale counter, this window still matches it and
     * NO idle step ever fires again. */
    double before_idle = stratum_conn_difficulty_for_test(c);
    sleep_ms(1100);
    stratum_server_set_job(s, make_test_job("J2", net), 0);
    double after_idle = stratum_conn_difficulty_for_test(c);
    CHECK(after_idle < before_idle);
    printf("ok: drain guard clears its state -- idle step fired after a drain "
           "(%g -> %g)\n", before_idle, after_idle);

    /* ⛔ Do NOT stratum_conn_free_for_test(c) here. This connection was
     * REGISTERED, so the server still holds it on s->conns; freeing it first
     * makes stratum_server_free walk a dangling pointer. ASan caught exactly
     * that (heap-use-after-free at stratum.c:3263 in stratum_server_stop) --
     * the plain suite passed it silently, which is the whole reason this
     * target exists. server_free owns a registered connection's lifetime. */
    close(fds[0]); close(fds[1]);
    stratum_server_free(s);
    stratum_conn_free_for_test(c);
}

/* The reference must not leak either: once the holder lets go, the job is
 * destroyed rather than pinned for the process's lifetime. Run under ASan or
 * valgrind, a leak here is the failure. */
static void test_held_job_is_freed_on_release(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1, .initial_diff = 1.0,
                          .ctx = &obs, .on_share = on_share,
                          .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);

    uint8_t net[32]; memset(net, 0xff, 32);
    stratum_server_set_job(s, make_test_job("A", net), 1);
    stratum_job_t *held = stratum_job_find_for_test(s, "A");
    CHECK(held != NULL);
    stratum_job_free(held);        /* holder done; server still owns one */
    stratum_server_free(s);        /* server drops the last one */
}

/* While accrual is suspended the pool must turn miners away, not bank their
 * work. A miner whose shares are accepted but never credited is mining for
 * free without being told — worse than being refused, because they cannot
 * tell it is happening. */
static void test_gated_pps_refuses_authorize_and_submits(void) {
    obs_t obs = {0};
    _Atomic int gate = 1;
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1, .initial_diff = 1.0,
                          .pps_enabled = 1, .pps_gate = &gate,
                          .pps_refuse_shares_below_min = 1,
                          .ctx = &obs, .on_share = on_share,
                          .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    snprintf(cfg.pool_btc_address, sizeof cfg.pool_btc_address, "%s", TEST_ADDR);
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);

    stratum_conn_t *c = stratum_conn_new_for_test(s);
    char *out = NULL; size_t olen = 0;
    stratum_handle_message(s, c, "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}",
                           &out, &olen); free(out); out=NULL; olen=0;

    /* Authorize is refused, and the reason says what to do about it. */
    int rc = stratum_handle_message(s, c,
        "{\"id\":2,\"method\":\"mining.authorize\","
         "\"params\":[\"2sYBNmMJMMZHi6xasMcCPgNiYJ1z\",\"x\"]}",
        &out, &olen);
    CHECK(rc == 0);
    CHECK(strstr(out, "not crediting shares") != NULL);
    CHECK(stratum_conn_authorized_for_test(c) == 0);
    free(out); out=NULL; olen=0;

    /* A miner that got in before the gate closed is stopped too. */
    gate = 0;
    stratum_handle_message(s, c,
        "{\"id\":3,\"method\":\"mining.authorize\","
         "\"params\":[\"2sYBNmMJMMZHi6xasMcCPgNiYJ1z\",\"x\"]}",
        &out, &olen);
    CHECK(stratum_conn_authorized_for_test(c) == 1);
    free(out); out=NULL; olen=0;

    uint8_t net[32]; memset(net, 0xff, 32);
    stratum_server_set_job(s, make_test_job("J1", net), 1);
    gate = 1;
    stratum_handle_message(s, c,
        "{\"id\":4,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J1\",\"" TEST_EN2 "\",\"60000000\",\"00000001\"]}",
        &out, &olen);
    CHECK(strstr(out, "not crediting shares") != NULL);
    CHECK(obs.shares == 0);            /* nothing banked */
    free(out); out=NULL; olen=0;

    /* And once the chain retargets, work is taken again. */
    gate = 0;
    stratum_handle_message(s, c,
        "{\"id\":5,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J1\",\"" TEST_EN2 "\",\"60000000\",\"00000002\"]}",
        &out, &olen);
    CHECK(obs.shares == 1);
    free(out);

    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

/* An operator who has deliberately turned the refusal off keeps taking work. */
static void test_gate_can_be_disabled(void) {
    obs_t obs = {0};
    _Atomic int gate = 1;
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1, .initial_diff = 1.0,
                          .pps_enabled = 1, .pps_gate = &gate,
                          .pps_refuse_shares_below_min = 0,
                          .ctx = &obs, .on_share = on_share,
                          .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    snprintf(cfg.pool_btc_address, sizeof cfg.pool_btc_address, "%s", TEST_ADDR);
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);
    stratum_conn_t *c = stratum_conn_new_for_test(s);
    char *out = NULL; size_t olen = 0;
    stratum_handle_message(s, c, "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}",
                           &out, &olen); free(out); out=NULL; olen=0;
    stratum_handle_message(s, c,
        "{\"id\":2,\"method\":\"mining.authorize\","
         "\"params\":[\"2sYBNmMJMMZHi6xasMcCPgNiYJ1z\",\"x\"]}",
        &out, &olen);
    CHECK(stratum_conn_authorized_for_test(c) == 1);
    free(out);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

/* Solo has no accrual to suspend, so the gate must never touch it. */
static void test_solo_is_never_gated(void) {
    obs_t obs = {0};
    _Atomic int gate = 1;
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1, .initial_diff = 1.0,
                          .pps_enabled = 0, .pps_gate = &gate,
                          .pps_refuse_shares_below_min = 1,
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
         "\"params\":[\"" TEST_ADDR "\",\"x\"]}", &out, &olen);
    CHECK(stratum_conn_authorized_for_test(c) == 1);
    free(out);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}


/* ---- sd= : static difficulty --------------------------------------------
 *
 * A pin takes a connection out of vardiff entirely. Every test below asserts
 * the NUMBER, never a range: a wrong clamp switches the guard off while the
 * source still reads correct. */

static stratum_server_t *sd_server_min(obs_t *obs, int enabled, int submit_ceiling,
                                       double vd_min, int sd_min) {
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 2,
                          .initial_diff = 100000.0,
                          .vardiff_enabled = 1,
                          .vardiff_target_spm = 12,
                          .vardiff_min = vd_min,
                          .vardiff_max = 1e12,
                          .vardiff_window_sec = 30,
                          .max_suggested_diff = 1e9,
                          .max_submits_per_sec = submit_ceiling,
                          .static_diff_enabled = enabled,
                          .static_diff_min = sd_min,
                          .ctx = obs, .on_share = on_share,
                          .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    if (stratum_server_start(&cfg, &s) != 0) return NULL;
    uint8_t net[32] = {0}; net[7] = 0xff; net[8] = 0xff;
    stratum_server_set_job(s, make_test_job("J1", net), 1);
    return s;
}

/* Most tests do not care about the pin floor; give them the shipped default. */
static stratum_server_t *sd_server(obs_t *obs, int enabled, int submit_ceiling,
                                   double vd_min) {
    return sd_server_min(obs, enabled, submit_ceiling, vd_min, 16384);
}

/* Authorize with `pw` and return the difficulty the server announced. */
static double sd_authorize(stratum_server_t *s, stratum_conn_t *c, const char *pw) {
    char *out = NULL; size_t olen = 0; char msg[256];
    stratum_handle_message(s, c, "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}",
                           &out, &olen); free(out); out = NULL; olen = 0;
    snprintf(msg, sizeof msg,
             "{\"id\":2,\"method\":\"mining.authorize\",\"params\":[\"" TEST_ADDR "\",\"%s\"]}", pw);
    stratum_handle_message(s, c, msg, &out, &olen);
    double d = stratum_conn_difficulty_for_test(c);
    free(out);
    return d;
}

/* 🔴 THE GATE. Off by default, and the assertion is that the connection lands
 * on initial_diff — not merely "not 50000". A pin that silently became a floor
 * would also miss 50000 while doing something entirely different. */
static void test_sd_is_off_by_default(void) {
    obs_t obs = {0};
    stratum_server_t *s = sd_server(&obs, 0, 120, 1024.0);
    CHECK(s != NULL); if (!s) return;
    stratum_conn_t *c = stratum_conn_new_for_test(s);
    CHECK(sd_authorize(s, c, "sd=50000") == 100000.0);
    CHECK(stratum_conn_pinned_diff_for_test(c) == 0.0);
    stratum_conn_free_for_test(c); stratum_server_free(s);
    printf("ok: sd= does not pin unless static_diff_enabled\n");
}

/* ...and the instrument check for the gate: the same request with the flag on
 * must actually pin, or three "not pinned" results would be equally consistent
 * with sd= never having been wired up at all. */
static void test_sd_pins_when_enabled(void) {
    obs_t obs = {0};
    stratum_server_t *s = sd_server(&obs, 1, 120, 1024.0);
    CHECK(s != NULL); if (!s) return;
    stratum_conn_t *c = stratum_conn_new_for_test(s);
    CHECK(sd_authorize(s, c, "sd=50000") == 50000.0);
    /* On the field, not inferred from the difficulty: a floor at 4242 would
     * leave the same difficulty here. */
    CHECK(stratum_conn_pinned_diff_for_test(c) == 50000.0);
    stratum_conn_free_for_test(c); stratum_server_free(s);
    printf("ok: sd= pins the connection when enabled\n");
}

/* The floor is the safety argument, so assert the exact value it lands on. */
static void test_sd_is_floored_at_vardiff_min(void) {
    obs_t obs = {0};
    stratum_server_t *s = sd_server(&obs, 1, 120, 1024.0);
    CHECK(s != NULL); if (!s) return;
    stratum_conn_t *c = stratum_conn_new_for_test(s);
    CHECK(sd_authorize(s, c, "sd=1") == 16384.0);
    CHECK(stratum_conn_pinned_diff_for_test(c) == 16384.0);
    stratum_conn_free_for_test(c); stratum_server_free(s);
    printf("ok: sd= below the floor is clamped up to static_diff_min\n");
}

/* 🔴 THE POINT OF static_diff_min: the pin floor must NOT depend on
 * vardiff_min. vardiff_min is 1 here — its shipped default, and the value we
 * could never confirm on the live host — and a pin at 1 must still be refused
 * down to static_diff_min rather than honoured. If the floor ever regressed to
 * conn_vardiff_min() this lands at 1, which is the denial-of-service the whole
 * design exists to prevent. */
static void test_sd_floor_does_not_depend_on_vardiff_min(void) {
    obs_t obs = {0};
    stratum_server_t *s = sd_server_min(&obs, 1, 120, 1.0, 16384);
    CHECK(s != NULL); if (!s) return;
    stratum_conn_t *c = stratum_conn_new_for_test(s);
    CHECK(sd_authorize(s, c, "sd=1") == 16384.0);
    CHECK(stratum_conn_pinned_diff_for_test(c) == 16384.0);
    stratum_conn_free_for_test(c); stratum_server_free(s);
    printf("ok: the pin floor is static_diff_min even when vardiff_min is 1\n");
}

/* ...and a listener asking for MORE still wins, so a marketplace floor is not
 * lowered by this. */
static void test_sd_listener_floor_still_wins_over_static_diff_min(void) {
    obs_t obs = {0};
    stratum_server_t *s = sd_server_min(&obs, 1, 120, 50000.0, 16384);
    CHECK(s != NULL); if (!s) return;
    stratum_conn_t *c = stratum_conn_new_for_test(s);
    CHECK(sd_authorize(s, c, "sd=1") == 50000.0);
    stratum_conn_free_for_test(c); stratum_server_free(s);
    printf("ok: a higher vardiff floor still wins over static_diff_min\n");
}

/* 🔴 The ceiling, not the floor, is what bounds a pinned flood — so refuse to
 * PIN when it is off. But the request must degrade to a FLOOR, not vanish.
 *
 * The request is deliberately ABOVE initial_diff, because that is the only
 * value that separates the two outcomes: at 200000 a floor raises the
 * connection and a discard leaves it at 100000. Asserting a request BELOW
 * initial_diff would pass under both and prove nothing. */
static void test_sd_refused_falls_back_to_a_floor(void) {
    obs_t obs = {0};
    stratum_server_t *s = sd_server(&obs, 1, 0, 1024.0);
    CHECK(s != NULL); if (!s) return;
    stratum_conn_t *c = stratum_conn_new_for_test(s);
    CHECK(sd_authorize(s, c, "sd=200000") == 200000.0);
    /* 🔴 THE SAFETY ASSERTION. A floor and a pin at 200000 leave the SAME
     * difficulty, so the line above cannot tell them apart — and "not pinned"
     * is the entire point of refusing when the submit ceiling is off. Without
     * this, the refusal could break and the test would still pass. */
    CHECK(stratum_conn_pinned_diff_for_test(c) == 0.0);
    stratum_conn_free_for_test(c); stratum_server_free(s);
    printf("ok: a refused sd= degrades to a floor, it is not discarded\n");
}

/* Same for the kill switch, and this is the one that matters operationally:
 * flipping static_diff_enabled to 0 on a live pool must not drop pinned miners
 * to initial_diff and re-vardiff them from there. An emergency switch whose own
 * action causes a share surge is the wrong shape. */
static void test_sd_disabled_still_honours_the_floor(void) {
    obs_t obs = {0};
    stratum_server_t *s = sd_server(&obs, 0, 120, 1024.0);
    CHECK(s != NULL); if (!s) return;
    stratum_conn_t *c = stratum_conn_new_for_test(s);
    CHECK(sd_authorize(s, c, "sd=200000") == 200000.0);
    CHECK(stratum_conn_pinned_diff_for_test(c) == 0.0);
    stratum_conn_free_for_test(c); stratum_server_free(s);
    printf("ok: sd= with the feature off still applies as a floor\n");
}

/* ⛔ A malformed sd= must not take a valid d= down with it. Both orders, because
 * the first version of the two-pass parse aborted inside pass 0 and so was
 * order-independent for valid tokens and kind-dependent for malformed ones. */
static void test_malformed_sd_does_not_kill_a_valid_d(void) {
    obs_t obs = {0};
    stratum_server_t *s = sd_server(&obs, 1, 120, 1024.0);
    CHECK(s != NULL); if (!s) return;
    stratum_conn_t *a = stratum_conn_new_for_test(s);
    stratum_conn_t *b = stratum_conn_new_for_test(s);
    CHECK(sd_authorize(s, a, "sd=abc,d=200000") == 200000.0);
    CHECK(sd_authorize(s, b, "d=200000,sd=abc") == 200000.0);
    stratum_conn_free_for_test(a); stratum_conn_free_for_test(b);
    stratum_server_free(s);
    printf("ok: a malformed sd= leaves a valid d= intact, in either order\n");
}

/* 🔴 Order-independence. Same two tokens, both orders, same answer — the bug
 * being guarded is a single left-to-right scan making these mean opposite
 * things with no log line either way. */
static void test_sd_beats_d_in_either_order(void) {
    obs_t obs = {0};
    stratum_server_t *s = sd_server(&obs, 1, 120, 1024.0);
    CHECK(s != NULL); if (!s) return;
    stratum_conn_t *a = stratum_conn_new_for_test(s);
    stratum_conn_t *b = stratum_conn_new_for_test(s);
    double first  = sd_authorize(s, a, "d=50000,sd=50000");
    double second = sd_authorize(s, b, "sd=50000,d=50000");
    CHECK(first == 50000.0);
    CHECK(second == 50000.0);
    CHECK(first == second);
    stratum_conn_free_for_test(a); stratum_conn_free_for_test(b);
    stratum_server_free(s);
    printf("ok: sd= wins over d= regardless of token order\n");
}


/* 🔴 END-TO-END OVER A REAL SOCKET.
 *
 * Every other sd= test calls stratum_handle_message directly, which skips
 * accept(), the connection thread, the read loop and the line framing. That
 * proves the logic and NOT that a miner's password reaches it. This drives the
 * real path: connect, send subscribe and authorize as a miner's client would,
 * and read what the server actually writes back on the wire.
 *
 * The assertion is on `mining.set_difficulty` — the only thing the miner ever
 * sees, and the value it will configure itself to. */
static void test_sd_end_to_end_over_a_socket(void) {
    obs_t obs = {0};
    int port = 39434;
    stratum_server_t *s = NULL;
    for (int p = port; p < port + 20 && !s; p++) {
        stratum_cfg_t cfg = { .bind_port = p, .max_conns = 4,
                              .initial_diff = 100000.0,
                              .vardiff_enabled = 1, .vardiff_target_spm = 12,
                              .vardiff_min = 1.0, .vardiff_max = 1e12,
                              .vardiff_window_sec = 30,
                              .max_suggested_diff = 1e9,
                              .max_submits_per_sec = 120,
                              .static_diff_enabled = 1,
                              .static_diff_min = 16384,
                              .ctx = &obs, .on_share = on_share,
                              .on_reject = on_reject, .on_block = on_block };
        snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
        if (stratum_server_start(&cfg, &s) == 0) { port = p; break; }
        s = NULL;
    }
    CHECK(s != NULL); if (!s) return;
    uint8_t net[32] = {0}; net[7] = 0xff; net[8] = 0xff;
    stratum_server_set_job(s, make_test_job("J1", net), 1);

    int fd = dial(AF_INET, port);
    CHECK(fd >= 0);
    if (fd >= 0) {
        const char *sub = "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}\n";
        const char *aut = "{\"id\":2,\"method\":\"mining.authorize\","
                          "\"params\":[\"" TEST_ADDR "\",\"x,sd=50000\"]}\n";
        CHECK(write(fd, sub, strlen(sub)) == (ssize_t)strlen(sub));
        sleep_ms(120);
        CHECK(write(fd, aut, strlen(aut)) == (ssize_t)strlen(aut));
        sleep_ms(350);

        char rx[8192] = {0};
        size_t got = 0;
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        for (int i = 0; i < 8 && got < sizeof(rx) - 1; i++) {
            ssize_t n = recv(fd, rx + got, sizeof(rx) - 1 - got, 0);
            if (n <= 0) break;
            got += (size_t)n;
            if (strstr(rx, "mining.set_difficulty")) break;
        }
        /* The number the miner is actually told to use. */
        CHECK(strstr(rx, "\"mining.set_difficulty\"") != NULL);
        CHECK(strstr(rx, "\"params\":[50000]") != NULL);
        CHECK(strstr(rx, "\"params\":[100000]") == NULL);   /* not initial_diff */
        /* ...and the server really pinned, not merely floored. */
        CHECK(stratum_server_pinned_count_for_test(s) == 1);
        close(fd);
        sleep_ms(250);
        /* The counter must come back down, or a long-running pool would report
         * a pinned population that only ever grows. */
        CHECK(stratum_server_pinned_count_for_test(s) == 0);
    }
    stratum_server_free(s);
    printf("ok: sd= in the password pins a real socket session end to end\n");
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
    test_dual_stack_ipv4_peer_is_dotted_quad();
    test_dual_stack_accepts_ipv6();
    test_ipv4_wildcard_is_still_ipv4_only();
    test_vardiff_tracks_miner_local_floor();
    test_stale_difficulty_shares_do_not_drive_vardiff();
    test_drain_guard_cleans_up_after_itself();
    test_vardiff_floor_detect_respects_min_samples();
    test_vardiff_still_lowers_for_a_matched_miner();
    test_vardiff_clamped_to_network_diff();
    test_vardiff_grace_accepts_old_diff_shares();
    test_vardiff_waits_for_min_samples();
    test_password_diff_raises_difficulty();
    test_password_diff_cannot_lower_difficulty();
    test_password_diff_capped();
    test_requested_diff_clamped_to_network();
    test_suggest_difficulty_method();
    test_vardiff_cannot_lower_below_request();
    test_password_diff_parsing_edge_cases();
    test_requests_disabled_when_cap_is_zero();
    test_socket_setup_applies_rcvtimeo();
    test_socket_setup_disabled();
    test_extranonce1_unique_across_connections();
    test_stop_waits_for_connection_threads();
    test_dedupe_same_hash_across_job_ids();
    test_rejected_candidate_is_not_a_block();
    test_accepted_candidate_reports_accepted();
    test_job_survives_retirement_while_held();
    test_held_job_is_freed_on_release();
    test_gated_pps_refuses_authorize_and_submits();
    test_gate_can_be_disabled();
    test_solo_is_never_gated();
    test_authorize_resumes_difficulty_from_hint();
    test_authorize_without_hint_uses_initial();
    test_proportional_shared_coinbase();
    test_proportional_falls_back_without_window();
    test_extranonce1_is_one_sequence_per_server();
    test_listener_policy_reaches_the_connection();
    test_listener_floor_beats_the_server_wide_floor();
    test_hint_below_vardiff_min_is_floored();
    test_request_lowers_replayed_hint_at_authorize();
    test_request_below_floor_lowers_only_to_floor();
    test_sd_is_off_by_default();
    test_sd_pins_when_enabled();
    test_sd_is_floored_at_vardiff_min();
    test_sd_floor_does_not_depend_on_vardiff_min();
    test_sd_listener_floor_still_wins_over_static_diff_min();
    test_sd_refused_falls_back_to_a_floor();
    test_sd_disabled_still_honours_the_floor();
    test_malformed_sd_does_not_kill_a_valid_d();
    test_sd_beats_d_in_either_order();
    test_sd_end_to_end_over_a_socket();
    test_initial_diff_below_vardiff_min_is_not_floored();
    test_submit_judged_at_the_jobs_own_difficulty();
    test_submit_ceiling_refuses_past_the_limit();
    test_submit_ceiling_window_rolls();
    test_submit_ceiling_zero_disables();
    test_submit_below_the_jobs_own_difficulty_is_rejected();
    test_unknown_job_record_falls_back_to_grace();
    test_rerecording_a_job_does_not_consume_ring_slots();
    test_clean_jobs_only_on_a_new_block();
    test_unauthorized_submit_is_recorded_as_a_reject();
    test_job_id_classification_is_three_way();
    test_stale_reject_carries_a_job_age();
    test_reject_records_the_peer_ip();
    printf("test_stratum: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
