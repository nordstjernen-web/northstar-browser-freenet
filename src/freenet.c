/* Northstar — the freenet: URL scheme, mapped onto a local Freenet node.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "freenet.h"

#include "config.h"
#include "ws.h"

#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>

#define NS_FREENET_SCHEME_LEN 8
#define NS_FREENET_WEB_PATH   "v1/contract/web/"
#define NS_FREENET_WEB_TAIL   "contract/web/"

static const char *const ns_freenet_api_versions[] = { "v1/", "v2/", NULL };

static const char *const ns_freenet_node_paths[] = {
    "v1/", "v2/", "permission/", "freenet-notify-sw.js", NULL
};

static const char ns_freenet_base58[] =
    "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

static const char *
ns_freenet_after_web_prefix(const char *path)
{
    for (int i = 0; ns_freenet_api_versions[i]; i++) {
        if (!g_str_has_prefix(path, ns_freenet_api_versions[i])) continue;
        const char *rest = path + strlen(ns_freenet_api_versions[i]);
        if (g_str_has_prefix(rest, NS_FREENET_WEB_TAIL))
            return rest + strlen(NS_FREENET_WEB_TAIL);
    }
    return NULL;
}

static gboolean
ns_freenet_path_belongs_to_node(const char *path)
{
    for (int i = 0; ns_freenet_node_paths[i]; i++)
        if (g_str_has_prefix(path, ns_freenet_node_paths[i]))
            return TRUE;
    return FALSE;
}

static gboolean
ns_freenet_is_base58(guint8 c)
{
    return c != 0 && strchr(ns_freenet_base58, c) != NULL;
}

gboolean
ns_freenet_is_url(const char *url)
{
    return url && g_ascii_strncasecmp(url, "freenet:", NS_FREENET_SCHEME_LEN) == 0;
}

gboolean
ns_freenet_key_is_valid(const char *key)
{
    if (!key) return FALSE;
    size_t len = strlen(key);
    if (len < 1 || len > 64) return FALSE;
    for (size_t i = 0; i < len; i++)
        if (!ns_freenet_is_base58((guint8)key[i]))
            return FALSE;
    return TRUE;
}

gboolean
ns_freenet_key_is_full(const char *key)
{
    return ns_freenet_key_is_valid(key) && strlen(key) >= 32;
}

const char *
ns_freenet_gateway(void)
{
    const ns_config *cfg = ns_config_get();
    if (cfg && cfg->freenet_gateway && *cfg->freenet_gateway)
        return cfg->freenet_gateway;
    return NS_FREENET_DEFAULT_GATEWAY;
}

char *
ns_freenet_gateway_base(gboolean websocket)
{
    const char *gateway = ns_freenet_gateway();
    const char *authority = gateway;
    gboolean tls = FALSE;

    if (g_ascii_strncasecmp(gateway, "https://", 8) == 0) {
        tls = TRUE;
        authority = gateway + 8;
    } else if (g_ascii_strncasecmp(gateway, "http://", 7) == 0) {
        authority = gateway + 7;
    }

    size_t len = strlen(authority);
    while (len && authority[len - 1] == '/') len--;
    if (!len) return NULL;

    const char *transport = websocket ? (tls ? "wss://" : "ws://")
                                      : (tls ? "https://" : "http://");
    char *host = g_strndup(authority, len);
    char *base = g_strconcat(transport, host, NULL);
    g_free(host);
    return base;
}

static gboolean
ns_freenet_take_key(const char *p, char **key_out, char **rest_out)
{
    const char *end = p + strcspn(p, "/?#");
    if (end == p) return FALSE;

    char *key = g_strndup(p, (size_t)(end - p));
    if (!ns_freenet_key_is_valid(key)) {
        g_free(key);
        return FALSE;
    }

    *key_out  = key;
    *rest_out = g_strdup(*end == '/' ? end + 1 : end);
    return TRUE;
}

static gboolean
ns_freenet_split(const char *url, char **key_out, char **rest_out)
{
    if (!ns_freenet_is_url(url)) return FALSE;

    const char *p = url + NS_FREENET_SCHEME_LEN;
    if (p[0] == '/' && p[1] == '/') p += 2;
    while (*p == '/') p++;

    if (!ns_freenet_take_key(p, key_out, rest_out)) return FALSE;

    for (const char *inner; (inner = ns_freenet_after_web_prefix(*rest_out)); ) {
        char *inner_key = NULL, *inner_rest = NULL;
        if (!ns_freenet_take_key(inner, &inner_key, &inner_rest))
            break;
        g_free(*key_out);
        g_free(*rest_out);
        *key_out  = inner_key;
        *rest_out = inner_rest;
    }
    return TRUE;
}

char *
ns_freenet_key_of(const char *url)
{
    char *key = NULL, *rest = NULL;
    if (!ns_freenet_split(url, &key, &rest)) return NULL;
    g_free(rest);
    return key;
}

char *
ns_freenet_canonical_url(const char *input)
{
    char *key = NULL, *rest = NULL;
    if (!ns_freenet_split(input, &key, &rest)) return NULL;
    char *out = g_strconcat("freenet://", key, "/", rest, NULL);
    g_free(key);
    g_free(rest);
    return out;
}

static char *
ns_freenet_map(const char *url, gboolean websocket)
{
    char *key = NULL, *rest = NULL;
    if (!ns_freenet_split(url, &key, &rest)) return NULL;

    char *base = ns_freenet_gateway_base(websocket);
    char *out = NULL;
    if (base)
        out = ns_freenet_path_belongs_to_node(rest)
            ? g_strconcat(base, "/", rest, NULL)
            : g_strconcat(base, "/", NS_FREENET_WEB_PATH, key, "/", rest, NULL);

    g_free(base);
    g_free(key);
    g_free(rest);
    return out;
}

char *
ns_freenet_to_gateway(const char *url)
{
    return ns_freenet_map(url, FALSE);
}

char *
ns_freenet_to_gateway_ws(const char *url)
{
    return ns_freenet_map(url, TRUE);
}

#ifdef NS_HAVE_FREENET_RS
guint8 *ns_freenet_rs_contract_query(gsize *len);
guint8 *ns_freenet_rs_status_query(gsize *len);
char   *ns_freenet_rs_contract_ids(const guint8 *data, gsize len);
char   *ns_freenet_rs_status_text(const guint8 *data, gsize len);
void    ns_freenet_rs_free(guint8 *ptr, gsize len);
void    ns_freenet_rs_free_string(char *ptr);
#endif

typedef enum {
    NS_FREENET_QUERY_CONTRACTS,
    NS_FREENET_QUERY_STATUS,
} ns_freenet_query;

static void
ns_freenet_put_u32(GByteArray *out, guint32 v)
{
    guint8 b[4] = { v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >> 24) & 0xff };
    g_byte_array_append(out, b, sizeof b);
}

static GByteArray *
ns_freenet_diagnostics_request(ns_freenet_query which)
{
#ifdef NS_HAVE_FREENET_RS
    gsize encoded_len = 0;
    guint8 *encoded = which == NS_FREENET_QUERY_STATUS
        ? ns_freenet_rs_status_query(&encoded_len)
        : ns_freenet_rs_contract_query(&encoded_len);
    if (encoded) {
        GByteArray *from_stdlib = g_byte_array_new();
        g_byte_array_append(from_stdlib, encoded, (guint)encoded_len);
        ns_freenet_rs_free(encoded, encoded_len);
        return from_stdlib;
    }
#endif
    const guint8 no = 0, yes = 1;
    const guint8 want_node = which == NS_FREENET_QUERY_STATUS ? yes : no;
    GByteArray *req = g_byte_array_new();
    ns_freenet_put_u32(req, 4);
    ns_freenet_put_u32(req, 2);
    g_byte_array_append(req, &want_node, 1);
    g_byte_array_append(req, &want_node, 1);
    g_byte_array_append(req, &yes, 1);
    for (int i = 0; i < 8; i++) g_byte_array_append(req, &no, 1);
    g_byte_array_append(req, &no, 1);
    g_byte_array_append(req, &no, 1);
    g_byte_array_append(req, &no, 1);
    return req;
}

static GByteArray *
ns_freenet_node_query(ns_freenet_query which)
{
    if (!ns_ws_available()) return NULL;

    g_autofree char *base = ns_freenet_gateway_base(TRUE);
    if (!base) return NULL;
    g_autofree char *url =
        g_strconcat(base, "/v1/contract/command?encodingProtocol=native", NULL);

    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

    if (curl_easy_perform(curl) != CURLE_OK) {
        curl_easy_cleanup(curl);
        return NULL;
    }

    GByteArray *request = ns_freenet_diagnostics_request(which);
    size_t sent = 0;
    CURLcode rc = curl_ws_send(curl, request->data, request->len, &sent, 0,
                               CURLWS_BINARY);
    g_byte_array_free(request, TRUE);
    if (rc != CURLE_OK) {
        curl_easy_cleanup(curl);
        return NULL;
    }

    /* A reply larger than the read buffer arrives in several pieces, and a
     * piece can report no bytes left in its own frame while the message goes
     * on. Read until the socket has been idle for a moment, so a long answer
     * is not truncated into something that will not decode. */
    GByteArray *reply = g_byte_array_new();
    int idle = 0;
    for (int spins = 0; spins < 2000 && reply->len < (1u << 20); spins++) {
        char buf[8192];
        size_t got = 0;
        const struct curl_ws_frame *meta = NULL;
        rc = curl_ws_recv(curl, buf, sizeof buf, &got, &meta);
        if (rc == CURLE_AGAIN) {
            if (reply->len && ++idle > 40) break;
            g_usleep(5000);
            continue;
        }
        if (rc != CURLE_OK) break;
        idle = 0;
        if (got) g_byte_array_append(reply, (const guint8 *)buf, (guint)got);
    }

    curl_easy_cleanup(curl);
    if (!reply->len) {
        g_byte_array_free(reply, TRUE);
        return NULL;
    }
    return reply;
}

GByteArray *
ns_freenet_node_diagnostics(void)
{
    return ns_freenet_node_query(NS_FREENET_QUERY_CONTRACTS);
}

static size_t
ns_freenet_discard(void *data, size_t size, size_t n, void *user)
{
    (void)data; (void)user;
    return size * n;
}

static void
ns_freenet_status_probe_gateway(ns_freenet_status *status)
{
    g_autofree char *base = ns_freenet_gateway_base(FALSE);
    if (!base) {
        status->error = g_strdup("The configured gateway is not a host and port.");
        return;
    }
    g_autofree char *url = g_strconcat(base, "/", NULL);

    CURL *curl = curl_easy_init();
    if (!curl) return;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ns_freenet_discard);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);

    CURLcode rc = curl_easy_perform(curl);
    if (rc == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status->http_status);
        status->reachable = status->http_status > 0;
    } else {
        status->error = g_strdup(curl_easy_strerror(rc));
    }
    curl_easy_cleanup(curl);
}

static void
ns_freenet_status_read_details(ns_freenet_status *status)
{
#ifdef NS_HAVE_FREENET_RS
    GByteArray *reply = ns_freenet_node_query(NS_FREENET_QUERY_STATUS);
    if (!reply) return;
    char *text = ns_freenet_rs_status_text(reply->data, reply->len);
    g_byte_array_free(reply, TRUE);
    if (!text) return;

    g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
    ns_freenet_rs_free_string(text);
    for (int i = 0; lines[i]; i++) {
        char *eq = strchr(lines[i], '=');
        if (!eq) continue;
        *eq = '\0';
        const char *key = lines[i], *value = eq + 1;
        if (g_str_equal(key, "peers"))            status->peers = atoi(value);
        else if (g_str_equal(key, "connections")) status->connections = atoi(value);
        else if (g_str_equal(key, "contracts"))   status->contracts = atoi(value);
        else if (g_str_equal(key, "uptime"))      status->uptime_seconds = g_ascii_strtoll(value, NULL, 10);
        else if (g_str_equal(key, "gateway"))     status->is_gateway = *value == '1';
        else if (g_str_equal(key, "peer_id"))     status->peer_id = g_strdup(value);
    }
    status->detailed = TRUE;
#else
    (void)status;
#endif
}

ns_freenet_status *
ns_freenet_status_query(void)
{
    ns_freenet_status *status = g_new0(ns_freenet_status, 1);
    ns_freenet_status_probe_gateway(status);
    if (status->reachable)
        ns_freenet_status_read_details(status);
    return status;
}

void
ns_freenet_status_free(ns_freenet_status *status)
{
    if (!status) return;
    g_free(status->peer_id);
    g_free(status->error);
    g_free(status);
}

char *
ns_freenet_with_key(const char *url, const char *key)
{
    char *old_key = NULL, *rest = NULL;
    if (!ns_freenet_split(url, &old_key, &rest)) return NULL;
    char *out = g_strconcat("freenet://", key, "/", rest, NULL);
    g_free(old_key);
    g_free(rest);
    return out;
}

static char *
ns_freenet_match_prefix(const char *const *ids, const char *prefix)
{
    size_t prefix_len = strlen(prefix);
    char *found = NULL;
    for (int i = 0; ids[i]; i++) {
        if (strncmp(ids[i], prefix, prefix_len) != 0) continue;
        if (!ns_freenet_key_is_full(ids[i])) continue;
        if (found && strcmp(found, ids[i]) != 0) {
            g_free(found);
            return NULL;
        }
        if (!found) found = g_strdup(ids[i]);
    }
    return found;
}

void
ns_freenet_collect_keys(const guint8 *data, gsize len, GPtrArray *out)
{
    if (!data || !len || !out) return;

#ifdef NS_HAVE_FREENET_RS
    char *decoded = ns_freenet_rs_contract_ids(data, len);
    if (decoded) {
        g_auto(GStrv) ids = g_strsplit(decoded, "\n", -1);
        ns_freenet_rs_free_string(decoded);
        for (int i = 0; ids[i]; i++)
            if (ns_freenet_key_is_full(ids[i]))
                g_ptr_array_add(out, g_strdup(ids[i]));
        return;
    }
#endif

    for (gsize i = 0; i < len; ) {
        if (!ns_freenet_is_base58(data[i])) { i++; continue; }
        gsize start = i;
        while (i < len && ns_freenet_is_base58(data[i])) i++;
        gsize run = i - start;
        if (run < 32 || run > 64) continue;
        g_ptr_array_add(out, g_strndup((const char *)data + start, run));
    }
}

char *
ns_freenet_find_key_with_prefix(const guint8 *data, gsize len,
                                const char *prefix)
{
    if (!data || !len || !prefix || !*prefix) return NULL;

    g_autoptr(GPtrArray) keys = g_ptr_array_new_with_free_func(g_free);
    ns_freenet_collect_keys(data, len, keys);
    g_ptr_array_add(keys, NULL);
    return ns_freenet_match_prefix((const char *const *)keys->pdata, prefix);
}

static GMutex     g_known_contracts_lock;
static GPtrArray *g_known_contracts;
static gint64     g_known_contracts_at;

#define NS_FREENET_CONTRACT_CACHE_US (30 * G_USEC_PER_SEC)

char *
ns_freenet_known_contract_for_host(const char *host)
{
    if (!host || strlen(host) < 32 || strlen(host) > 64) return NULL;

    g_mutex_lock(&g_known_contracts_lock);
    gint64 now = g_get_monotonic_time();
    if (!g_known_contracts || now - g_known_contracts_at > NS_FREENET_CONTRACT_CACHE_US) {
        g_mutex_unlock(&g_known_contracts_lock);
        GByteArray *diagnostics = ns_freenet_node_diagnostics();
        GPtrArray *fresh = g_ptr_array_new_with_free_func(g_free);
        if (diagnostics) {
            ns_freenet_collect_keys(diagnostics->data, diagnostics->len, fresh);
            g_byte_array_free(diagnostics, TRUE);
        }
        g_mutex_lock(&g_known_contracts_lock);
        if (g_known_contracts) g_ptr_array_unref(g_known_contracts);
        g_known_contracts = fresh;
        g_known_contracts_at = g_get_monotonic_time();
    }

    char *found = NULL;
    for (guint i = 0; i < g_known_contracts->len && !found; i++) {
        const char *candidate = g_ptr_array_index(g_known_contracts, i);
        if (g_ascii_strcasecmp(candidate, host) == 0)
            found = g_strdup(candidate);
    }
    g_mutex_unlock(&g_known_contracts_lock);
    return found;
}

/* The node's client API answers anything that can reach the port: it has no
 * origin check and no authentication on loopback, and a WebSocket is not
 * bound by the same-origin policy. So an ordinary web page can open one and
 * drive the node. Only a document that is itself Freenet has business there. */
gboolean
ns_freenet_is_node_endpoint(const char *url)
{
    if (!url) return FALSE;

    static const char *const schemes[] = {
        "ws://", "wss://", "http://", "https://", NULL
    };
    const char *authority = NULL;
    for (int i = 0; schemes[i]; i++) {
        size_t n = strlen(schemes[i]);
        if (g_ascii_strncasecmp(url, schemes[i], n) == 0) {
            authority = url + n;
            break;
        }
    }
    if (!authority) return FALSE;

    g_autofree char *base = ns_freenet_gateway_base(FALSE);
    if (!base) return FALSE;
    const char *gateway = strstr(base, "://");
    gateway = gateway ? gateway + 3 : base;

    size_t host_len = strcspn(authority, "/?#");
    size_t gateway_len = strlen(gateway);
    return host_len == gateway_len &&
           g_ascii_strncasecmp(authority, gateway, host_len) == 0;
}

char *
ns_freenet_localize_origin(const char *target, const char *doc_url)
{
    if (!target || !ns_freenet_is_url(doc_url)) return NULL;

    static const struct { const char *scheme; gboolean websocket; } schemes[] = {
        { "ws://", TRUE }, { "wss://", TRUE },
        { "http://", FALSE }, { "https://", FALSE },
    };
    const char *authority = NULL;
    gboolean websocket = FALSE;
    for (gsize i = 0; i < G_N_ELEMENTS(schemes); i++) {
        size_t n = strlen(schemes[i].scheme);
        if (g_ascii_strncasecmp(target, schemes[i].scheme, n) == 0) {
            authority = target + n;
            websocket = schemes[i].websocket;
            break;
        }
    }
    if (!authority) return NULL;

    size_t host_len = strcspn(authority, ":/?#");
    g_autofree char *host = g_strndup(authority, host_len);

    g_autofree char *key = ns_freenet_key_of(doc_url);
    if (!key || g_ascii_strcasecmp(host, key) != 0) {
        g_autofree char *known = ns_freenet_known_contract_for_host(host);
        if (!known) return NULL;
    }

    const char *rest = authority + host_len;
    rest += strcspn(rest, "/?#");
    while (*rest == '/') rest++;

    char *base = ns_freenet_gateway_base(websocket);
    char *out = base ? g_strconcat(base, "/", rest, NULL) : NULL;
    g_free(base);
    return out;
}

char *
ns_freenet_node_error(long status, const guint8 *body, gsize len)
{
    if (status < 400 || !body || !len) return NULL;

    gsize scan = len < 512 ? len : 512;
    for (gsize i = 0; i < scan; i++)
        if (body[i] == '<')
            return NULL;

    gsize keep = len < 400 ? len : 400;
    char *text = g_strndup((const char *)body, keep);
    g_strstrip(text);
    if (!*text || !g_utf8_validate(text, -1, NULL)) {
        g_free(text);
        return NULL;
    }
    return text;
}

char *
ns_freenet_localize_csp(const char *csp, const char *url)
{
    if (!csp || !*csp) return NULL;

    char *base = ns_freenet_gateway_base(FALSE);
    if (!base) return NULL;
    if (!strstr(csp, base)) {
        g_free(base);
        return NULL;
    }

    char *key = ns_freenet_key_of(url);
    if (!key) {
        g_free(base);
        return NULL;
    }

    char *widened = g_strconcat(base, " freenet://", key, NULL);
    char **parts = g_strsplit(csp, base, -1);
    char *out = g_strjoinv(widened, parts);

    g_strfreev(parts);
    g_free(widened);
    g_free(key);
    g_free(base);
    return out;
}

char *
ns_freenet_from_gateway(const char *url)
{
    if (!url) return NULL;

    g_autofree char *base = ns_freenet_gateway_base(FALSE);
    if (!base) return NULL;
    if (!g_str_has_prefix(url, base) || url[strlen(base)] != '/') return NULL;

    const char *p = ns_freenet_after_web_prefix(url + strlen(base) + 1);
    if (!p) return NULL;
    const char *end = p + strcspn(p, "/?#");
    if (end == p) return NULL;

    char *key = g_strndup(p, (size_t)(end - p));
    if (!ns_freenet_key_is_valid(key)) {
        g_free(key);
        return NULL;
    }

    char *out = g_strconcat("freenet://", key, "/",
                            *end == '/' ? end + 1 : end, NULL);
    g_free(key);
    return out;
}
