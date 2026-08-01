/* Northstar — the freenet: URL scheme, mapped onto a local Freenet node.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "freenet.h"

#include "config.h"
#include "ws.h"

#include <curl/curl.h>
#include <string.h>

#define NS_FREENET_SCHEME_LEN 8
#define NS_FREENET_WEB_PATH   "v1/contract/web/"
#define NS_FREENET_API_PATH   "v1/"

static const char ns_freenet_base58[] =
    "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

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

    while (g_str_has_prefix(*rest_out, NS_FREENET_WEB_PATH)) {
        char *inner_key = NULL, *inner_rest = NULL;
        if (!ns_freenet_take_key(*rest_out + strlen(NS_FREENET_WEB_PATH),
                                 &inner_key, &inner_rest))
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
        out = g_str_has_prefix(rest, NS_FREENET_API_PATH)
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

static void
ns_freenet_put_u32(GByteArray *out, guint32 v)
{
    guint8 b[4] = { v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >> 24) & 0xff };
    g_byte_array_append(out, b, sizeof b);
}

static GByteArray *
ns_freenet_diagnostics_request(void)
{
    GByteArray *req = g_byte_array_new();
    const guint8 no = 0;
    ns_freenet_put_u32(req, 4);
    ns_freenet_put_u32(req, 2);
    g_byte_array_append(req, &no, 1);
    g_byte_array_append(req, &no, 1);
    const guint8 yes = 1;
    g_byte_array_append(req, &yes, 1);
    for (int i = 0; i < 8; i++) g_byte_array_append(req, &no, 1);
    g_byte_array_append(req, &no, 1);
    g_byte_array_append(req, &no, 1);
    g_byte_array_append(req, &no, 1);
    return req;
}

GByteArray *
ns_freenet_node_diagnostics(void)
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

    GByteArray *request = ns_freenet_diagnostics_request();
    size_t sent = 0;
    CURLcode rc = curl_ws_send(curl, request->data, request->len, &sent, 0,
                               CURLWS_BINARY);
    g_byte_array_free(request, TRUE);
    if (rc != CURLE_OK) {
        curl_easy_cleanup(curl);
        return NULL;
    }

    GByteArray *reply = g_byte_array_new();
    for (int spins = 0; spins < 2000 && reply->len < (1u << 20); spins++) {
        char buf[8192];
        size_t got = 0;
        const struct curl_ws_frame *meta = NULL;
        rc = curl_ws_recv(curl, buf, sizeof buf, &got, &meta);
        if (rc == CURLE_AGAIN) {
            g_usleep(5000);
            continue;
        }
        if (rc != CURLE_OK) break;
        if (got) g_byte_array_append(reply, (const guint8 *)buf, (guint)got);
        if (meta && meta->bytesleft == 0 && reply->len) break;
    }

    curl_easy_cleanup(curl);
    if (!reply->len) {
        g_byte_array_free(reply, TRUE);
        return NULL;
    }
    return reply;
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

char *
ns_freenet_find_key_with_prefix(const guint8 *data, gsize len,
                                const char *prefix)
{
    if (!data || !len || !prefix || !*prefix) return NULL;

    size_t prefix_len = strlen(prefix);
    char *found = NULL;

    for (gsize i = 0; i < len; ) {
        if (!ns_freenet_is_base58(data[i])) { i++; continue; }
        gsize start = i;
        while (i < len && ns_freenet_is_base58(data[i])) i++;
        gsize run = i - start;
        if (run < 32 || run > 64) continue;
        if (run < prefix_len) continue;
        if (strncmp((const char *)data + start, prefix, prefix_len) != 0)
            continue;

        char *candidate = g_strndup((const char *)data + start, run);
        if (!found) {
            found = candidate;
        } else if (strcmp(found, candidate) != 0) {
            g_free(candidate);
            g_free(found);
            return NULL;
        } else {
            g_free(candidate);
        }
    }
    return found;
}

char *
ns_freenet_localize_origin(const char *target, const char *doc_url)
{
    if (!target) return NULL;

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
    if (!key || g_ascii_strcasecmp(host, key) != 0)
        return NULL;

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

    char *base = ns_freenet_gateway_base(FALSE);
    if (!base) return NULL;
    char *prefix = g_strconcat(base, "/", NS_FREENET_WEB_PATH, NULL);
    g_free(base);
    gboolean matched = g_str_has_prefix(url, prefix);
    size_t prefix_len = strlen(prefix);
    g_free(prefix);
    if (!matched) return NULL;

    const char *p = url + prefix_len;
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
