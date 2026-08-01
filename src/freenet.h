/* Northstar — the freenet: URL scheme, mapped onto a local Freenet node.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NS_FREENET_H
#define NS_FREENET_H

#include <glib.h>

G_BEGIN_DECLS

#define NS_FREENET_DEFAULT_GATEWAY "127.0.0.1:7509"

gboolean    ns_freenet_is_url(const char *url);
gboolean    ns_freenet_key_is_valid(const char *key);
gboolean    ns_freenet_key_is_full(const char *key);
const char *ns_freenet_gateway(void);
char       *ns_freenet_gateway_base(gboolean websocket);

char *ns_freenet_canonical_url(const char *input);
char *ns_freenet_key_of(const char *url);
char *ns_freenet_to_gateway(const char *url);
char *ns_freenet_to_gateway_ws(const char *url);
char *ns_freenet_from_gateway(const char *url);
char *ns_freenet_localize_csp(const char *csp, const char *url);
char *ns_freenet_node_error(long status, const guint8 *body, gsize len);
char *ns_freenet_localize_origin(const char *target, const char *doc_url);
char *ns_freenet_with_key(const char *url, const char *key);
char *ns_freenet_find_key_with_prefix(const guint8 *data, gsize len,
                                      const char *prefix);
GByteArray *ns_freenet_node_diagnostics(void);

G_END_DECLS

#endif
