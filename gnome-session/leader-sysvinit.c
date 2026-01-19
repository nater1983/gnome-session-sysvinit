/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2006 Novell, Inc.
 * Copyright (C) 2008 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <string.h>
#include <config.h>

#include <glib.h>
#include <glib-unix.h>
#include <gio/gio.h>
#include <sys/syslog.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

typedef struct {
        GDBusConnection *session_bus;
        GMainLoop *loop;
        int fifo_fd;
        GDBusProxy *awaiting_shutdown;
} Leader;

static void
leader_clear (Leader *ctx)
{
        g_clear_object (&ctx->session_bus);
        g_clear_pointer (&ctx->loop, g_main_loop_unref);
        if (ctx->fifo_fd >= 0)
            close (ctx->fifo_fd);
        g_clear_object (&ctx->awaiting_shutdown);
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (Leader, leader_clear);

static gboolean
async_run_cmd (gchar **argv, GError **error)
{
        return g_spawn_async(NULL,
                             argv,
                             NULL,
                             G_SPAWN_DEFAULT,
                             NULL,
                             NULL,
                             NULL,
                             error);
}

/* -------------------- SysVinit-compatible unit handling -------------------- */

/**
 * Run a SysVinit unit (GNOME Settings Daemon plugin) with the given action.
 *
 * Works with wrappers that:
 * 1. Automatically start the plugin (action ignored), or
 * 2. Respect start/stop arguments.
 */
static gboolean
sysvinit_unit_action(const char *unit,
                     const char *action,
                     GError    **error)
{
    if (!unit || !action)
        return FALSE;

    /* Build command: call the wrapper with the action */
    g_autofree char *cmd = g_strdup_printf("/etc/xdg/gnome/%s %s", unit, action);
    gchar *argv[] = { "/bin/sh", "-c", cmd, NULL };

    /* Run asynchronously */
    gboolean res = async_run_cmd(argv, error);
    if (!res)
        g_warning("Failed to run unit %s %s: %s",
                  unit,
                  action,
                  error && *error ? (*error)->message : "(no message)");

    return res;
}

/* Wrapper helpers for start/stop */
static gboolean
sysvinit_start_unit(const char *unit, GError **error)
{
    return sysvinit_unit_action(unit, "start", error);
}

static gboolean
sysvinit_stop_unit(const char *unit, GError **error)
{
    return sysvinit_unit_action(unit, "stop", error);
}

/* --------------------------------------------------------------------------- */

static gboolean
leader_term_or_int_signal_cb (gpointer data)
{
        Leader *ctx = data;

        g_debug ("Session termination requested");

        if (write (ctx->fifo_fd, "S", 1) < 0) {
                g_warning ("Failed to signal shutdown to monitor: %m");
                g_main_loop_quit (ctx->loop);
        }

        return G_SOURCE_REMOVE;
}

static void
graphical_session_pre_state_changed_cb (GDBusProxy  *proxy,
                                        GVariant    *changed_properties,
                                        char       **invalidated_properties,
                                        gpointer     data)
{
        Leader *ctx = data;
        g_autoptr (GVariant) value = NULL;
        const char *state;

        value = g_variant_lookup_value (changed_properties, "ActiveState", NULL);
        if (value == NULL)
                return;
        g_variant_get (value, "&s", &state);

        if (g_strcmp0 (state, "inactive") == 0) {
                g_debug ("Session services now inactive, quitting");
                g_main_loop_quit (ctx->loop);
        }
}

static gboolean
monitor_hangup_cb (int          fd,
                   GIOCondition condition,
                   gpointer     user_data)
{
        Leader *ctx = user_data;
        g_autoptr (GVariant) unit = NULL;
        const char *unit_path = NULL;
        g_autoptr (GDBusProxy) proxy = NULL;
        g_autoptr (GVariant) value = NULL;
        g_autoptr (GError) error = NULL;

        g_debug ("Services have begun stopping, waiting for them to finish stopping");

        unit = g_dbus_connection_call_sync (ctx->session_bus,
                                            "org.freedesktop.systemd1",
                                            "/org/freedesktop/systemd1",
                                            "org.freedesktop.systemd1.Manager",
                                            "GetUnit",
                                            g_variant_new ("(s)", "graphical-session-pre.target"),
                                            G_VARIANT_TYPE ("(o)"),
                                            G_DBUS_CALL_FLAGS_NONE,
                                            -1,
                                            NULL,
                                            &error);
        if (!unit) {
                g_warning ("Could not get unit for graphical-session-pre.target: %s",
                           error->message);
                g_main_loop_quit (ctx->loop);
                return G_SOURCE_REMOVE;
        }

        g_variant_get (unit, "(&o)", &unit_path);

        proxy = g_dbus_proxy_new_sync (ctx->session_bus,
                                       G_DBUS_PROXY_FLAGS_NONE,
                                       NULL,
                                       "org.freedesktop.systemd1",
                                       unit_path,
                                       "org.freedesktop.systemd1.Unit",
                                       NULL,
                                       &error);
        if (!proxy) {
                g_warning ("Could not get proxy for graphical-session-pre.target unit: %s",
                           error->message);
                g_main_loop_quit (ctx->loop);
                return G_SOURCE_REMOVE;
        }

        value = g_dbus_proxy_get_cached_property (proxy, "ActiveState");

        if (value) {
                const char *state;

                g_variant_get (value, "&s", &state);

                if (g_strcmp0 (state, "inactive") == 0) {
                        g_debug ("State of graphical-session-pre.target unit already inactive quitting");
                        g_main_loop_quit (ctx->loop);
                        return G_SOURCE_REMOVE;
                }
                g_debug ("State of graphical-session-pre.target unit is '%s', waiting for it to go inactive", state);
        } else {
                g_debug ("State of graphical-session-pre.target unit is unknown, waiting for it to go inactive");
        }

        g_signal_connect (proxy,
                          "g-properties-changed",
                          G_CALLBACK (graphical_session_pre_state_changed_cb),
                          ctx);

        ctx->awaiting_shutdown = g_steal_pointer (&proxy);

        return G_SOURCE_REMOVE;
}

static void
debug_logger (gchar const *log_domain,
              GLogLevelFlags log_level,
              gchar const *message,
              gpointer user_data)
{
        printf ("%s\n", message);
        syslog (LOG_INFO, "%s", message);
}

int
main (int argc, char **argv)
{
        g_log_set_default_handler(debug_logger, NULL);
        g_autoptr (GError) error = NULL;
        g_auto (Leader) ctx = { .fifo_fd = -1 };
        const char *session_name = NULL;
        const char *debug_string = NULL;
        g_autofree char *fifo_path = NULL;
        g_autofree char *home_dir = NULL;
        g_autofree char *config_dir = NULL;
        struct stat statbuf;

        if (argc < 2)
            g_error ("No session name was specified");
        session_name = argv[1];

        char const *user = g_getenv("USER");
        if (!user)
                user = "gdm-greeter";

        g_info("User is: %s", user);
        if (strncmp(user, "gdm-greeter", sizeof("gdm-greeter")) == 0)
        {
                home_dir = g_strdup_printf("/var/lib/%s", user);
                config_dir = g_strdup_printf("%s/.config", home_dir);
                g_setenv("XDG_CONFIG_HOME", config_dir, TRUE);
                g_setenv("HOME", home_dir, TRUE);
        }
        else
                g_warning("The gdm-greeter-{1,2,3,4} user wasn't found. Expect stuff to break.");

        ctx.loop = g_main_loop_new (NULL, TRUE);

        ctx.session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
        if (ctx.session_bus == NULL)
                g_error ("Failed to obtain session bus: %s", error->message);

        const char *session_type = g_getenv("XDG_SESSION_TYPE");
        if (session_type && strcmp(session_type, "tty") == 0)
                session_type = "wayland";

        fifo_path = g_build_filename (g_get_user_runtime_dir (),
                                      "gnome-session-leader-fifo",
                                      NULL);
        if (mkfifo (fifo_path, 0666) < 0 && errno != EEXIST)
                g_warning ("Failed to create leader FIFO: %m");

        ctx.fifo_fd = g_open (fifo_path, O_WRONLY | O_CLOEXEC, 0666);
        if (ctx.fifo_fd < 0)
                g_error ("Failed to watch session: open failed: %m");
        if (fstat (ctx.fifo_fd, &statbuf) < 0)
                g_error ("Failed to watch session: fstat failed: %m");
        else if (!(statbuf.st_mode & S_IFIFO))
                g_error ("Failed to watch session: FD is not a FIFO");

        g_unix_fd_add (ctx.fifo_fd, G_IO_HUP, (GUnixFDSourceFunc) monitor_hangup_cb, &ctx);
        g_unix_signal_add (SIGHUP, leader_term_or_int_signal_cb, &ctx);
        g_unix_signal_add (SIGTERM, leader_term_or_int_signal_cb, &ctx);
        g_unix_signal_add (SIGINT, leader_term_or_int_signal_cb, &ctx);

        g_main_loop_run (ctx.loop);
        return 0;
}
