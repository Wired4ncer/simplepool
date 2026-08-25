#include "../src/bitcoind.h"
#include "../src/cjson/cJSON.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static const char *SAMPLE_GBT =
"{"
"  \"version\": 536870912,"
"  \"previousblockhash\": \"0000000000000000000a1b2c3d4e5f6789abcdef0123456789abcdef01234567\","
"  \"transactions\": ["
"    {"
"      \"data\": \"0100000001abcd\","
"      \"txid\": \"aabbccddeeff00112233445566778899aabbccddeeff00112233445566778899\","
"      \"hash\": \"aabbccddeeff00112233445566778899aabbccddeeff00112233445566778899\","
"      \"fee\": 12345,"
"      \"sigops\": 4,"
"      \"weight\": 800"
"    },"
"    {"
"      \"data\": \"0200000002deadbeef\","
"      \"txid\": \"1111111111111111111111111111111111111111111111111111111111111111\","
"      \"fee\": 600,"
"      \"weight\": 400"
"    }"
"  ],"
"  \"coinbaseaux\": {},"
"  \"coinbasevalue\": 5000012945,"
"  \"target\": \"0000000000000000000a000000000000000000000000000000000000000000ff\","
"  \"mintime\": 1700000000,"
"  \"mutable\": [\"time\",\"transactions\",\"prevblock\"],"
"  \"noncerange\": \"00000000ffffffff\","
"  \"sigoplimit\": 80000,"
"  \"sizelimit\": 4000000,"
"  \"weightlimit\": 4000000,"
"  \"curtime\": 1700001234,"
"  \"bits\": \"170abc12\","
"  \"height\": 800123,"
"  \"default_witness_commitment\": \"6a24aa21a9ed0000000000000000000000000000000000000000000000000000000000000000\","
"  \"longpollid\": \"0000000000000000000a1b2c3d4e5f6789abcdef0123456789abcdef01234567\""
"}";

static void test_parse_ok(void) {
    cJSON *root = cJSON_Parse(SAMPLE_GBT);
    assert(root);
    bitcoind_template_t *t = NULL;
    char err[256] = {0};
    int rc = bitcoind_parse_template(root, &t, err, sizeof(err));
    CHECK(rc == 0);
    CHECK(t != NULL);
    if (t) {
        CHECK(t->height == 800123);
        CHECK(strcmp(t->prev_hash_hex, "0000000000000000000a1b2c3d4e5f6789abcdef0123456789abcdef01234567") == 0);
        CHECK(t->coinbase_value_sats == 5000012945LL);
        CHECK(strcmp(t->target_hex, "0000000000000000000a000000000000000000000000000000000000000000ff") == 0);
        CHECK(t->bits == 0x170abc12u);
        CHECK(t->curtime == 1700001234u);
        CHECK(t->version == 536870912);
        CHECK(t->min_time == 1700000000LL);
        CHECK(t->default_witness_commitment != NULL);
        CHECK(t->default_witness_commitment != NULL &&
              strncmp(t->default_witness_commitment, "6a24aa21a9ed", 12) == 0);
        /* BIP22 long-poll token is optional but must round-trip when sent. */
        CHECK(t->longpollid != NULL);
        CHECK(t->longpollid != NULL &&
              strcmp(t->longpollid,
                     "0000000000000000000a1b2c3d4e5f6789abcdef0123456789abcdef01234567") == 0);
        CHECK(t->tx_count == 2);
        CHECK(t->txs != NULL);
        if (t->tx_count == 2 && t->txs) {
            CHECK(strcmp(t->txs[0].data_hex, "0100000001abcd") == 0);
            CHECK(strcmp(t->txs[0].txid_hex, "aabbccddeeff00112233445566778899aabbccddeeff00112233445566778899") == 0);
            CHECK(t->txs[0].fee == 12345);
            CHECK(t->txs[0].weight == 800);
            CHECK(strcmp(t->txs[1].data_hex, "0200000002deadbeef") == 0);
            CHECK(t->txs[1].fee == 600);
            CHECK(t->txs[1].weight == 400);
        }
    }
    bitcoind_template_free(t);
    cJSON_Delete(root);
}

/* Backends like the CUSF enforcer return a server-built coinbase under
 * "coinbasetxn" (no "coinbasevalue"). The coinbase's total output value is
 * reported as a negative "fee"; the parser stores the raw tx hex and recovers
 * the value by negating that fee. */
static const char *SAMPLE_GBT_COINBASETXN =
"{"
"  \"version\": 536870912,"
"  \"previousblockhash\": \"0000000000000000000a1b2c3d4e5f6789abcdef0123456789abcdef01234567\","
"  \"transactions\": [],"
"  \"coinbaseaux\": {},"
"  \"coinbasetxn\": {"
"    \"data\": \"02000000010000000000000000000000000000000000000000000000000000000000000000ffffffff04025c0bffffffff0100f2052a01000000160014000000000000000000000000000000000000000000000000\","
"    \"txid\": \"aabbccddeeff00112233445566778899aabbccddeeff00112233445566778899\","
"    \"hash\": \"aabbccddeeff00112233445566778899aabbccddeeff00112233445566778899\","
"    \"depends\": [],"
"    \"fee\": -5000000000,"
"    \"sigops\": 0,"
"    \"weight\": 400"
"  },"
"  \"target\": \"0000000000000000000a000000000000000000000000000000000000000000ff\","
"  \"mintime\": 1700000000,"
"  \"noncerange\": \"00000000ffffffff\","
"  \"curtime\": 1700001234,"
"  \"bits\": \"170abc12\","
"  \"height\": 800123,"
"  \"default_witness_commitment\": \"6a24aa21a9ed0000000000000000000000000000000000000000000000000000000000000000\""
"}";

static void test_parse_coinbasetxn(void) {
    cJSON *root = cJSON_Parse(SAMPLE_GBT_COINBASETXN);
    assert(root);
    bitcoind_template_t *t = NULL;
    char err[256] = {0};
    int rc = bitcoind_parse_template(root, &t, err, sizeof(err));
    CHECK(rc == 0);
    CHECK(t != NULL);
    if (t) {
        CHECK(t->height == 800123);
        /* value derived by negating the coinbasetxn "fee" */
        CHECK(t->coinbase_value_sats == 5000000000LL);
        CHECK(t->coinbasetxn_hex != NULL);
        CHECK(t->coinbasetxn_hex != NULL &&
              strncmp(t->coinbasetxn_hex, "02000000", 8) == 0);
        CHECK(t->bits == 0x170abc12u);
        /* No longpollid in this fixture: the server doesn't long poll. */
        CHECK(t->longpollid == NULL);
    }
    bitcoind_template_free(t);
    cJSON_Delete(root);
}

static void test_parse_missing_prevhash(void) {
    const char *bad =
        "{ \"version\":1, \"coinbasevalue\":100, \"target\":\"00\","
        "  \"bits\":\"1d00ffff\", \"curtime\":1, \"height\":1, \"transactions\":[] }";
    cJSON *root = cJSON_Parse(bad);
    assert(root);
    bitcoind_template_t *t = NULL;
    char err[256] = {0};
    int rc = bitcoind_parse_template(root, &t, err, sizeof(err));
    CHECK(rc < 0);
    CHECK(t == NULL);
    CHECK(strlen(err) > 0);
    cJSON_Delete(root);
}

static void test_parse_empty_txs(void) {
    const char *src =
        "{ \"version\":1,"
        "  \"previousblockhash\":\"00000000000000000000000000000000aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
        "  \"coinbasevalue\":50,"
        "  \"target\":\"ffff000000000000000000000000000000000000000000000000000000000000\","
        "  \"bits\":\"1d00ffff\","
        "  \"curtime\":42,"
        "  \"height\":1,"
        "  \"mintime\":40,"
        "  \"transactions\":[] }";
    cJSON *root = cJSON_Parse(src);
    assert(root);
    bitcoind_template_t *t = NULL;
    char err[256] = {0};
    int rc = bitcoind_parse_template(root, &t, err, sizeof(err));
    CHECK(rc == 0);
    CHECK(t != NULL);
    if (t) {
        CHECK(t->tx_count == 0);
        CHECK(t->txs == NULL);
        CHECK(t->bits == 0x1d00ffffu);
        CHECK(t->default_witness_commitment == NULL);
    }
    bitcoind_template_free(t);
    cJSON_Delete(root);
}

static void test_double_free_safe(void) {
    /* template_free(NULL) must be safe */
    bitcoind_template_free(NULL);
    CHECK(1);
}

static void test_client_init_validates(void) {
    bitcoind_cfg_t cfg = {0};
    bitcoind_client_t c;
    int rc = bitcoind_client_init(&c, &cfg);
    CHECK(rc < 0); /* empty url */

    snprintf(cfg.url, sizeof(cfg.url), "http://127.0.0.1:18443/");
    snprintf(cfg.user, sizeof(cfg.user), "u");
    snprintf(cfg.pass, sizeof(cfg.pass), "p");
    rc = bitcoind_client_init(&c, &cfg);
    CHECK(rc == 0);
    CHECK(c._curl != NULL);
    CHECK(c._lock != NULL);
    bitcoind_client_free(&c);
    CHECK(c._curl == NULL);
    CHECK(c._lock == NULL);
}


/* ---- stub JSON-RPC server ------------------------------------------------
 *
 * bitcoind_ping() talks HTTP, so proving its fallback needs a server that
 * answers one method and refuses another. Pointing at a dead port only shows
 * that failure fails.
 *
 * Serves a fixed number of requests then exits, so the thread always joins.
 */

#define STUB_FULL_NODE     0   /* answers getblockchaininfo (real bitcoind) */
#define STUB_TEMPLATE_ONLY 1   /* only getblocktemplate (the CUSF enforcer) */
#define STUB_DEAD          2   /* answers nothing (backend is broken) */

typedef struct {
    int listen_fd;
    int port;
    int mode;
    int requests;      /* how many to serve before returning */
    int saw_gbt;       /* did a getblocktemplate request arrive? */
    int saw_getblockhash;
} stub_t;

#define STUB_BLOCK_HASH \
    "0000000000000000000fedcba9876543210fedcba9876543210fedcba987654"

static const char *STUB_TEMPLATE_RESULT =
    "{\"version\":536870912,"
    "\"previousblockhash\":\"0000000000000000000a1b2c3d4e5f6789abcdef0123456789abcdef01234567\","
    "\"transactions\":[],\"coinbasevalue\":312500000,"
    "\"target\":\"0000000000000000000a000000000000000000000000000000000000000000ff\","
    "\"mintime\":1700000000,\"curtime\":1700001234,"
    "\"bits\":\"170abc12\",\"height\":800123}";

static void stub_reply(int fd, const char *json_body) {
    char hdr[256];
    int blen = (int)strlen(json_body);
    int hlen = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Content-Length: %d\r\nConnection: close\r\n\r\n", blen);
    (void)!write(fd, hdr, (size_t)hlen);
    (void)!write(fd, json_body, (size_t)blen);
}

static void *stub_thread(void *arg) {
    stub_t *st = (stub_t *)arg;
    for (int i = 0; i < st->requests; i++) {
        int fd = accept(st->listen_fd, NULL, NULL);
        if (fd < 0) break;
        char buf[8192];
        ssize_t n = read(fd, buf, sizeof buf - 1);
        if (n <= 0) { close(fd); continue; }
        buf[n] = '\0';

        int wants_gbt  = strstr(buf, "getblocktemplate")  != NULL;
        int wants_info = strstr(buf, "getblockchaininfo") != NULL;
        int wants_hash = strstr(buf, "getblockhash")      != NULL;
        if (wants_gbt) st->saw_gbt = 1;
        if (wants_hash) st->saw_getblockhash = 1;

        char body[2048];
        if (st->mode == STUB_FULL_NODE && wants_hash) {
            snprintf(body, sizeof body,
                     "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":\"%s\","
                     "\"error\":null}", STUB_BLOCK_HASH);
        } else if (st->mode == STUB_FULL_NODE && wants_info) {
            snprintf(body, sizeof body,
                     "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"chain\":\"main\"},\"error\":null}");
        } else if (st->mode != STUB_DEAD && wants_gbt) {
            snprintf(body, sizeof body,
                     "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":%s,\"error\":null}",
                     STUB_TEMPLATE_RESULT);
        } else {
            /* Exactly what the enforcer says to anything it does not serve. */
            snprintf(body, sizeof body,
                     "{\"jsonrpc\":\"2.0\",\"id\":1,"
                     "\"error\":{\"code\":-32601,\"message\":\"Method not found\"}}");
        }
        stub_reply(fd, body);
        close(fd);
    }
    return NULL;
}

static int stub_start(stub_t *st, int mode, int requests) {
    memset(st, 0, sizeof *st);
    st->mode = mode;
    st->requests = requests;
    st->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (st->listen_fd < 0) return -1;
    int one = 1;
    setsockopt(st->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    /* inet_addr rather than INADDR_LOOPBACK: the build sets
     * -D_POSIX_C_SOURCE, under which macOS does not expose that macro. */
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    a.sin_port = 0;   /* ephemeral */
    if (bind(st->listen_fd, (struct sockaddr *)&a, sizeof a) < 0) return -1;
    if (listen(st->listen_fd, 8) < 0) return -1;
    socklen_t alen = sizeof a;
    if (getsockname(st->listen_fd, (struct sockaddr *)&a, &alen) < 0) return -1;
    st->port = ntohs(a.sin_port);
    return 0;
}

/* A pool pointed at the CUSF enforcer must start. The enforcer serves only
 * getblocktemplate and submitblock, so a ping hard-wired to
 * getblockchaininfo refuses to start against the one backend that supplies
 * BIP300/301 commitments — which is how the pool ends up mining blocks no
 * sidechain can be merge-mined into. */
static void test_ping_falls_back_to_template(void) {
    stub_t st;
    if (stub_start(&st, STUB_TEMPLATE_ONLY, 2) != 0) { CHECK(0); return; }
    pthread_t th;
    pthread_create(&th, NULL, stub_thread, &st);

    bitcoind_client_t c;
    bitcoind_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    snprintf(cfg.url, sizeof cfg.url, "http://127.0.0.1:%d", st.port);
    cfg.timeout_ms = 5000;
    CHECK(bitcoind_client_init(&c, &cfg) == 0);

    char err[512] = {0};
    CHECK(bitcoind_ping(&c, err, sizeof err) == 0);
    CHECK(st.saw_gbt == 1);   /* it really did fall back */

    bitcoind_client_free(&c);
    pthread_join(th, NULL);
    close(st.listen_fd);
}

/* The common path must not regress: a real node answers the first call and
 * the template probe is never needed. */
static void test_ping_uses_chaininfo_when_available(void) {
    stub_t st;
    if (stub_start(&st, STUB_FULL_NODE, 1) != 0) { CHECK(0); return; }
    pthread_t th;
    pthread_create(&th, NULL, stub_thread, &st);

    bitcoind_client_t c;
    bitcoind_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    snprintf(cfg.url, sizeof cfg.url, "http://127.0.0.1:%d", st.port);
    cfg.timeout_ms = 5000;
    CHECK(bitcoind_client_init(&c, &cfg) == 0);

    char err[512] = {0};
    CHECK(bitcoind_ping(&c, err, sizeof err) == 0);
    CHECK(st.saw_gbt == 0);   /* no pointless second call */

    bitcoind_client_free(&c);
    pthread_join(th, NULL);
    close(st.listen_fd);
}

/* A backend that serves neither must still be reported as down — the
 * fallback widens what counts as alive, it must not make everything pass. */
static void test_ping_fails_when_nothing_is_served(void) {
    stub_t st;
    if (stub_start(&st, STUB_DEAD, 2) != 0) { CHECK(0); return; }
    pthread_t th;
    pthread_create(&th, NULL, stub_thread, &st);

    bitcoind_client_t c;
    bitcoind_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    snprintf(cfg.url, sizeof cfg.url, "http://127.0.0.1:%d", st.port);
    cfg.timeout_ms = 5000;
    CHECK(bitcoind_client_init(&c, &cfg) == 0);

    char err[512] = {0};
    CHECK(bitcoind_ping(&c, err, sizeof err) != 0);
    /* The surviving error must name the capability we actually need. */
    CHECK(strstr(err, "Method not found") != NULL);

    bitcoind_client_free(&c);
    pthread_join(th, NULL);
    close(st.listen_fd);
}

/* A backend that does not serve getblockhash must be recognised as such, not
 * treated as a failure to retry. The CUSF enforcer — the backend a drivechain
 * pool has to point at — answers "Method not found" to everything but
 * getblocktemplate and submitblock, so a confirmation design that assumes
 * getblockhash simply never runs there. The caller latches this answer and
 * falls back to the observed chain of template tips. */
static void test_getblockhash_unsupported_is_distinguished(void) {
    stub_t st;
    if (stub_start(&st, STUB_TEMPLATE_ONLY, 1) != 0) { CHECK(0); return; }
    pthread_t th;
    pthread_create(&th, NULL, stub_thread, &st);

    bitcoind_client_t c;
    bitcoind_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    snprintf(cfg.url, sizeof cfg.url, "http://127.0.0.1:%d", st.port);
    cfg.timeout_ms = 5000;
    CHECK(bitcoind_client_init(&c, &cfg) == 0);

    char hash[80] = {0};
    char err[256] = {0};
    int rc = bitcoind_get_block_hash(&c, 800123, hash, sizeof hash,
                                     err, sizeof err);
    CHECK(rc == BITCOIND_ERR_UNSUPPORTED);
    CHECK(hash[0] == '\0');

    bitcoind_client_free(&c);
    pthread_join(th, NULL);
    close(st.listen_fd);
}

/* And where it is served, the hash comes back verbatim — this is what says a
 * block the pool submitted is still the one at its height. */
static void test_getblockhash_returns_the_hash(void) {
    stub_t st;
    if (stub_start(&st, STUB_FULL_NODE, 1) != 0) { CHECK(0); return; }
    pthread_t th;
    pthread_create(&th, NULL, stub_thread, &st);

    bitcoind_client_t c;
    bitcoind_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    snprintf(cfg.url, sizeof cfg.url, "http://127.0.0.1:%d", st.port);
    cfg.timeout_ms = 5000;
    CHECK(bitcoind_client_init(&c, &cfg) == 0);

    char hash[80] = {0};
    char err[256] = {0};
    int rc = bitcoind_get_block_hash(&c, 800123, hash, sizeof hash,
                                     err, sizeof err);
    CHECK(rc == 0);
    CHECK(strcmp(hash, STUB_BLOCK_HASH) == 0);
    CHECK(st.saw_getblockhash == 1);

    bitcoind_client_free(&c);
    pthread_join(th, NULL);
    close(st.listen_fd);
}

int main(void) {
    test_parse_ok();
    test_parse_coinbasetxn();
    test_parse_missing_prevhash();
    test_parse_empty_txs();
    test_double_free_safe();
    test_client_init_validates();
    test_ping_uses_chaininfo_when_available();
    test_ping_falls_back_to_template();
    test_ping_fails_when_nothing_is_served();
    test_getblockhash_unsupported_is_distinguished();
    test_getblockhash_returns_the_hash();

    printf("test_bitcoind: %d passed, %d failed\n", g_pass, g_fail);
    if (g_fail == 0) {
        printf("all assertions passed\n");
        return 0;
    }
    return 1;
}
