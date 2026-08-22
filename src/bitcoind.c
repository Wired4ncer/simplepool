#define _POSIX_C_SOURCE 200809L
#include "bitcoind.h"
#include "cjson/cJSON.h"
#include "log.h"

#include <curl/curl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define RESP_INITIAL_CAP (64 * 1024)
#define RESP_MAX_CAP     (32 * 1024 * 1024)

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    int oom;
} resp_buf_t;

static void set_err(char *errbuf, size_t errlen, const char *fmt, ...) {
    if (!errbuf || errlen == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(errbuf, errlen, fmt, ap);
    va_end(ap);
}

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    resp_buf_t *r = (resp_buf_t *)userdata;
    size_t add = size * nmemb;
    if (r->oom) return 0;
    if (r->len + add + 1 > r->cap) {
        size_t newcap = r->cap ? r->cap : RESP_INITIAL_CAP;
        while (newcap < r->len + add + 1) {
            if (newcap >= RESP_MAX_CAP) {
                r->oom = 1;
                return 0;
            }
            newcap *= 2;
            if (newcap > RESP_MAX_CAP) newcap = RESP_MAX_CAP;
        }
        char *nb = (char *)realloc(r->buf, newcap);
        if (!nb) { r->oom = 1; return 0; }
        r->buf = nb;
        r->cap = newcap;
    }
    memcpy(r->buf + r->len, ptr, add);
    r->len += add;
    r->buf[r->len] = '\0';
    return add;
}

int bitcoind_client_init(bitcoind_client_t *out, const bitcoind_cfg_t *cfg) {
    if (!out || !cfg) return -1;
    if (cfg->url[0] == '\0') return -2;
    memset(out, 0, sizeof(*out));
    out->cfg = *cfg;
    if (out->cfg.timeout_ms <= 0) out->cfg.timeout_ms = 10000;

    CURL *c = curl_easy_init();
    if (!c) return -3;

    pthread_mutex_t *m = (pthread_mutex_t *)malloc(sizeof(*m));
    if (!m) { curl_easy_cleanup(c); return -4; }
    if (pthread_mutex_init(m, NULL) != 0) { free(m); curl_easy_cleanup(c); return -5; }

    out->_curl = c;
    out->_lock = m;
    return 0;
}

void bitcoind_client_free(bitcoind_client_t *c) {
    if (!c) return;
    if (c->_curl) {
        curl_easy_cleanup((CURL *)c->_curl);
        c->_curl = NULL;
    }
    if (c->_lock) {
        pthread_mutex_destroy((pthread_mutex_t *)c->_lock);
        free(c->_lock);
        c->_lock = NULL;
    }
}

/* Performs a JSON-RPC call. On success returns 0 and sets *out_result to
 * a heap-allocated cJSON of the "result" field (caller must cJSON_Delete).
 * On error returns negative and fills errbuf. */
static int rpc_call(bitcoind_client_t *c,
                    const char *method,
                    cJSON *params /* takes ownership */,
                    cJSON **out_result,
                    char *errbuf, size_t errlen) {
    if (!c || !c->_curl || !c->_lock) {
        if (params) cJSON_Delete(params);
        set_err(errbuf, errlen, "client not initialized");
        return -1;
    }
    if (out_result) *out_result = NULL;

    cJSON *req = cJSON_CreateObject();
    if (!req) {
        if (params) cJSON_Delete(params);
        set_err(errbuf, errlen, "oom");
        return -2;
    }
    /* JSON-RPC 2.0: bitcoind accepts both, but strict servers (e.g. the CUSF
     * enforcer, built on jsonrpsee) reject 1.0 requests outright. */
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddStringToObject(req, "id", "proxy");
    cJSON_AddStringToObject(req, "method", method);
    if (params) {
        cJSON_AddItemToObject(req, "params", params);
    } else {
        cJSON_AddItemToObject(req, "params", cJSON_CreateArray());
    }

    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) {
        set_err(errbuf, errlen, "json print failed");
        return -3;
    }

    LOG_DEBUG("bitcoind rpc -> %s %s: %s", c->cfg.url, method, body);

    pthread_mutex_t *m = (pthread_mutex_t *)c->_lock;
    pthread_mutex_lock(m);

    CURL *cu = (CURL *)c->_curl;
    resp_buf_t resp = {0};
    resp.buf = (char *)malloc(RESP_INITIAL_CAP);
    if (resp.buf) { resp.cap = RESP_INITIAL_CAP; resp.buf[0] = '\0'; }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_reset(cu);
    curl_easy_setopt(cu, CURLOPT_URL, c->cfg.url);
    curl_easy_setopt(cu, CURLOPT_POST, 1L);
    curl_easy_setopt(cu, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(cu, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    curl_easy_setopt(cu, CURLOPT_HTTPHEADER, headers);

    /* Some block-template backends (e.g. a stock bitcoind reachable over a
     * trusted socket) accept unauthenticated JSON-RPC. When neither user nor
     * pass is configured we omit the credentials entirely rather than sending
     * an empty "user:pass" basic-auth header. */
    char userpwd[128 + 256 + 2];
    if (c->cfg.user[0] != '\0' || c->cfg.pass[0] != '\0') {
        snprintf(userpwd, sizeof(userpwd), "%s:%s", c->cfg.user, c->cfg.pass);
        curl_easy_setopt(cu, CURLOPT_USERPWD, userpwd);
        curl_easy_setopt(cu, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
    }

    curl_easy_setopt(cu, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(cu, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(cu, CURLOPT_TIMEOUT_MS, c->cfg.timeout_ms);
    curl_easy_setopt(cu, CURLOPT_NOSIGNAL, 1L);

    CURLcode rc = curl_easy_perform(cu);
    long http_code = 0;
    curl_easy_getinfo(cu, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    free(body);
    pthread_mutex_unlock(m);

    if (rc == CURLE_OK) {
        LOG_DEBUG("bitcoind rpc <- %s (http %ld): %s", method, http_code,
                  resp.buf ? resp.buf : "");
    }

    if (rc != CURLE_OK) {
        set_err(errbuf, errlen, "curl: %s", curl_easy_strerror(rc));
        free(resp.buf);
        return -10;
    }
    if (resp.oom) {
        set_err(errbuf, errlen, "response too large");
        free(resp.buf);
        return -11;
    }
    if (http_code != 200 && http_code != 500) {
        /* bitcoind sends 500 for RPC errors with a JSON body, treat that path below */
        set_err(errbuf, errlen, "http %ld: %.*s", http_code,
                (int)(resp.len > 200 ? 200 : resp.len),
                resp.buf ? resp.buf : "");
        free(resp.buf);
        return -12;
    }

    cJSON *root = cJSON_Parse(resp.buf ? resp.buf : "");
    free(resp.buf);
    if (!root) {
        set_err(errbuf, errlen, "json parse failed");
        return -13;
    }
    cJSON *err = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (err && !cJSON_IsNull(err)) {
        const char *msg = "unknown rpc error";
        cJSON *m2 = cJSON_GetObjectItemCaseSensitive(err, "message");
        if (cJSON_IsString(m2) && m2->valuestring) msg = m2->valuestring;
        set_err(errbuf, errlen, "rpc error: %s", msg);
        cJSON_Delete(root);
        return -14;
    }
    cJSON *result = cJSON_DetachItemFromObjectCaseSensitive(root, "result");
    cJSON_Delete(root);
    if (!result) {
        set_err(errbuf, errlen, "missing result");
        return -15;
    }
    if (out_result) {
        *out_result = result;
    } else {
        cJSON_Delete(result);
    }
    return 0;
}

/* params: [{"rules":["segwit"],"capabilities":["coinbasetxn","longpoll"]}]
 *
 * Requesting the "coinbasetxn" capability is harmless against Bitcoin Core
 * (it ignores it and still returns "coinbasevalue"), but tells backends that
 * can dictate the coinbase — notably the CUSF enforcer — to hand us a fully
 * built coinbase carrying the mandatory BIP300/301 commitments. "longpoll"
 * declares client-side BIP22 long-poll support (a SHOULD in the BIP); servers
 * advertise theirs by including "longpollid" in the template.
 *
 * Returns NULL on allocation failure. */
static cJSON *gbt_params(const char *longpollid) {
    cJSON *params = cJSON_CreateArray();
    cJSON *obj    = cJSON_CreateObject();
    cJSON *rules  = cJSON_CreateArray();
    cJSON *caps   = cJSON_CreateArray();
    if (!params || !obj || !rules || !caps) {
        cJSON_Delete(params); cJSON_Delete(obj);
        cJSON_Delete(rules);  cJSON_Delete(caps);
        return NULL;
    }
    cJSON_AddItemToArray(rules, cJSON_CreateString("segwit"));
    cJSON_AddItemToObject(obj, "rules", rules);
    cJSON_AddItemToArray(caps, cJSON_CreateString("coinbasetxn"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("longpoll"));
    cJSON_AddItemToObject(obj, "capabilities", caps);
    if (longpollid && longpollid[0]) {
        cJSON_AddStringToObject(obj, "longpollid", longpollid);
    }
    cJSON_AddItemToArray(params, obj);
    return params;
}

/* Startup liveness probe.
 *
 * Tries getblockchaininfo first, which is what a full node answers. A
 * template server is not a full node: the CUSF enforcer serves exactly
 * getblocktemplate and submitblock and replies "Method not found" to
 * everything else, so requiring getblockchaininfo would refuse to start
 * against the very backend that supplies BIP300/301 commitments.
 *
 * So fall back to getblocktemplate. That is the better probe anyway — the
 * question at startup is not "are you a full node?" but "can you hand me
 * work?", and getblocktemplate is the call the proxy actually depends on.
 * When both fail, errbuf keeps the getblocktemplate error, because that names
 * the capability we need. */
int bitcoind_ping(bitcoind_client_t *c, char *errbuf, size_t errlen) {
    cJSON *result = NULL;
    if (rpc_call(c, "getblockchaininfo", NULL, &result, errbuf, errlen) == 0) {
        if (result) cJSON_Delete(result);
        return 0;
    }

    cJSON *params = gbt_params(NULL);
    if (!params) { set_err(errbuf, errlen, "oom"); return -20; }
    result = NULL;
    int rc = rpc_call(c, "getblocktemplate", params, &result, errbuf, errlen);
    if (result) cJSON_Delete(result);
    return rc;
}

static int hex_to_u32(const char *hex, uint32_t *out) {
    if (!hex || !out) return -1;
    char *end = NULL;
    unsigned long v = strtoul(hex, &end, 16);
    if (!end || *end != '\0') return -1;
    *out = (uint32_t)v;
    return 0;
}

static void copy_hex64(char *dst, const char *src) {
    if (!src) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n > 64) n = 64;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

int bitcoind_parse_template(void *result_json,
                            bitcoind_template_t **out,
                            char *errbuf, size_t errlen) {
    if (!out) return -1;
    *out = NULL;
    cJSON *r = (cJSON *)result_json;
    if (!r || !cJSON_IsObject(r)) {
        set_err(errbuf, errlen, "result is not an object");
        return -2;
    }

    cJSON *jh = cJSON_GetObjectItemCaseSensitive(r, "height");
    cJSON *jp = cJSON_GetObjectItemCaseSensitive(r, "previousblockhash");
    cJSON *jc = cJSON_GetObjectItemCaseSensitive(r, "coinbasevalue");
    cJSON *jcbtxn = cJSON_GetObjectItemCaseSensitive(r, "coinbasetxn");
    cJSON *jt = cJSON_GetObjectItemCaseSensitive(r, "target");
    cJSON *jb = cJSON_GetObjectItemCaseSensitive(r, "bits");
    cJSON *jct = cJSON_GetObjectItemCaseSensitive(r, "curtime");
    cJSON *jv = cJSON_GetObjectItemCaseSensitive(r, "version");
    cJSON *jmt = cJSON_GetObjectItemCaseSensitive(r, "mintime");
    cJSON *jdwc = cJSON_GetObjectItemCaseSensitive(r, "default_witness_commitment");
    cJSON *jtxs = cJSON_GetObjectItemCaseSensitive(r, "transactions");

    if (!cJSON_IsNumber(jh))               { set_err(errbuf, errlen, "missing height");            return -3;  }
    if (!cJSON_IsString(jp) || !jp->valuestring) { set_err(errbuf, errlen, "missing previousblockhash"); return -4; }
    /* The reward arrives as either "coinbasevalue" (we build the coinbase) or
     * "coinbasetxn" (server-provided coinbase); require at least one. */
    int has_value = cJSON_IsNumber(jc);
    int has_cbtxn = cJSON_IsObject(jcbtxn);
    if (!has_value && !has_cbtxn) { set_err(errbuf, errlen, "missing coinbasevalue/coinbasetxn"); return -5; }
    if (!cJSON_IsString(jt) || !jt->valuestring) { set_err(errbuf, errlen, "missing target");      return -6;  }
    if (!cJSON_IsString(jb) || !jb->valuestring) { set_err(errbuf, errlen, "missing bits");        return -7;  }
    if (!cJSON_IsNumber(jct))              { set_err(errbuf, errlen, "missing curtime");           return -8;  }
    if (!cJSON_IsNumber(jv))               { set_err(errbuf, errlen, "missing version");           return -9;  }

    bitcoind_template_t *t = (bitcoind_template_t *)calloc(1, sizeof(*t));
    if (!t) { set_err(errbuf, errlen, "oom"); return -20; }

    t->height = (int)jh->valuedouble;
    copy_hex64(t->prev_hash_hex, jp->valuestring);
    copy_hex64(t->target_hex, jt->valuestring);

    if (has_value) {
        t->coinbase_value_sats = (int64_t)jc->valuedouble;
        /* BIP22 allows a server to send both. We keep using coinbasevalue and
         * build our own coinbase, which silently drops whatever the server put
         * in coinbasetxn — for the CUSF enforcer that is the mandatory
         * BIP300/301 commitments, so the blocks stop being able to carry a
         * sidechain, and pool_mode=proportional loses the shared coinbase it
         * needs and pays every block to its finder instead. Nothing else
         * reports that, so say it here. Changing which one wins is a
         * behavioural change and is deliberately NOT made silently. */
        if (has_cbtxn) {
            LOG_WARN("getblocktemplate returned BOTH coinbasevalue and "
                     "coinbasetxn; using coinbasevalue and building our own "
                     "coinbase, which drops any commitments the server put in "
                     "coinbasetxn (proportional mode needs coinbasetxn)");
        }
    } else {
        /* coinbasetxn: keep the raw coinbase hex; the value is reported as a
         * negative "fee" (fee = -sum(coinbase outputs)), so negate it. */
        cJSON *jdata = cJSON_GetObjectItemCaseSensitive(jcbtxn, "data");
        cJSON *jfee  = cJSON_GetObjectItemCaseSensitive(jcbtxn, "fee");
        if (!cJSON_IsString(jdata) || !jdata->valuestring) {
            set_err(errbuf, errlen, "coinbasetxn missing data");
            free(t);
            return -11;
        }
        t->coinbasetxn_hex = strdup(jdata->valuestring);
        if (!t->coinbasetxn_hex) { set_err(errbuf, errlen, "oom"); free(t); return -12; }
        t->coinbase_value_sats = cJSON_IsNumber(jfee) ? (int64_t)(-jfee->valuedouble) : 0;
    }

    if (hex_to_u32(jb->valuestring, &t->bits) != 0) {
        set_err(errbuf, errlen, "bad bits hex");
        bitcoind_template_free(t);
        return -10;
    }
    t->curtime = (uint32_t)jct->valuedouble;
    t->version = (int32_t)jv->valuedouble;
    t->min_time = cJSON_IsNumber(jmt) ? (int64_t)jmt->valuedouble : 0;
    {
        cJSON *jwl = cJSON_GetObjectItemCaseSensitive(r, "weightlimit");
        t->weight_limit = cJSON_IsNumber(jwl) ? (int64_t)jwl->valuedouble : 0;
    }
    /* curtime can sit below the consensus minimum (median-time-past + 1)
     * when blocks were just mined in a rapid burst, e.g. regtest
     * generatetoaddress — a block stamped with it is rejected
     * "time-too-old". mintime is the floor the template gives us. */
    if (t->min_time > 0 && (int64_t)t->curtime < t->min_time)
        t->curtime = (uint32_t)t->min_time;

    if (cJSON_IsString(jdwc) && jdwc->valuestring) {
        t->default_witness_commitment = strdup(jdwc->valuestring);
        if (!t->default_witness_commitment) {
            set_err(errbuf, errlen, "oom");
            bitcoind_template_free(t);
            return -21;
        }
    }

    cJSON *jlp = cJSON_GetObjectItemCaseSensitive(r, "longpollid");
    if (cJSON_IsString(jlp) && jlp->valuestring && jlp->valuestring[0]) {
        t->longpollid = strdup(jlp->valuestring);
        if (!t->longpollid) {
            set_err(errbuf, errlen, "oom");
            bitcoind_template_free(t);
            return -25;
        }
    }

    if (jtxs && cJSON_IsArray(jtxs)) {
        int n = cJSON_GetArraySize(jtxs);
        if (n > 0) {
            t->txs = (bitcoind_template_tx_t *)calloc((size_t)n, sizeof(*t->txs));
            if (!t->txs) {
                set_err(errbuf, errlen, "oom");
                bitcoind_template_free(t);
                return -22;
            }
            for (int i = 0; i < n; i++) {
                cJSON *tx = cJSON_GetArrayItem(jtxs, i);
                cJSON *jd = cJSON_GetObjectItemCaseSensitive(tx, "data");
                cJSON *jx = cJSON_GetObjectItemCaseSensitive(tx, "txid");
                cJSON *jf = cJSON_GetObjectItemCaseSensitive(tx, "fee");
                cJSON *jw = cJSON_GetObjectItemCaseSensitive(tx, "weight");
                if (!cJSON_IsString(jd) || !jd->valuestring) {
                    set_err(errbuf, errlen, "tx[%d] missing data", i);
                    t->tx_count = (size_t)i;
                    bitcoind_template_free(t);
                    return -23;
                }
                t->txs[i].data_hex = strdup(jd->valuestring);
                if (!t->txs[i].data_hex) {
                    set_err(errbuf, errlen, "oom");
                    t->tx_count = (size_t)i;
                    bitcoind_template_free(t);
                    return -24;
                }
                if (cJSON_IsString(jx) && jx->valuestring) {
                    copy_hex64(t->txs[i].txid_hex, jx->valuestring);
                }
                t->txs[i].fee = cJSON_IsNumber(jf) ? (int64_t)jf->valuedouble : 0;
                t->txs[i].weight = cJSON_IsNumber(jw) ? (int)jw->valuedouble : 0;
            }
            t->tx_count = (size_t)n;
        }
    }

    *out = t;
    return 0;
}

int bitcoind_get_block_template(bitcoind_client_t *c,
                                bitcoind_template_t **out,
                                char *errbuf, size_t errlen) {
    if (!out) return -1;
    *out = NULL;

    return bitcoind_get_block_template_lp(c, NULL, out, errbuf, errlen);
}

int bitcoind_get_block_template_lp(bitcoind_client_t *c,
                                   const char *longpollid,
                                   bitcoind_template_t **out,
                                   char *errbuf, size_t errlen) {
    if (!out) return -1;
    *out = NULL;

    cJSON *params = gbt_params(longpollid);
    if (!params) { set_err(errbuf, errlen, "oom"); return -20; }

    cJSON *result = NULL;
    int rc = rpc_call(c, "getblocktemplate", params, &result, errbuf, errlen);
    if (rc != 0) return rc;

    rc = bitcoind_parse_template(result, out, errbuf, errlen);
    cJSON_Delete(result);
    return rc;
}

int bitcoind_submit_block(bitcoind_client_t *c, const char *block_hex,
                          char *errbuf, size_t errlen) {
    if (!block_hex) {
        set_err(errbuf, errlen, "null block_hex");
        return -1;
    }
    cJSON *params = cJSON_CreateArray();
    cJSON_AddItemToArray(params, cJSON_CreateString(block_hex));

    cJSON *result = NULL;
    int rc = rpc_call(c, "submitblock", params, &result, errbuf, errlen);
    if (rc != 0) return rc;

    /* submitblock returns null on success; non-null string == reject reason */
    if (result && cJSON_IsString(result) && result->valuestring && result->valuestring[0] != '\0') {
        set_err(errbuf, errlen, "rejected: %s", result->valuestring);
        cJSON_Delete(result);
        return -30;
    }
    if (result) cJSON_Delete(result);
    return 0;
}

void bitcoind_template_free(bitcoind_template_t *t) {
    if (!t) return;
    if (t->txs) {
        for (size_t i = 0; i < t->tx_count; i++) {
            free(t->txs[i].data_hex);
        }
        free(t->txs);
    }
    free(t->default_witness_commitment);
    free(t->coinbasetxn_hex);
    free(t->longpollid);
    free(t);
}
