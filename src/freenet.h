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
const char *ns_freenet_gateway(void);

char *ns_freenet_canonical_url(const char *input);
char *ns_freenet_key_of(const char *url);
char *ns_freenet_to_gateway(const char *url);
char *ns_freenet_to_gateway_ws(const char *url);
char *ns_freenet_from_gateway(const char *url);

G_END_DECLS

#endif
