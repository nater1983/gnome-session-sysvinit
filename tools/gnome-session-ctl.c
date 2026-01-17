/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 * gnome-session-tl.c - Small utility program to manage GNOME session leader
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 */

#include <config.h>
#include <locale.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <glib.h>
#include <glib-unix.h>
#include <gio/gio.h>
#include <glib/gi18n.h>

#include <syslog.h>

#ifdef USE_OPENRC
# include <rc.h>
#endif

#include <fcntl.h>

typedef struct {
    GMainLoop *loop;
    gint fifo_fd;
} MonitorLeader;

/* ------------------ SysVinit / OpenRC support ------------------ */

#ifdef USE_OPENRC
static gboolean
async_run_cmd(gchar **argv, GError **error)
{
    return g_spawn_async(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, error);
}
#endif

static void
do_start_unit(const gchar *unit)
{
    g_autoptr(GError) error = NULL;

#ifdef USE_OPENRC
    gchar *rl_argv[] = { "/usr/bin/openrc", "-U", "default", NULL };
    if (!async_run_cmd(rl_argv, &error))
        g_warning("Failed to start OpenRC unit: %s", error ? error->message : "(no message)");
#else
    gchar *argv[] = { "/etc/init.d/gnome-session-shutdown", "start", NULL };
    if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, &error))
        g_warning("Failed to start SysVinit unit: %s", error ? error->message : "(no message)");
#endif
}

/* ------------------ D-Bus Helpers ------------------ */

static GDBusConnection *
get_session_bus(void)
{
    g_autoptr(GError) error = NULL;
    GDBusConnection *bus;

    bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (!bus)
        g_warning("Couldn't connect to session bus: %s", error->message);

    return bus;
}

static void
do_signal_init(void)
{
    g_autoptr(GDBusConnection) connection = get_session_bus();
    if (!connection)
        return;

    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) reply = g_dbus_connection_call_sync(connection,
                                                            "org.gnome.SessionManager",
                                                            "/org/gnome/SessionManager",
                                                            "org.gnome.SessionManager",
                                                            "Initialized",
                                                            NULL,
                                                            NULL,
                                                            G_DBUS_CALL_FLAGS_NO_AUTO_START,
                                                            -1, NULL, &error);
    if (error)
        g_warning("Failed to call signal initialization: %s", error->message);
}

/* ------------------ FIFO Monitoring ------------------ */

static gboolean
leader_term_or_int_signal_cb(gpointer user_data)
{
    MonitorLeader *data = (MonitorLeader*)user_data;
    g_main_loop_quit(data->loop);
    return G_SOURCE_REMOVE;
}

static gboolean
leader_fifo_io_cb(gint fd, GIOCondition condition, gpointer user_data)
{
    MonitorLeader *data = (MonitorLeader*)user_data;

    /* Notify monitor / session manager about STOPPING */
    sd_notify(0, "STOPPING=1");

    if (condition & G_IO_IN) {
        char buf[1];
        read(data->fifo_fd, buf, 1);
        g_main_loop_quit(data->loop);
    }

    if (condition & G_IO_HUP) {
        g_main_loop_quit(data->loop);
    }

    return G_SOURCE_CONTINUE;
}

static void
do_monitor_leader(void)
{
    MonitorLeader data;
    g_autofree char *fifo_name = NULL;
    int res;

    data.loop = g_main_loop_new(NULL, TRUE);

    fifo_name = g_strdup_printf("%s/gnome-session-leader-fifo",
                                g_get_user_runtime_dir());
    res = mkfifo(fifo_name, 0666);
    if (res < 0 && errno != EEXIST)
        g_warning("Error creating FIFO: %m");

    data.fifo_fd = g_open(fifo_name, O_RDONLY | O_CLOEXEC, 0666);
    if (data.fifo_fd >= 0) {
        struct stat buf;
        res = fstat(data.fifo_fd, &buf);
        if (res < 0 || !(buf.st_mode & S_IFIFO)) {
            g_warning("FD is not a FIFO, cannot monitor leader");
            close(data.fifo_fd);
            data.fifo_fd = -1;
        } else {
            sd_notify(0, "STATUS=Watching session leader");
            g_unix_fd_add(data.fifo_fd, G_IO_HUP | G_IO_IN, leader_fifo_io_cb, &data);
        }
    } else {
        g_warning("Failed to open FIFO: %m");
    }

    g_unix_signal_add(SIGTERM, leader_term_or_int_signal_cb, &data);
    g_unix_signal_add(SIGINT, leader_term_or_int_signal_cb, &data);

    g_main_loop_run(data.loop);
    g_main_loop_unref(data.loop);
}

/* ------------------ Main ------------------ */

int main(int argc, char *argv[])
{
    g_autoptr(GError) error = NULL;
    static gboolean opt_shutdown = FALSE;
    static gboolean opt_monitor = FALSE;
    static gboolean opt_signal_init = FALSE;

    GOptionContext *ctx;
    static const GOptionEntry options[] = {
        { "shutdown", '\0', 0, G_OPTION_ARG_NONE, &opt_shutdown, "Start gnome-session-shutdown service", NULL },
        { "monitor", '\0', 0, G_OPTION_ARG_NONE, &opt_monitor, "Monitor leader FIFO for session shutdown", NULL },
        { "signal-init", '\0', 0, G_OPTION_ARG_NONE, &opt_signal_init, "Signal initialization done to gnome-session", NULL },
        { NULL }
    };

    /* i18n init */
    setlocale(LC_ALL, "");
    bindtextdomain(GETTEXT_PACKAGE, LOCALE_DIR);
    bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
    textdomain(GETTEXT_PACKAGE);

    ctx = g_option_context_new("");
    g_option_context_add_main_entries(ctx, options, GETTEXT_PACKAGE);
    if (!g_option_context_parse(ctx, &argc, &argv, &error)) {
        g_warning("Unable to start: %s", error->message);
        exit(1);
    }
    g_option_context_free(ctx);

    /* Exactly one option must be provided */
    int opt_count = (opt_shutdown ? 1 : 0) + (opt_monitor ? 1 : 0) + (opt_signal_init ? 1 : 0);
    if (opt_count != 1) {
        g_printerr("Program needs exactly one parameter\n");
        exit(1);
    }

    sd_notify(0, "READY=1");

    if (opt_signal_init) {
        do_signal_init();
    } else if (opt_shutdown) {
        do_start_unit("gnome-session-shutdown.target");
    } else if (opt_monitor) {
        do_monitor_leader();
        /* Ensure shutdown after monitor exits */
        do_start_unit("gnome-session-shutdown.target");
    } else {
        g_assert_not_reached();
    }

    return 0;
}
