/* Northstar — asks the supervisor process to start and stop the Freenet node.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "nodectl.h"

#include <string.h>

#ifndef G_OS_WIN32
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <glib-unix.h>
#endif

#define NS_NODECTL_LINE_MAX     8192
#define NS_NODECTL_OUTPUT_MAX   4096
#define NS_NODECTL_COMMAND_SECS 60
#define NS_NODECTL_REPLY_SECS   90

static const char *const ns_nodectl_verbs[] = {
    "ping", "status", "start", "stop", "restart", NULL
};

gboolean
ns_nodectl_verb_is_known(const char *verb)
{
    for (int i = 0; verb && ns_nodectl_verbs[i]; i++)
        if (g_strcmp0(verb, ns_nodectl_verbs[i]) == 0)
            return TRUE;
    return FALSE;
}

#ifdef G_OS_WIN32

int  ns_nodectl_supervisor_open(void)   { return -1; }
void ns_nodectl_supervisor_listen(void) { }
void ns_nodectl_supervisor_close(void)  { }

gboolean
ns_nodectl_available(void)
{
    return FALSE;
}

gboolean
ns_nodectl_run(const char *verb, char **output)
{
    (void)verb;
    if (output)
        *output = g_strdup("The Freenet node is managed by its own Windows "
                           "service; use the tray icon it installs.");
    return FALSE;
}

#else

static int     g_supervisor_fd = -1;
static int     g_supervisor_peer_fd = -1;
static GString *g_supervisor_buf;

static char *
ns_nodectl_escape(const char *text)
{
    GString *out = g_string_new(NULL);
    for (const char *p = text ? text : ""; *p; p++) {
        if (*p == '\\')      g_string_append(out, "\\\\");
        else if (*p == '\n') g_string_append(out, "\\n");
        else if (*p == '\r') continue;
        else                 g_string_append_c(out, *p);
    }
    return g_string_free(out, FALSE);
}

static char *
ns_nodectl_unescape(const char *text)
{
    GString *out = g_string_new(NULL);
    for (const char *p = text ? text : ""; *p; p++) {
        if (*p != '\\') {
            g_string_append_c(out, *p);
            continue;
        }
        p++;
        if (*p == 'n')       g_string_append_c(out, '\n');
        else if (*p == '\\') g_string_append_c(out, '\\');
        else if (!*p)        break;
        else                 g_string_append_c(out, *p);
    }
    return g_string_free(out, FALSE);
}

static char *
ns_nodectl_find_node(void)
{
    char *found = g_find_program_in_path("freenet");
    if (found) return found;

    const char *home = g_get_home_dir();
    g_autofree char *local = home
        ? g_build_filename(home, ".local", "bin", "freenet", NULL) : NULL;
    const char *candidates[] = {
        local, "/usr/local/bin/freenet", "/opt/homebrew/bin/freenet",
        "/usr/bin/freenet", NULL
    };
    for (int i = 0; candidates[i]; i++)
        if (candidates[i] && g_file_test(candidates[i], G_FILE_TEST_IS_EXECUTABLE))
            return g_strdup(candidates[i]);
    return NULL;
}

typedef struct {
    GMainLoop *loop;
    gint       status;
    gboolean   exited;
    GPid       pid;
    guint      timeout_id;
} ns_nodectl_wait;

static void
ns_nodectl_child_exited(GPid pid, gint status, gpointer user_data)
{
    ns_nodectl_wait *wait = user_data;
    (void)pid;
    wait->status = status;
    wait->exited = TRUE;
    g_main_loop_quit(wait->loop);
}

static gboolean
ns_nodectl_child_timeout(gpointer user_data)
{
    ns_nodectl_wait *wait = user_data;
    wait->timeout_id = 0;
    kill((pid_t)wait->pid, SIGKILL);
    return G_SOURCE_REMOVE;
}

static char *
ns_nodectl_run_node_command(const char *verb, gboolean *ok)
{
    g_autofree char *node = ns_nodectl_find_node();
    if (!node) {
        *ok = FALSE;
        return g_strdup("No freenet binary found. Install a node with "
                        "curl -fsSL https://freenet.org/install.sh | sh");
    }

    char *argv[] = { node, (char *)"service", (char *)verb, NULL };
    GPid pid = 0;
    int out_fd = -1, err_fd = -1;
    GError *error = NULL;
    if (!g_spawn_async_with_pipes(NULL, argv, NULL,
                                  G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL,
                                  &pid, NULL, &out_fd, &err_fd, &error)) {
        char *message = g_strdup_printf("Could not run %s service %s: %s",
                                        node, verb,
                                        error ? error->message : "unknown error");
        g_clear_error(&error);
        *ok = FALSE;
        return message;
    }

    GMainContext *context = g_main_context_new();
    ns_nodectl_wait wait = { .loop = g_main_loop_new(context, FALSE),
                             .pid = pid };
    GSource *child = g_child_watch_source_new(pid);
    g_source_set_callback(child, G_SOURCE_FUNC(ns_nodectl_child_exited),
                          &wait, NULL);
    g_source_attach(child, context);
    GSource *timer = g_timeout_source_new_seconds(NS_NODECTL_COMMAND_SECS);
    g_source_set_callback(timer, ns_nodectl_child_timeout, &wait, NULL);
    g_source_attach(timer, context);
    wait.timeout_id = 1;
    g_main_loop_run(wait.loop);
    if (wait.timeout_id) g_source_destroy(timer);
    g_source_unref(timer);
    g_source_destroy(child);
    g_source_unref(child);
    g_main_loop_unref(wait.loop);
    g_main_context_unref(context);
    g_spawn_close_pid(pid);

    GString *text = g_string_new(NULL);
    for (int which = 0; which < 2; which++) {
        int fd = which == 0 ? out_fd : err_fd;
        if (fd < 0) continue;
        char buf[1024];
        gssize got;
        while (text->len < NS_NODECTL_OUTPUT_MAX &&
               (got = read(fd, buf, sizeof buf)) > 0)
            g_string_append_len(text, buf, got);
        close(fd);
    }
    if (text->len > NS_NODECTL_OUTPUT_MAX)
        g_string_truncate(text, NS_NODECTL_OUTPUT_MAX);
    g_strstrip(text->str);
    g_string_set_size(text, strlen(text->str));

    *ok = wait.exited && WIFEXITED(wait.status) && WEXITSTATUS(wait.status) == 0;
    if (!text->len)
        g_string_append(text, *ok ? "done" : "the command reported no output");
    return g_string_free(text, FALSE);
}

static void
ns_nodectl_reply(const char *status, const char *text)
{
    g_autofree char *escaped = ns_nodectl_escape(text);
    g_autofree char *line = g_strconcat(status, " ", escaped, "\n", NULL);
    gsize len = strlen(line), sent = 0;
    while (sent < len) {
        gssize wrote = write(g_supervisor_fd, line + sent, len - sent);
        if (wrote > 0) { sent += (gsize)wrote; continue; }
        if (wrote < 0 && errno == EINTR) continue;
        break;
    }
}

static void
ns_nodectl_dispatch(const char *verb)
{
    if (!ns_nodectl_verb_is_known(verb)) {
        ns_nodectl_reply("err", "unknown command");
        return;
    }
    if (g_strcmp0(verb, "ping") == 0) {
        ns_nodectl_reply("ok", "ready");
        return;
    }
    gboolean ok = FALSE;
    g_autofree char *text = ns_nodectl_run_node_command(verb, &ok);
    ns_nodectl_reply(ok ? "ok" : "err", text);
}

static gboolean
ns_nodectl_supervisor_readable(gint fd, GIOCondition condition,
                               gpointer user_data)
{
    (void)user_data;
    if (condition & (G_IO_ERR | G_IO_NVAL)) return G_SOURCE_REMOVE;

    char buf[1024];
    gssize got = read(fd, buf, sizeof buf);
    if (got < 0) return (errno == EAGAIN || errno == EINTR)
        ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
    if (got == 0) return G_SOURCE_CONTINUE;

    g_string_append_len(g_supervisor_buf, buf, got);
    for (char *nl; (nl = strchr(g_supervisor_buf->str, '\n')); ) {
        *nl = '\0';
        g_autofree char *verb = g_strdup(g_supervisor_buf->str);
        g_string_erase(g_supervisor_buf, 0,
                       (gssize)(nl - g_supervisor_buf->str) + 1);
        g_strstrip(verb);
        if (*verb) ns_nodectl_dispatch(verb);
    }
    if (g_supervisor_buf->len > NS_NODECTL_LINE_MAX)
        g_string_truncate(g_supervisor_buf, 0);
    return G_SOURCE_CONTINUE;
}

int
ns_nodectl_supervisor_open(void)
{
    if (g_supervisor_fd >= 0) return g_supervisor_peer_fd;

    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) return -1;
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    g_supervisor_fd = fds[0];
    g_supervisor_peer_fd = fds[1];
    return g_supervisor_peer_fd;
}

void
ns_nodectl_supervisor_listen(void)
{
    if (g_supervisor_fd < 0 || g_supervisor_buf) return;
    g_supervisor_buf = g_string_new(NULL);
    g_unix_fd_add(g_supervisor_fd, G_IO_IN | G_IO_ERR,
                  ns_nodectl_supervisor_readable, NULL);
}

void
ns_nodectl_supervisor_close(void)
{
    if (g_supervisor_fd >= 0) close(g_supervisor_fd);
    if (g_supervisor_peer_fd >= 0) close(g_supervisor_peer_fd);
    g_supervisor_fd = g_supervisor_peer_fd = -1;
    if (g_supervisor_buf) g_string_free(g_supervisor_buf, TRUE);
    g_supervisor_buf = NULL;
}

static int
ns_nodectl_client_fd(void)
{
    static int fd = -2;
    if (fd != -2) return fd;

    fd = -1;
    const char *value = g_getenv(NS_NODECTL_FD_ENV);
    if (value && *value) {
        char *end = NULL;
        gint64 parsed = g_ascii_strtoll(value, &end, 10);
        if (end && !*end && parsed > 2 && parsed < 1024 &&
            fcntl((int)parsed, F_GETFD) != -1)
            fd = (int)parsed;
    }
    return fd;
}

gboolean
ns_nodectl_available(void)
{
    return ns_nodectl_client_fd() >= 0;
}

gboolean
ns_nodectl_run(const char *verb, char **output)
{
    static GMutex lock;
    if (output) *output = NULL;

    int fd = ns_nodectl_client_fd();
    if (fd < 0 || !ns_nodectl_verb_is_known(verb)) {
        if (output)
            *output = g_strdup(fd < 0
                ? "Northstar is not running under its supervisor, so it "
                  "cannot start or stop the node."
                : "unknown command");
        return FALSE;
    }

    g_mutex_lock(&lock);
    g_autofree char *line = g_strconcat(verb, "\n", NULL);
    gsize len = strlen(line), sent = 0;
    gboolean ok = TRUE;
    while (sent < len) {
        gssize wrote = write(fd, line + sent, len - sent);
        if (wrote > 0) { sent += (gsize)wrote; continue; }
        if (wrote < 0 && errno == EINTR) continue;
        ok = FALSE;
        break;
    }

    GString *reply = g_string_new(NULL);
    gint64 deadline = g_get_monotonic_time() +
        (gint64)NS_NODECTL_REPLY_SECS * G_USEC_PER_SEC;
    while (ok && !strchr(reply->str, '\n')) {
        gint64 left_us = deadline - g_get_monotonic_time();
        if (left_us <= 0) { ok = FALSE; break; }
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int ready = poll(&pfd, 1, (int)(left_us / 1000));
        if (ready < 0) {
            if (errno == EINTR) continue;
            ok = FALSE;
            break;
        }
        if (ready == 0) { ok = FALSE; break; }
        char buf[1024];
        gssize got = read(fd, buf, sizeof buf);
        if (got > 0) {
            g_string_append_len(reply, buf, got);
            if (reply->len > NS_NODECTL_LINE_MAX) { ok = FALSE; break; }
            continue;
        }
        if (got < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        ok = FALSE;
    }
    g_mutex_unlock(&lock);

    if (!ok) {
        g_string_free(reply, TRUE);
        if (output)
            *output = g_strdup("The supervisor did not answer.");
        return FALSE;
    }

    char *nl = strchr(reply->str, '\n');
    if (nl) *nl = '\0';
    gboolean succeeded = g_str_has_prefix(reply->str, "ok");
    const char *text = reply->str + (succeeded ? 2 : 3);
    while (*text == ' ') text++;
    if (output) *output = ns_nodectl_unescape(text);
    g_string_free(reply, TRUE);
    return succeeded;
}

#endif
