/* Northstar — asks the supervisor process to start and stop the Freenet node.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NS_NODECTL_H
#define NS_NODECTL_H

#include <glib.h>

G_BEGIN_DECLS

#define NS_NODECTL_FD_ENV "NS_NODE_CONTROL_FD"

int      ns_nodectl_supervisor_open(void);
void     ns_nodectl_supervisor_listen(void);
void     ns_nodectl_supervisor_close(void);

gboolean ns_nodectl_verb_is_known(const char *verb);
gboolean ns_nodectl_available(void);
gboolean ns_nodectl_run(const char *verb, char **output);

G_END_DECLS

#endif
