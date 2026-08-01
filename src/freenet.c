/* Northstar — the freenet: URL scheme, mapped onto a local Freenet node.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "freenet.h"

#include "config.h"

#include <string.h>

#define NS_FREENET_SCHEME_LEN 8
#define NS_FREENET_WEB_PATH   "v1/contract/web/"
#define NS_FREENET_API_PATH   "v1/"

static const char ns_freenet_base58[] =
    "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

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
    if (len < 32 || len > 64) return FALSE;
    for (size_t i = 0; i < len; i++)
        if (!strchr(ns_freenet_base58, key[i]))
            return FALSE;
    return TRUE;
}

const char *
ns_freenet_gateway(void)
{
    const ns_config *cfg = ns_config_get();
    if (cfg && cfg->freenet_gateway && *cfg->freenet_gateway)
        return cfg->freenet_gateway;
    return NS_FREENET_DEFAULT_GATEWAY;
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
ns_freenet_map(const char *url, const char *transport)
{
    char *key = NULL, *rest = NULL;
    if (!ns_freenet_split(url, &key, &rest)) return NULL;

    const char *gateway = ns_freenet_gateway();
    char *out = g_str_has_prefix(rest, NS_FREENET_API_PATH)
        ? g_strconcat(transport, gateway, "/", rest, NULL)
        : g_strconcat(transport, gateway, "/", NS_FREENET_WEB_PATH,
                      key, "/", rest, NULL);

    g_free(key);
    g_free(rest);
    return out;
}

char *
ns_freenet_to_gateway(const char *url)
{
    return ns_freenet_map(url, "http://");
}

char *
ns_freenet_to_gateway_ws(const char *url)
{
    return ns_freenet_map(url, "ws://");
}

char *
ns_freenet_from_gateway(const char *url)
{
    if (!url) return NULL;

    char *prefix = g_strconcat("http://", ns_freenet_gateway(), "/",
                               NS_FREENET_WEB_PATH, NULL);
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
