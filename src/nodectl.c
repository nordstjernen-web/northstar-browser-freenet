/* Northstar — asks the supervisor process to start and stop the Freenet node.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "nodectl.h"

#include <string.h>

#ifdef G_OS_WIN32
#include <windows.h>
#else
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

#ifdef G_OS_WIN32

/* The browser forbids itself child processes, so the node is started by the
 * supervisor, which is spawned before that mitigation is applied. The channel
 * is a pair of anonymous pipes whose child ends are inheritable; their handle
 * values reach the browser in the environment, the way the Unix build passes a
 * socketpair descriptor. The line protocol is the same on both. */
static HANDLE g_sup_cmd_read  = NULL;
static HANDLE g_sup_rsp_write = NULL;
static HANDLE g_sup_thread    = NULL;

static char *ns_nodectl_run_node_command(const char *verb, gboolean *ok);

int
ns_nodectl_supervisor_open(void)
{
    if (g_sup_cmd_read) return 0;

    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof sa);
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;

    HANDLE cmd_read = NULL, cmd_write = NULL;
    HANDLE rsp_read = NULL, rsp_write = NULL;
    if (!CreatePipe(&cmd_read, &cmd_write, &sa, 0)) return -1;
    if (!CreatePipe(&rsp_read, &rsp_write, &sa, 0)) {
        CloseHandle(cmd_read);
        CloseHandle(cmd_write);
        return -1;
    }
    /* Keep the supervisor's own ends out of the child. */
    SetHandleInformation(cmd_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(rsp_write, HANDLE_FLAG_INHERIT, 0);

    g_sup_cmd_read  = cmd_read;
    g_sup_rsp_write = rsp_write;

    g_autofree char *value =
        g_strdup_printf("%" G_GUINTPTR_FORMAT ",%" G_GUINTPTR_FORMAT,
                        (guintptr)cmd_write, (guintptr)rsp_read);
    g_setenv(NS_NODECTL_FD_ENV, value, TRUE);

    /* CreateProcessW with a NULL environment hands the child the Windows
     * environment block, which g_setenv does not necessarily reach. */
    g_autofree wchar_t *wname =
        g_utf8_to_utf16(NS_NODECTL_FD_ENV, -1, NULL, NULL, NULL);
    g_autofree wchar_t *wvalue = g_utf8_to_utf16(value, -1, NULL, NULL, NULL);
    if (wname && wvalue) SetEnvironmentVariableW(wname, wvalue);
    return 0;
}

static gboolean
ns_nodectl_write_all(HANDLE h, const char *data, gsize len)
{
    gsize sent = 0;
    while (sent < len) {
        DWORD wrote = 0;
        if (!WriteFile(h, data + sent, (DWORD)(len - sent), &wrote, NULL) ||
            wrote == 0)
            return FALSE;
        sent += wrote;
    }
    return TRUE;
}

static gpointer
ns_nodectl_supervisor_thread(gpointer data)
{
    (void)data;
    GString *pending = g_string_new(NULL);

    for (;;) {
        char buf[512];
        DWORD got = 0;
        if (!ReadFile(g_sup_cmd_read, buf, sizeof buf, &got, NULL) || got == 0)
            break;
        g_string_append_len(pending, buf, (gssize)got);
        if (pending->len > NS_NODECTL_LINE_MAX) g_string_set_size(pending, 0);

        char *nl;
        while ((nl = strchr(pending->str, '\n')) != NULL) {
            *nl = '\0';
            g_autofree char *verb = g_strdup(g_strstrip(pending->str));
            g_string_erase(pending, 0, (gssize)(nl - pending->str) + 1);

            gboolean ok = FALSE;
            g_autofree char *text = ns_nodectl_verb_is_known(verb)
                ? ns_nodectl_run_node_command(verb, &ok)
                : g_strdup("Unknown command.");
            g_autofree char *escaped = ns_nodectl_escape(text);
            g_autofree char *line =
                g_strconcat(ok ? "ok " : "err ", escaped, "\n", NULL);
            if (!ns_nodectl_write_all(g_sup_rsp_write, line, strlen(line)))
                goto done;
        }
    }
done:
    g_string_free(pending, TRUE);
    return NULL;
}

void
ns_nodectl_supervisor_listen(void)
{
    if (!g_sup_cmd_read || g_sup_thread) return;
    GThread *t = g_thread_new("ns-nodectl", ns_nodectl_supervisor_thread, NULL);
    g_sup_thread = (HANDLE)t;
}

void
ns_nodectl_supervisor_close(void)
{
    if (g_sup_cmd_read)  { CloseHandle(g_sup_cmd_read);  g_sup_cmd_read = NULL; }
    if (g_sup_rsp_write) { CloseHandle(g_sup_rsp_write); g_sup_rsp_write = NULL; }
    if (g_sup_thread) {
        g_thread_join((GThread *)g_sup_thread);
        g_sup_thread = NULL;
    }
}

static char *
ns_nodectl_find_node(void)
{
    char *found = g_find_program_in_path("freenet");
    if (found) return found;

    const char *local = g_get_user_data_dir();
    const char *programs = g_getenv("ProgramFiles");
    const char *localapp = g_getenv("LOCALAPPDATA");
    g_autofree char *a = local
        ? g_build_filename(local, "freenet", "freenet.exe", NULL) : NULL;
    g_autofree char *b = programs
        ? g_build_filename(programs, "Freenet", "freenet.exe", NULL) : NULL;
    g_autofree char *c = localapp
        ? g_build_filename(localapp, "Programs", "Freenet", "freenet.exe", NULL)
        : NULL;
    const char *candidates[] = { a, b, c, NULL };
    for (int i = 0; candidates[i]; i++)
        if (g_file_test(candidates[i], G_FILE_TEST_IS_REGULAR))
            return g_strdup(candidates[i]);
    return NULL;
}

/* The browser applies ProcessChildProcessPolicy = NoChildProcessCreation to
 * itself, so it cannot start anything. Ask the kernel rather than guess. */
static gboolean
ns_nodectl_child_processes_blocked(void)
{
    typedef BOOL (WINAPI *ns_gmp_fn)(HANDLE, int, PVOID, SIZE_T);
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    if (!k) return FALSE;
    ns_gmp_fn get = (ns_gmp_fn)(void *)
        GetProcAddress(k, "GetProcessMitigationPolicy");
    if (!get) return FALSE;

    DWORD policy = 0;
    if (!get(GetCurrentProcess(), 13, &policy, sizeof policy))
        return FALSE;
    return (policy & 0x1u) != 0;
}

static gboolean
ns_nodectl_client_handles(HANDLE *write_end, HANDLE *read_end)
{
    const char *value = g_getenv(NS_NODECTL_FD_ENV);
    if (!value || !*value) return FALSE;
    char *end = NULL;
    guintptr w = (guintptr)g_ascii_strtoull(value, &end, 10);
    if (!end || *end != ',') return FALSE;
    guintptr r = (guintptr)g_ascii_strtoull(end + 1, &end, 10);
    if (!end || *end || !w || !r) return FALSE;
    *write_end = (HANDLE)w;
    *read_end  = (HANDLE)r;
    return TRUE;
}

gboolean
ns_nodectl_available(void)
{
    HANDLE w = NULL, r = NULL;
    if (ns_nodectl_client_handles(&w, &r)) return TRUE;
    if (ns_nodectl_child_processes_blocked()) return FALSE;
    g_autofree char *node = ns_nodectl_find_node();
    return node != NULL;
}

const char *
ns_nodectl_mechanism(void)
{
    HANDLE w = NULL, r = NULL;
    if (ns_nodectl_client_handles(&w, &r))
        return "Northstar asks its supervisor process to run the node's own "
               "service commands.";
    return "Northstar runs the node's own service commands.";
}

static gboolean
ns_nodectl_ask_supervisor(const char *verb, char **output)
{
    HANDLE write_end = NULL, read_end = NULL;
    if (!ns_nodectl_client_handles(&write_end, &read_end)) return FALSE;

    static GMutex lock;
    g_mutex_lock(&lock);

    g_autofree char *request = g_strconcat(verb, "\n", NULL);
    gboolean ok = ns_nodectl_write_all(write_end, request, strlen(request));

    GString *reply = g_string_new(NULL);
    while (ok && !strchr(reply->str, '\n')) {
        char buf[512];
        DWORD got = 0;
        if (!ReadFile(read_end, buf, sizeof buf, &got, NULL) || got == 0) {
            ok = FALSE;
            break;
        }
        g_string_append_len(reply, buf, (gssize)got);
        if (reply->len > NS_NODECTL_LINE_MAX) ok = FALSE;
    }
    g_mutex_unlock(&lock);

    if (!ok) {
        g_string_free(reply, TRUE);
        if (output) *output = g_strdup("The supervisor did not answer.");
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

gboolean
ns_nodectl_run(const char *verb, char **output)
{
    if (output) *output = NULL;
    if (!ns_nodectl_verb_is_known(verb)) {
        if (output) *output = g_strdup("Unknown command.");
        return FALSE;
    }

    {
        HANDLE w = NULL, r = NULL;
        if (ns_nodectl_client_handles(&w, &r)) {
            if (g_strcmp0(verb, "ping") == 0) {
                if (output) *output = g_strdup("ok");
                return TRUE;
            }
            return ns_nodectl_ask_supervisor(verb, output);
        }
    }

    if (ns_nodectl_child_processes_blocked()) {
        if (output)
            *output = g_strdup("Northstar blocks itself from starting any "
                               "process, so it cannot run the node. Use the "
                               "node's own service or tray icon, or start it "
                               "with NS_NO_WIN32_MITIGATIONS set.");
        return FALSE;
    }

    g_autofree char *node = ns_nodectl_find_node();
    if (!node) {
        if (output)
            *output = g_strdup("No freenet executable was found. Install a "
                               "node from freenet.org, or put freenet.exe on "
                               "PATH.");
        return FALSE;
    }
    if (g_strcmp0(verb, "ping") == 0) {
        if (output) *output = g_strdup("ok");
        return TRUE;
    }

    gboolean ok = FALSE;
    char *text = ns_nodectl_run_node_command(verb, &ok);
    if (output) *output = text;
    else        g_free(text);
    return ok;
}

/* CreateProcessW rather than g_spawn_sync: GLib routes a Windows spawn
 * through a gspawn-win64-helper binary, which is itself a child process.
 * watchdog.c starts its child the same way. */
static char *
ns_nodectl_run_node_command(const char *verb, gboolean *ok)
{
    *ok = FALSE;
    g_autofree char *node = ns_nodectl_find_node();
    if (!node)
        return g_strdup("No freenet executable was found. Install a node "
                        "from freenet.org, or put freenet.exe on PATH.");

    g_autofree wchar_t *app = g_utf8_to_utf16(node, -1, NULL, NULL, NULL);
    g_autofree char *line = g_strdup_printf("\"%s\" service %s", node, verb);
    g_autofree wchar_t *cmd = g_utf8_to_utf16(line, -1, NULL, NULL, NULL);
    if (!app || !cmd)
        return g_strdup("Could not build the node command.");

    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof sa);
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;

    HANDLE read_end = NULL, write_end = NULL;
    if (!CreatePipe(&read_end, &write_end, &sa, 0))
        return g_strdup("Could not open a pipe to the node.");
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    memset(&pi, 0, sizeof pi);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write_end;
    si.hStdError = write_end;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    BOOL started = CreateProcessW(app, cmd, NULL, NULL, TRUE,
                                  CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    DWORD spawn_error = started ? 0 : GetLastError();
    CloseHandle(write_end);

    if (!started) {
        CloseHandle(read_end);
        g_autofree char *msg = g_win32_error_message((gint)spawn_error);
        return g_strdup_printf("Could not run %s service %s: %s",
                               node, verb, msg ? msg : "failed");
    }

    GString *text = g_string_new(NULL);
    for (;;) {
        char buf[1024];
        DWORD got = 0;
        if (!ReadFile(read_end, buf, sizeof buf, &got, NULL) || got == 0)
            break;
        g_string_append_len(text, buf, (gssize)got);
    }
    CloseHandle(read_end);

    WaitForSingleObject(pi.hProcess, 20000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (text->len > NS_NODECTL_OUTPUT_MAX)
        g_string_truncate(text, NS_NODECTL_OUTPUT_MAX);
    g_strstrip(text->str);
    g_string_set_size(text, strlen(text->str));
    *ok = code == 0;
    if (!text->len)
        g_string_append(text, *ok ? "done" : "the node reported no output");
    return g_string_free(text, FALSE);
}

#else

static int     g_supervisor_fd = -1;
static int     g_supervisor_peer_fd = -1;
static GString *g_supervisor_buf;


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

const char *
ns_nodectl_mechanism(void)
{
    return "Northstar asks its supervisor process to run the node's own "
           "service commands.";
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
