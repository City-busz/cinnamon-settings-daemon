/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2007 William Jon McCann <mccann@jhu.edu>
 * Copyright (C) 2011 Richard Hughes <richard@hughsie.com>
 * Copyright (C) 2011 Ritesh Khadgaray <khadgaray@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 */

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/wait.h>
#include <glib/gi18n.h>
#include <gdk/gdkx.h>
#include <gtk/gtk.h>
#include <libupower-glib/upower.h>
#include <libnotify/notify.h>
#include <canberra-gtk.h>

#define GNOME_DESKTOP_USE_UNSTABLE_API
#include <libgnome-desktop/gnome-rr.h>

#include "gpm-common.h"
#include "gpm-phone.h"
#include "gpm-idletime.h"
#include "gnome-settings-profile.h"
#include "gnome-settings-session.h"
#include "gsd-enums.h"
#include "gsd-power-manager.h"
#include "gsd-power-helper.h"

#define GNOME_SESSION_DBUS_NAME                 "org.gnome.SessionManager"
#define GNOME_SESSION_DBUS_PATH                 "/org/gnome/SessionManager"
#define GNOME_SESSION_DBUS_PATH_PRESENCE        "/org/gnome/SessionManager/Presence"
#define GNOME_SESSION_DBUS_INTERFACE            "org.gnome.SessionManager"
#define GNOME_SESSION_DBUS_INTERFACE_PRESENCE   "org.gnome.SessionManager.Presence"

#define UPOWER_DBUS_NAME                        "org.freedesktop.UPower"
#define UPOWER_DBUS_PATH                        "/org/freedesktop/UPower"
#define UPOWER_DBUS_PATH_KBDBACKLIGHT           "/org/freedesktop/UPower/KbdBacklight"
#define UPOWER_DBUS_INTERFACE                   "org.freedesktop.UPower"
#define UPOWER_DBUS_INTERFACE_KBDBACKLIGHT      "org.freedesktop.UPower.KbdBacklight"

#define GSD_POWER_SETTINGS_SCHEMA               "org.gnome.settings-daemon.plugins.power"

#define GSD_DBUS_SERVICE                        "org.gnome.SettingsDaemon"
#define GSD_DBUS_PATH                           "/org/gnome/SettingsDaemon"
#define GSD_POWER_DBUS_PATH                     GSD_DBUS_PATH "/Power"
#define GSD_POWER_DBUS_INTERFACE                "org.gnome.SettingsDaemon.Power"
#define GSD_POWER_DBUS_INTERFACE_SCREEN         "org.gnome.SettingsDaemon.Power.Screen"
#define GSD_POWER_DBUS_INTERFACE_KEYBOARD       "org.gnome.SettingsDaemon.Power.Keyboard"

#define GS_DBUS_NAME                            "org.gnome.ScreenSaver"
#define GS_DBUS_PATH                            "/org/gnome/ScreenSaver"
#define GS_DBUS_INTERFACE                       "org.gnome.ScreenSaver"

#define GSD_POWER_MANAGER_NOTIFY_TIMEOUT_NEVER          0 /* ms */
#define GSD_POWER_MANAGER_NOTIFY_TIMEOUT_SHORT          10 * 1000 /* ms */
#define GSD_POWER_MANAGER_NOTIFY_TIMEOUT_LONG           30 * 1000 /* ms */

#define GSD_POWER_MANAGER_CRITICAL_ALERT_TIMEOUT        5 /* seconds */
#define GSD_POWER_MANAGER_RECALL_DELAY                  30 /* seconds */
#define GSD_POWER_MANAGER_LID_CLOSE_SAFETY_TIMEOUT      30 /* seconds */

/* Keep this in sync with gnome-shell */
#define SCREENSAVER_FADE_TIME                           10 /* seconds */

#define XSCREENSAVER_WATCHDOG_TIMEOUT                   120 /* seconds */

enum {
        GSD_POWER_IDLETIME_NULL_ID,
        GSD_POWER_IDLETIME_DIM_ID,
        GSD_POWER_IDLETIME_BLANK_ID,
        GSD_POWER_IDLETIME_SLEEP_ID
};

static const gchar introspection_xml[] =
"<node>"
  "<interface name='org.gnome.SettingsDaemon.Power'>"
    "<property name='Icon' type='s' access='read'>"
    "</property>"
    "<property name='Tooltip' type='s' access='read'>"
    "</property>"
    "<method name='GetPrimaryDevice'>"
      "<arg name='device' type='(susdut)' direction='out' />"
    "</method>"
    "<method name='GetDevices'>"
      "<arg name='devices' type='a(susdut)' direction='out' />"
    "</method>"
  "</interface>"
"  <interface name='org.gnome.SettingsDaemon.Power.Screen'>"
"    <method name='StepUp'>"
"      <arg type='u' name='new_percentage' direction='out'/>"
"    </method>"
"    <method name='StepDown'>"
"      <arg type='u' name='new_percentage' direction='out'/>"
"    </method>"
"    <method name='GetPercentage'>"
"      <arg type='u' name='percentage' direction='out'/>"
"    </method>"
"    <method name='SetPercentage'>"
"      <arg type='u' name='percentage' direction='in'/>"
"      <arg type='u' name='new_percentage' direction='out'/>"
"    </method>"
"    <signal name='Changed'>"
"    </signal>"
"  </interface>"
"  <interface name='org.gnome.SettingsDaemon.Power.Keyboard'>"
"    <method name='StepUp'>"
"      <arg type='u' name='new_percentage' direction='out'/>"
"    </method>"
"    <method name='StepDown'>"
"      <arg type='u' name='new_percentage' direction='out'/>"
"    </method>"
"    <method name='Toggle'>"
"      <arg type='u' name='new_percentage' direction='out'/>"
"    </method>"
"  </interface>"
"</node>";

/* on ACPI machines we have 4-16 levels, on others it's ~150 */
#define BRIGHTNESS_STEP_AMOUNT(max) ((max) < 20 ? 1 : (max) / 20)

/* take a discrete value with offset and convert to percentage */
static int
abs_to_percentage (int min, int max, int value)
{
        g_return_val_if_fail (max > min, -1);
        g_return_val_if_fail (value >= min, -1);
        g_return_val_if_fail (value <= max, -1);
        return (((value - min) * 100) / (max - min));
}
#define ABS_TO_PERCENTAGE(min, max, value) abs_to_percentage(min, max, value)
#define PERCENTAGE_TO_ABS(min, max, value) (min + (((max - min) * value) / 100))

#define GSD_POWER_MANAGER_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), GSD_TYPE_POWER_MANAGER, GsdPowerManagerPrivate))

typedef enum {
        GSD_POWER_IDLE_MODE_NORMAL,
        GSD_POWER_IDLE_MODE_DIM,
        GSD_POWER_IDLE_MODE_BLANK,
        GSD_POWER_IDLE_MODE_SLEEP
} GsdPowerIdleMode;

struct GsdPowerManagerPrivate
{
        GnomeSettingsSession    *session;
        gboolean                 lid_is_closed;
        GSettings               *settings;
        GSettings               *settings_screensaver;
        UpClient                *up_client;
        GDBusNodeInfo           *introspection_data;
        GDBusConnection         *connection;
        GCancellable            *bus_cancellable;
        GDBusProxy              *upower_proxy;
        GDBusProxy              *upower_kdb_proxy;
        gint                     kbd_brightness_now;
        gint                     kbd_brightness_max;
        gint                     kbd_brightness_old;
        gint                     kbd_brightness_pre_dim;
        GnomeRRScreen           *x11_screen;
        gboolean                 use_time_primary;
        gchar                   *previous_summary;
        GIcon                   *previous_icon;
        GpmPhone                *phone;
        GPtrArray               *devices_array;
        guint                    action_percentage;
        guint                    action_time;
        guint                    critical_percentage;
        guint                    critical_time;
        guint                    low_percentage;
        guint                    low_time;
        gint                     pre_dim_brightness; /* level, not percentage */
        UpDevice                *device_composite;
        NotifyNotification      *notification_discharging;
        NotifyNotification      *notification_low;
        ca_context              *canberra_context;
        ca_proplist             *critical_alert_loop_props;
        guint32                  critical_alert_timeout_id;
        GDBusProxy              *screensaver_proxy;
        GDBusProxy              *session_proxy;
        GDBusProxy              *session_presence_proxy;
        GpmIdletime             *idletime;
        GsdPowerIdleMode         current_idle_mode;
        guint                    lid_close_safety_timer_id;
        GtkStatusIcon           *status_icon;
        guint                    xscreensaver_watchdog_timer_id;
        gboolean                 is_virtual_machine;
};

enum {
        PROP_0,
};

static void     gsd_power_manager_class_init  (GsdPowerManagerClass *klass);
static void     gsd_power_manager_init        (GsdPowerManager      *power_manager);
static void     gsd_power_manager_finalize    (GObject              *object);

static UpDevice *engine_get_composite_device (GsdPowerManager *manager, UpDevice *original_device);
static UpDevice *engine_update_composite_device (GsdPowerManager *manager, UpDevice *original_device);
static GIcon    *engine_get_icon (GsdPowerManager *manager);
static gchar    *engine_get_summary (GsdPowerManager *manager);
static void      do_power_action_type (GsdPowerManager *manager, GsdPowerActionType action_type);
static void      do_lid_closed_action (GsdPowerManager *manager);
static void      lock_screensaver (GsdPowerManager *manager);
static void      kill_lid_close_safety_timer (GsdPowerManager *manager);

G_DEFINE_TYPE (GsdPowerManager, gsd_power_manager, G_TYPE_OBJECT)

static gpointer manager_object = NULL;

GQuark
gsd_power_manager_error_quark (void)
{
        static GQuark quark = 0;
        if (!quark)
                quark = g_quark_from_static_string ("gsd_power_manager_error");
        return quark;
}

static gboolean
play_loop_timeout_cb (GsdPowerManager *manager)
{
        ca_context *context;
        context = ca_gtk_context_get_for_screen (gdk_screen_get_default ());
        ca_context_play_full (context, 0,
                              manager->priv->critical_alert_loop_props,
                              NULL,
                              NULL);
        return TRUE;
}

static gboolean
play_loop_stop (GsdPowerManager *manager)
{
        if (manager->priv->critical_alert_timeout_id == 0) {
                g_warning ("no sound loop present to stop");
                return FALSE;
        }

        g_source_remove (manager->priv->critical_alert_timeout_id);
        ca_proplist_destroy (manager->priv->critical_alert_loop_props);

        manager->priv->critical_alert_loop_props = NULL;
        manager->priv->critical_alert_timeout_id = 0;

        return TRUE;
}

static gboolean
play_loop_start (GsdPowerManager *manager,
                 const gchar *id,
                 const gchar *desc,
                 gboolean force,
                 guint timeout)
{
        ca_context *context;

        if (timeout == 0) {
                g_warning ("received invalid timeout");
                return FALSE;
        }

        /* if a sound loop is already running, stop the existing loop */
        if (manager->priv->critical_alert_timeout_id != 0) {
                g_warning ("was instructed to play a sound loop with one already playing");
                play_loop_stop (manager);
        }

        ca_proplist_create (&(manager->priv->critical_alert_loop_props));
        ca_proplist_sets (manager->priv->critical_alert_loop_props,
                          CA_PROP_EVENT_ID, id);
        ca_proplist_sets (manager->priv->critical_alert_loop_props,
                          CA_PROP_EVENT_DESCRIPTION, desc);

        manager->priv->critical_alert_timeout_id = g_timeout_add_seconds (timeout,
                                                                          (GSourceFunc) play_loop_timeout_cb,
                                                                          manager);
        g_source_set_name_by_id (manager->priv->critical_alert_timeout_id,
                                 "[GsdPowerManager] play-loop");

        /* play the sound, using sounds from the naming spec */
        context = ca_gtk_context_get_for_screen (gdk_screen_get_default ());
        ca_context_play (context, 0,
                         CA_PROP_EVENT_ID, id,
                         CA_PROP_EVENT_DESCRIPTION, desc, NULL);
        return TRUE;
}

static void
notify_close_if_showing (NotifyNotification *notification)
{
        gboolean ret;
        GError *error = NULL;

        if (notification == NULL)
                return;
        ret = notify_notification_close (notification, &error);
        if (!ret) {
                g_warning ("failed to close notification: %s",
                           error->message);
                g_error_free (error);
        }
}

static const gchar *
get_first_themed_icon_name (GIcon *icon)
{
        const gchar* const *icon_names;
        const gchar *icon_name = NULL;

        /* no icon */
        if (icon == NULL)
                goto out;

        /* just use the first icon */
        icon_names = g_themed_icon_get_names (G_THEMED_ICON (icon));
        if (icon_names != NULL)
                icon_name = icon_names[0];
out:
        return icon_name;
}

typedef enum {
        WARNING_NONE            = 0,
        WARNING_DISCHARGING     = 1,
        WARNING_LOW             = 2,
        WARNING_CRITICAL        = 3,
        WARNING_ACTION          = 4
} GsdPowerManagerWarning;

static GVariant *
engine_get_icon_property_variant (GsdPowerManager  *manager)
{
        GIcon *icon;
        GVariant *retval;

        icon = engine_get_icon (manager);
        if (icon != NULL) {
                char *str;
                str = g_icon_to_string (icon);
                g_object_unref (icon);
                retval = g_variant_new_string (str);
                g_free (str);
        } else {
                retval = g_variant_new_string ("");
        }
        return retval;
}

static GVariant *
engine_get_tooltip_property_variant (GsdPowerManager  *manager)
{
        char *tooltip;
        GVariant *retval;

        tooltip = engine_get_summary (manager);
        retval = g_variant_new_string (tooltip != NULL ? tooltip : "");
        g_free (tooltip);

        return retval;
}

static void
engine_emit_changed (GsdPowerManager *manager,
                     gboolean         icon_changed,
                     gboolean         state_changed)
{
        GVariantBuilder props_builder;
        GVariant *props_changed = NULL;
        GError *error = NULL;

        /* not yet connected to the bus */
        if (manager->priv->connection == NULL)
                return;

        g_variant_builder_init (&props_builder, G_VARIANT_TYPE ("a{sv}"));

        if (icon_changed)
                g_variant_builder_add (&props_builder, "{sv}", "Icon",
                                       engine_get_icon_property_variant (manager));
        if (state_changed)
                g_variant_builder_add (&props_builder, "{sv}", "Tooltip",
                                       engine_get_tooltip_property_variant (manager));

        props_changed = g_variant_new ("(s@a{sv}@as)", GSD_POWER_DBUS_INTERFACE,
                                       g_variant_builder_end (&props_builder),
                                       g_variant_new_strv (NULL, 0));
        g_variant_ref_sink (props_changed);

        if (!g_dbus_connection_emit_signal (manager->priv->connection,
                                            NULL,
                                            GSD_POWER_DBUS_PATH,
                                            "org.freedesktop.DBus.Properties",
                                            "PropertiesChanged",
                                            props_changed,
                                            &error))
                goto out;

 out:
        if (error) {
                g_warning ("%s", error->message);
                g_clear_error (&error);
        }
        if (props_changed)
                g_variant_unref (props_changed);
}

static GsdPowerManagerWarning
engine_get_warning_csr (GsdPowerManager *manager, UpDevice *device)
{
        gdouble percentage;

        /* get device properties */
        g_object_get (device, "percentage", &percentage, NULL);

        if (percentage < 26.0f)
                return WARNING_LOW;
        else if (percentage < 13.0f)
                return WARNING_CRITICAL;
        return WARNING_NONE;
}

static GsdPowerManagerWarning
engine_get_warning_percentage (GsdPowerManager *manager, UpDevice *device)
{
        gdouble percentage;

        /* get device properties */
        g_object_get (device, "percentage", &percentage, NULL);

        if (percentage <= manager->priv->action_percentage)
                return WARNING_ACTION;
        if (percentage <= manager->priv->critical_percentage)
                return WARNING_CRITICAL;
        if (percentage <= manager->priv->low_percentage)
                return WARNING_LOW;
        return WARNING_NONE;
}

static GsdPowerManagerWarning
engine_get_warning_time (GsdPowerManager *manager, UpDevice *device)
{
        UpDeviceKind kind;
        gint64 time_to_empty;

        /* get device properties */
        g_object_get (device,
                      "kind", &kind,
                      "time-to-empty", &time_to_empty,
                      NULL);

        /* this is probably an error condition */
        if (time_to_empty == 0) {
                g_debug ("time zero, falling back to percentage for %s",
                         up_device_kind_to_string (kind));
                return engine_get_warning_percentage (manager, device);
        }

        if (time_to_empty <= manager->priv->action_time)
                return WARNING_ACTION;
        if (time_to_empty <= manager->priv->critical_time)
                return WARNING_CRITICAL;
        if (time_to_empty <= manager->priv->low_time)
                return WARNING_LOW;
        return WARNING_NONE;
}

/**
 * This gets the possible engine state for the device according to the
 * policy, which could be per-percent, or per-time.
 **/
static GsdPowerManagerWarning
engine_get_warning (GsdPowerManager *manager, UpDevice *device)
{
        UpDeviceKind kind;
        UpDeviceState state;
        GsdPowerManagerWarning warning_type;

        /* get device properties */
        g_object_get (device,
                      "kind", &kind,
                      "state", &state,
                      NULL);

        /* default to no engine */
        warning_type = WARNING_NONE;

        /* if the device in question is on ac, don't give a warning */
        if (state == UP_DEVICE_STATE_CHARGING)
                goto out;

        if (kind == UP_DEVICE_KIND_MOUSE ||
            kind == UP_DEVICE_KIND_KEYBOARD) {

                warning_type = engine_get_warning_csr (manager, device);

        } else if (kind == UP_DEVICE_KIND_UPS ||
#if UP_CHECK_VERSION(0,9,5)
                   kind == UP_DEVICE_KIND_MEDIA_PLAYER ||
                   kind == UP_DEVICE_KIND_TABLET ||
                   kind == UP_DEVICE_KIND_COMPUTER ||
#endif
                   kind == UP_DEVICE_KIND_PDA) {

                warning_type = engine_get_warning_percentage (manager, device);

        } else if (kind == UP_DEVICE_KIND_PHONE) {

                warning_type = engine_get_warning_percentage (manager, device);

        } else if (kind == UP_DEVICE_KIND_BATTERY) {
                /* only use the time when it is accurate, and settings is not disabled */
                if (manager->priv->use_time_primary)
                        warning_type = engine_get_warning_time (manager, device);
                else
                        warning_type = engine_get_warning_percentage (manager, device);
        }

        /* If we have no important engines, we should test for discharging */
        if (warning_type == WARNING_NONE) {
                if (state == UP_DEVICE_STATE_DISCHARGING)
                        warning_type = WARNING_DISCHARGING;
        }

 out:
        return warning_type;
}

static gchar *
engine_get_summary (GsdPowerManager *manager)
{
        guint i;
        GPtrArray *array;
        UpDevice *device;
        UpDeviceState state;
        GString *tooltip = NULL;
        gchar *part;
        gboolean is_present;


        /* need to get AC state */
        tooltip = g_string_new ("");

        /* do we have specific device types? */
        array = manager->priv->devices_array;
        for (i=0;i<array->len;i++) {
                device = g_ptr_array_index (array, i);
                g_object_get (device,
                              "is-present", &is_present,
                              "state", &state,
                              NULL);
                if (!is_present)
                        continue;
                if (state == UP_DEVICE_STATE_EMPTY)
                        continue;
                part = gpm_upower_get_device_summary (device);
                if (part != NULL)
                        g_string_append_printf (tooltip, "%s\n", part);
                g_free (part);
        }

        /* remove the last \n */
        g_string_truncate (tooltip, tooltip->len-1);

        g_debug ("tooltip: %s", tooltip->str);

        return g_string_free (tooltip, FALSE);
}

static GIcon *
engine_get_icon_priv (GsdPowerManager *manager,
                      UpDeviceKind device_kind,
                      GsdPowerManagerWarning warning,
                      gboolean use_state)
{
        guint i;
        GPtrArray *array;
        UpDevice *device;
        GsdPowerManagerWarning warning_temp;
        UpDeviceKind kind;
        UpDeviceState state;
        gboolean is_present;

        /* do we have specific device types? */
        array = manager->priv->devices_array;
        for (i=0;i<array->len;i++) {
                device = g_ptr_array_index (array, i);

                /* get device properties */
                g_object_get (device,
                              "kind", &kind,
                              "state", &state,
                              "is-present", &is_present,
                              NULL);

                /* if battery then use composite device to cope with multiple batteries */
                if (kind == UP_DEVICE_KIND_BATTERY)
                        device = engine_get_composite_device (manager, device);

                warning_temp = GPOINTER_TO_INT(g_object_get_data (G_OBJECT(device),
                                                                  "engine-warning-old"));
                if (kind == device_kind && is_present) {
                        if (warning != WARNING_NONE) {
                                if (warning_temp == warning)
                                        return gpm_upower_get_device_icon (device, TRUE);
                                continue;
                        }
                        if (use_state) {
                                if (state == UP_DEVICE_STATE_CHARGING ||
                                    state == UP_DEVICE_STATE_DISCHARGING)
                                        return gpm_upower_get_device_icon (device, TRUE);
                                continue;
                        }
                        return gpm_upower_get_device_icon (device, TRUE);
                }
        }
        return NULL;
}

static GIcon *
engine_get_icon (GsdPowerManager *manager)
{
        GIcon *icon = NULL;


        /* we try CRITICAL: BATTERY, UPS, MOUSE, KEYBOARD */
        icon = engine_get_icon_priv (manager, UP_DEVICE_KIND_BATTERY, WARNING_CRITICAL, FALSE);
        if (icon != NULL)
                return icon;
        icon = engine_get_icon_priv (manager, UP_DEVICE_KIND_UPS, WARNING_CRITICAL, FALSE);
        if (icon != NULL)
                return icon;
        icon = engine_get_icon_priv (manager, UP_DEVICE_KIND_MOUSE, WARNING_CRITICAL, FALSE);
        if (icon != NULL)
                return icon;
        icon = engine_get_icon_priv (manager, UP_DEVICE_KIND_KEYBOARD, WARNING_CRITICAL, FALSE);
        if (icon != NULL)
                return icon;

        /* we try CRITICAL: BATTERY, UPS, MOUSE, KEYBOARD */
        icon = engine_get_icon_priv (manager, UP_DEVICE_KIND_BATTERY, WARNING_LOW, FALSE);
        if (icon != NULL)
                return icon;
        icon = engine_get_icon_priv (manager, UP_DEVICE_KIND_UPS, WARNING_LOW, FALSE);
        if (icon != NULL)
                return icon;
        icon = engine_get_icon_priv (manager, UP_DEVICE_KIND_MOUSE, WARNING_LOW, FALSE);
        if (icon != NULL)
                return icon;
        icon = engine_get_icon_priv (manager, UP_DEVICE_KIND_KEYBOARD, WARNING_LOW, FALSE);
        if (icon != NULL)
                return icon;

        /* we try (DIS)CHARGING: BATTERY, UPS */
        icon = engine_get_icon_priv (manager, UP_DEVICE_KIND_BATTERY, WARNING_NONE, TRUE);
        if (icon != NULL)
                return icon;
        icon = engine_get_icon_priv (manager, UP_DEVICE_KIND_UPS, WARNING_NONE, TRUE);
        if (icon != NULL)
                return icon;

        /* we try PRESENT: BATTERY, UPS */
        icon = engine_get_icon_priv (manager, UP_DEVICE_KIND_BATTERY, WARNING_NONE, FALSE);
        if (icon != NULL)
                return icon;
        icon = engine_get_icon_priv (manager, UP_DEVICE_KIND_UPS, WARNING_NONE, FALSE);
        if (icon != NULL)
                return icon;

        /* do not show an icon */
        return NULL;
}

static gboolean
engine_recalculate_state_icon (GsdPowerManager *manager)
{
        GIcon *icon;

        /* show a different icon if we are disconnected */
        icon = engine_get_icon (manager);
        gtk_status_icon_set_visible (manager->priv->status_icon, icon != NULL);

        if (icon == NULL) {
                /* none before, now none */
                if (manager->priv->previous_icon == NULL)
                        return FALSE;

                g_object_unref (manager->priv->previous_icon);
                manager->priv->previous_icon = NULL;
                return TRUE;
        }

        /* no icon before, now icon */
        if (manager->priv->previous_icon == NULL) {

                /* set fallback icon */
                gtk_status_icon_set_from_gicon (manager->priv->status_icon, icon);
                manager->priv->previous_icon = icon;
                return TRUE;
        }

        /* icon before, now different */
        if (!g_icon_equal (manager->priv->previous_icon, icon)) {

                /* set fallback icon */
                gtk_status_icon_set_from_gicon (manager->priv->status_icon, icon);

                g_object_unref (manager->priv->previous_icon);
                manager->priv->previous_icon = icon;
                return TRUE;
        }

        g_debug ("no change");
        /* nothing to do */
        g_object_unref (icon);
        return FALSE;
}

static gboolean
engine_recalculate_state_summary (GsdPowerManager *manager)
{
        gchar *summary;

        summary = engine_get_summary (manager);
        if (manager->priv->previous_summary == NULL) {
                manager->priv->previous_summary = summary;

                /* set fallback tooltip */
                gtk_status_icon_set_tooltip_text (manager->priv->status_icon,
                                                  summary);

                return TRUE;
        }

        if (strcmp (manager->priv->previous_summary, summary) != 0) {
                g_free (manager->priv->previous_summary);
                manager->priv->previous_summary = summary;

                /* set fallback tooltip */
                gtk_status_icon_set_tooltip_text (manager->priv->status_icon,
                                                  summary);

                return TRUE;
        }
        g_debug ("no change");
        /* nothing to do */
        g_free (summary);
        return FALSE;
}

static void
engine_recalculate_state (GsdPowerManager *manager)
{
        gboolean icon_changed = FALSE;
        gboolean state_changed = FALSE;

        icon_changed = engine_recalculate_state_icon (manager);
        state_changed = engine_recalculate_state_summary (manager);

        /* only emit if the icon or summary has changed */
        if (icon_changed || state_changed)
                engine_emit_changed (manager, icon_changed, state_changed);
}

static UpDevice *
engine_get_composite_device (GsdPowerManager *manager,
                             UpDevice *original_device)
{
        guint battery_devices = 0;
        GPtrArray *array;
        UpDevice *device;
        UpDeviceKind kind;
        UpDeviceKind original_kind;
        guint i;

        /* get the type of the original device */
        g_object_get (original_device,
                      "kind", &original_kind,
                      NULL);

        /* find out how many batteries in the system */
        array = manager->priv->devices_array;
        for (i=0;i<array->len;i++) {
                device = g_ptr_array_index (array, i);
                g_object_get (device,
                              "kind", &kind,
                              NULL);
                if (kind == original_kind)
                        battery_devices++;
        }

        /* just use the original device if only one primary battery */
        if (battery_devices <= 1) {
                g_debug ("using original device as only one primary battery");
                device = original_device;
                goto out;
        }

        /* use the composite device */
        device = manager->priv->device_composite;
out:
        /* return composite device or original device */
        return device;
}

static UpDevice *
engine_update_composite_device (GsdPowerManager *manager,
                                UpDevice *original_device)
{
        guint i;
        gdouble percentage = 0.0;
        gdouble energy = 0.0;
        gdouble energy_full = 0.0;
        gdouble energy_rate = 0.0;
        gdouble energy_total = 0.0;
        gdouble energy_full_total = 0.0;
        gdouble energy_rate_total = 0.0;
        gint64 time_to_empty = 0;
        gint64 time_to_full = 0;
        guint battery_devices = 0;
        gboolean is_charging = FALSE;
        gboolean is_discharging = FALSE;
        gboolean is_fully_charged = TRUE;
        GPtrArray *array;
        UpDevice *device;
        UpDeviceState state;
        UpDeviceKind kind;
        UpDeviceKind original_kind;

        /* get the type of the original device */
        g_object_get (original_device,
                      "kind", &original_kind,
                      NULL);

        /* update the composite device */
        array = manager->priv->devices_array;
        for (i=0;i<array->len;i++) {
                device = g_ptr_array_index (array, i);
                g_object_get (device,
                              "kind", &kind,
                              "state", &state,
                              "energy", &energy,
                              "energy-full", &energy_full,
                              "energy-rate", &energy_rate,
                              NULL);
                if (kind != original_kind)
                        continue;

                /* one of these will be charging or discharging */
                if (state == UP_DEVICE_STATE_CHARGING)
                        is_charging = TRUE;
                if (state == UP_DEVICE_STATE_DISCHARGING)
                        is_discharging = TRUE;
                if (state != UP_DEVICE_STATE_FULLY_CHARGED)
                        is_fully_charged = FALSE;

                /* sum up composite */
                energy_total += energy;
                energy_full_total += energy_full;
                energy_rate_total += energy_rate;
                battery_devices++;
        }

        /* just use the original device if only one primary battery */
        if (battery_devices == 1) {
                g_debug ("using original device as only one primary battery");
                device = original_device;
                goto out;
        }

        /* use percentage weighted for each battery capacity */
        if (energy_full_total > 0.0)
                percentage = 100.0 * energy_total / energy_full_total;

        /* set composite state */
        if (is_charging)
                state = UP_DEVICE_STATE_CHARGING;
        else if (is_discharging)
                state = UP_DEVICE_STATE_DISCHARGING;
        else if (is_fully_charged)
                state = UP_DEVICE_STATE_FULLY_CHARGED;
        else
                state = UP_DEVICE_STATE_UNKNOWN;

        /* calculate a quick and dirty time remaining value */
        if (energy_rate_total > 0) {
                if (state == UP_DEVICE_STATE_DISCHARGING)
                        time_to_empty = 3600 * (energy_total / energy_rate_total);
                else if (state == UP_DEVICE_STATE_CHARGING)
                        time_to_full = 3600 * ((energy_full_total - energy_total) / energy_rate_total);
        }

        /* okay, we can use the composite device */
        device = manager->priv->device_composite;

        g_debug ("printing composite device");
        g_object_set (device,
                      "energy", energy,
                      "energy-full", energy_full,
                      "energy-rate", energy_rate,
                      "time-to-empty", time_to_empty,
                      "time-to-full", time_to_full,
                      "percentage", percentage,
                      "state", state,
                      NULL);

        /* force update of icon */
        if (engine_recalculate_state_icon (manager))
                engine_emit_changed (manager, TRUE, FALSE);
out:
        /* return composite device or original device */
        return device;
}

typedef struct {
        GsdPowerManager *manager;
        UpDevice        *device;
} GsdPowerManagerRecallData;

static void
device_perhaps_recall_response_cb (GtkDialog *dialog,
                                   gint response_id,
                                   GsdPowerManagerRecallData *recall_data)
{
        GdkScreen *screen;
        GtkWidget *dialog_error;
        GError *error = NULL;
        gboolean ret;
        gchar *website = NULL;

        /* don't show this again */
        if (response_id == GTK_RESPONSE_CANCEL) {
                g_settings_set_boolean (recall_data->manager->priv->settings,
                                        "notify-perhaps-recall",
                                        FALSE);
                goto out;
        }

        /* visit recall website */
        if (response_id == GTK_RESPONSE_OK) {

                g_object_get (recall_data->device,
                              "recall-url", &website,
                              NULL);

                screen = gdk_screen_get_default();
                ret = gtk_show_uri (screen,
                                    website,
                                    gtk_get_current_event_time (),
                                    &error);
                if (!ret) {
                        dialog_error = gtk_message_dialog_new (NULL,
                                                               GTK_DIALOG_MODAL,
                                                               GTK_MESSAGE_INFO,
                                                               GTK_BUTTONS_OK,
                                                               "Failed to show url %s",
                                                               error->message);
                        gtk_dialog_run (GTK_DIALOG (dialog_error));
                        g_error_free (error);
                }
        }
out:
        gtk_widget_destroy (GTK_WIDGET (dialog));
        g_object_unref (recall_data->device);
        g_object_unref (recall_data->manager);
        g_free (recall_data);
        g_free (website);
        return;
}

static gboolean
device_perhaps_recall_delay_cb (gpointer user_data)
{
        gchar *vendor;
        const gchar *title = NULL;
        GString *message = NULL;
        GtkWidget *dialog;
        GsdPowerManagerRecallData *recall_data = (GsdPowerManagerRecallData *) user_data;

        g_object_get (recall_data->device,
                      "recall-vendor", &vendor,
                      NULL);

        /* TRANSLATORS: the battery may be recalled by its vendor */
        title = _("Battery may be recalled");
        message = g_string_new ("");
        g_string_append_printf (message,
                                _("A battery in your computer may have been "
                                  "recalled by %s and you may be at risk."), vendor);
        g_string_append (message, "\n\n");
        g_string_append (message, _("For more information visit the battery recall website."));
        dialog = gtk_message_dialog_new_with_markup (NULL,
                                                     GTK_DIALOG_DESTROY_WITH_PARENT,
                                                     GTK_MESSAGE_INFO,
                                                     GTK_BUTTONS_CLOSE,
                                                     "<span size='larger'><b>%s</b></span>",
                                                     title);
        gtk_message_dialog_format_secondary_markup (GTK_MESSAGE_DIALOG (dialog),
                                                    "%s", message->str);

        /* TRANSLATORS: button text, visit the manufacturers recall website */
        gtk_dialog_add_button (GTK_DIALOG (dialog), _("Visit recall website"),
                               GTK_RESPONSE_OK);

        /* TRANSLATORS: button text, do not show this bubble again */
        gtk_dialog_add_button (GTK_DIALOG (dialog), _("Do not show me this again"),
                               GTK_RESPONSE_CANCEL);

        gtk_widget_show (dialog);
        g_signal_connect (dialog, "response",
                          G_CALLBACK (device_perhaps_recall_response_cb),
                          recall_data);

        g_string_free (message, TRUE);
        g_free (vendor);
        return FALSE;
}

static void
device_perhaps_recall (GsdPowerManager *manager, UpDevice *device)
{
        gboolean ret;
        guint timer_id;
        GsdPowerManagerRecallData *recall_data;

        /* don't show when running under GDM */
        if (g_getenv ("RUNNING_UNDER_GDM") != NULL) {
                g_debug ("running under gdm, so no notification");
                return;
        }

        /* already shown, and dismissed */
        ret = g_settings_get_boolean (manager->priv->settings,
                                      "notify-perhaps-recall");
        if (!ret) {
                g_debug ("settings prevents recall notification");
                return;
        }

        recall_data = g_new0 (GsdPowerManagerRecallData, 1);
        recall_data->manager = g_object_ref (manager);
        recall_data->device = g_object_ref (device);

        /* delay by a few seconds so the session can load */
        timer_id = g_timeout_add_seconds (GSD_POWER_MANAGER_RECALL_DELAY,
                                          device_perhaps_recall_delay_cb,
                                          recall_data);
        g_source_set_name_by_id (timer_id, "[GsdPowerManager] perhaps-recall");
}

static void
engine_device_add (GsdPowerManager *manager, UpDevice *device)
{
        gboolean recall_notice;
        GsdPowerManagerWarning warning;
        UpDeviceState state;
        UpDeviceKind kind;
        UpDevice *composite;

        /* assign warning */
        warning = engine_get_warning (manager, device);
        g_object_set_data (G_OBJECT(device),
                           "engine-warning-old",
                           GUINT_TO_POINTER(warning));

        /* get device properties */
        g_object_get (device,
                      "kind", &kind,
                      "state", &state,
                      "recall-notice", &recall_notice,
                      NULL);

        /* add old state for transitions */
        g_debug ("adding %s with state %s",
                 up_device_get_object_path (device), up_device_state_to_string (state));
        g_object_set_data (G_OBJECT(device),
                           "engine-state-old",
                           GUINT_TO_POINTER(state));

        if (kind == UP_DEVICE_KIND_BATTERY) {
                g_debug ("updating because we added a device");
                composite = engine_update_composite_device (manager, device);

                /* get the same values for the composite device */
                warning = engine_get_warning (manager, composite);
                g_object_set_data (G_OBJECT(composite),
                                   "engine-warning-old",
                                   GUINT_TO_POINTER(warning));
                g_object_get (composite, "state", &state, NULL);
                g_object_set_data (G_OBJECT(composite),
                                   "engine-state-old",
                                   GUINT_TO_POINTER(state));
        }

        /* the device is recalled */
        if (recall_notice)
                device_perhaps_recall (manager, device);
}

static gboolean
engine_check_recall (GsdPowerManager *manager, UpDevice *device)
{
        UpDeviceKind kind;
        gboolean recall_notice = FALSE;
        gchar *recall_vendor = NULL;
        gchar *recall_url = NULL;

        /* get device properties */
        g_object_get (device,
                      "kind", &kind,
                      "recall-notice", &recall_notice,
                      "recall-vendor", &recall_vendor,
                      "recall-url", &recall_url,
                      NULL);

        /* not battery */
        if (kind != UP_DEVICE_KIND_BATTERY)
                goto out;

        /* no recall data */
        if (!recall_notice)
                goto out;

        /* emit signal for manager */
        g_debug ("** EMIT: perhaps-recall");
        g_debug ("%s-%s", recall_vendor, recall_url);
out:
        g_free (recall_vendor);
        g_free (recall_url);
        return recall_notice;
}

static gboolean
engine_coldplug (GsdPowerManager *manager)
{
        guint i;
        GPtrArray *array = NULL;
        UpDevice *device;
        gboolean ret;
        GError *error = NULL;

        /* get devices from UPower */
        ret = up_client_enumerate_devices_sync (manager->priv->up_client, NULL, &error);
        if (!ret) {
                g_warning ("failed to get device list: %s", error->message);
                g_error_free (error);
                goto out;
        }

        /* connected mobile phones */
        gpm_phone_coldplug (manager->priv->phone);

        engine_recalculate_state (manager);

        /* add to database */
        array = up_client_get_devices (manager->priv->up_client);
        for (i=0;i<array->len;i++) {
                device = g_ptr_array_index (array, i);
                engine_device_add (manager, device);
                engine_check_recall (manager, device);
        }
out:
        if (array != NULL)
                g_ptr_array_unref (array);
        /* never repeat */
        return FALSE;
}

static void
engine_device_added_cb (UpClient *client, UpDevice *device, GsdPowerManager *manager)
{
        /* add to list */
        g_ptr_array_add (manager->priv->devices_array, g_object_ref (device));
        engine_check_recall (manager, device);

        engine_recalculate_state (manager);
}

static void
engine_device_removed_cb (UpClient *client, UpDevice *device, GsdPowerManager *manager)
{
        gboolean ret;
        ret = g_ptr_array_remove (manager->priv->devices_array, device);
        if (!ret)
                return;
        engine_recalculate_state (manager);
}

static void
on_notification_closed (NotifyNotification *notification, gpointer data)
{
    g_object_unref (notification);
}

static void
create_notification (const char *summary,
                     const char *body,
                     const char *icon,
                     NotifyNotification **weak_pointer_location)
{
        NotifyNotification *notification;

        notification = notify_notification_new (summary, body, icon);
        *weak_pointer_location = notification;
        g_object_add_weak_pointer (G_OBJECT (notification),
                                   (gpointer *) weak_pointer_location);
        g_signal_connect (notification, "closed",
                          G_CALLBACK (on_notification_closed), NULL);
}

static void
engine_ups_discharging (GsdPowerManager *manager, UpDevice *device)
{
        const gchar *title;
        gboolean ret;
        gchar *remaining_text = NULL;
        gdouble percentage;
        GError *error = NULL;
        GIcon *icon = NULL;
        gint64 time_to_empty;
        GString *message;
        UpDeviceKind kind;

        /* get device properties */
        g_object_get (device,
                      "kind", &kind,
                      "percentage", &percentage,
                      "time-to-empty", &time_to_empty,
                      NULL);

        if (kind != UP_DEVICE_KIND_UPS)
                return;

        /* only show text if there is a valid time */
        if (time_to_empty > 0)
                remaining_text = gpm_get_timestring (time_to_empty);

        /* TRANSLATORS: UPS is now discharging */
        title = _("UPS Discharging");

        message = g_string_new ("");
        if (remaining_text != NULL) {
                /* TRANSLATORS: tell the user how much time they have got */
                g_string_append_printf (message, _("%s of UPS backup power remaining"),
                                        remaining_text);
        } else {
                g_string_append (message, gpm_device_to_localised_string (device));
        }
        g_string_append_printf (message, " (%.0f%%)", percentage);

        icon = gpm_upower_get_device_icon (device, TRUE);

        /* close any existing notification of this class */
        notify_close_if_showing (manager->priv->notification_discharging);

        /* create a new notification */
        create_notification (title, message->str,
                             get_first_themed_icon_name (icon),
                             &manager->priv->notification_discharging);
        notify_notification_set_timeout (manager->priv->notification_discharging,
                                         GSD_POWER_MANAGER_NOTIFY_TIMEOUT_LONG);
        notify_notification_set_urgency (manager->priv->notification_discharging,
                                         NOTIFY_URGENCY_NORMAL);
        /* TRANSLATORS: this is the notification application name */
        notify_notification_set_app_name (manager->priv->notification_discharging, _("Power"));
        notify_notification_set_hint (manager->priv->notification_discharging,
                                      "transient", g_variant_new_boolean (TRUE));

        /* try to show */
        ret = notify_notification_show (manager->priv->notification_discharging,
                                        &error);
        if (!ret) {
                g_warning ("failed to show notification: %s", error->message);
                g_error_free (error);
                g_object_unref (manager->priv->notification_discharging);
        }
        g_string_free (message, TRUE);
        if (icon != NULL)
                g_object_unref (icon);
        g_free (remaining_text);
}

static GsdPowerActionType
manager_critical_action_get (GsdPowerManager *manager,
                             gboolean         is_ups)
{
        GsdPowerActionType policy;

        policy = g_settings_get_enum (manager->priv->settings, "critical-battery-action");
        if (policy == GSD_POWER_ACTION_SUSPEND) {
                if (is_ups == FALSE &&
                    up_client_get_can_suspend (manager->priv->up_client))
                        return policy;
                return GSD_POWER_ACTION_SHUTDOWN;
        } else if (policy == GSD_POWER_ACTION_HIBERNATE) {
                if (up_client_get_can_hibernate (manager->priv->up_client))
                        return policy;
                return GSD_POWER_ACTION_SHUTDOWN;
        }

        return policy;
}

static gboolean
manager_critical_action_do (GsdPowerManager *manager,
                            gboolean         is_ups)
{
        GsdPowerActionType action_type;

        /* stop playing the alert as it's too late to do anything now */
        if (manager->priv->critical_alert_timeout_id > 0)
                play_loop_stop (manager);

        action_type = manager_critical_action_get (manager, is_ups);
        do_power_action_type (manager, action_type);

        return FALSE;
}

static gboolean
manager_critical_action_do_cb (GsdPowerManager *manager)
{
        manager_critical_action_do (manager, FALSE);
        return FALSE;
}

static gboolean
manager_critical_ups_action_do_cb (GsdPowerManager *manager)
{
        manager_critical_action_do (manager, TRUE);
        return FALSE;
}

static gboolean
engine_just_laptop_battery (GsdPowerManager *manager)
{
        UpDevice *device;
        UpDeviceKind kind;
        GPtrArray *array;
        gboolean ret = TRUE;
        guint i;

        /* find if there are any other device types that mean we have to
         * be more specific in our wording */
        array = manager->priv->devices_array;
        for (i=0; i<array->len; i++) {
                device = g_ptr_array_index (array, i);
                g_object_get (device, "kind", &kind, NULL);
                if (kind != UP_DEVICE_KIND_BATTERY) {
                        ret = FALSE;
                        break;
                }
        }
        return ret;
}

static void
engine_charge_low (GsdPowerManager *manager, UpDevice *device)
{
        const gchar *title = NULL;
        gboolean ret;
        gchar *message = NULL;
        gchar *tmp;
        gchar *remaining_text;
        gdouble percentage;
        GIcon *icon = NULL;
        gint64 time_to_empty;
        UpDeviceKind kind;
        GError *error = NULL;

        /* get device properties */
        g_object_get (device,
                      "kind", &kind,
                      "percentage", &percentage,
                      "time-to-empty", &time_to_empty,
                      NULL);

        /* check to see if the batteries have not noticed we are on AC */
        if (kind == UP_DEVICE_KIND_BATTERY) {
                if (!up_client_get_on_battery (manager->priv->up_client)) {
                        g_warning ("ignoring low message as we are not on battery power");
                        goto out;
                }
        }

        if (kind == UP_DEVICE_KIND_BATTERY) {

                /* if the user has no other batteries, drop the "Laptop" wording */
                ret = engine_just_laptop_battery (manager);
                if (ret) {
                        /* TRANSLATORS: laptop battery low, and we only have one battery */
                        title = _("Battery low");
                } else {
                        /* TRANSLATORS: laptop battery low, and we have more than one kind of battery */
                        title = _("Laptop battery low");
                }
                tmp = gpm_get_timestring (time_to_empty);
                remaining_text = g_strconcat ("<b>", tmp, "</b>", NULL);
                g_free (tmp);

                /* TRANSLATORS: tell the user how much time they have got */
                message = g_strdup_printf (_("Approximately %s remaining (%.0f%%)"), remaining_text, percentage);
                g_free (remaining_text);

        } else if (kind == UP_DEVICE_KIND_UPS) {
                /* TRANSLATORS: UPS is starting to get a little low */
                title = _("UPS low");
                tmp = gpm_get_timestring (time_to_empty);
                remaining_text = g_strconcat ("<b>", tmp, "</b>", NULL);
                g_free (tmp);

                /* TRANSLATORS: tell the user how much time they have got */
                message = g_strdup_printf (_("Approximately %s of remaining UPS backup power (%.0f%%)"),
                                           remaining_text, percentage);
                g_free (remaining_text);
        } else if (kind == UP_DEVICE_KIND_MOUSE) {
                /* TRANSLATORS: mouse is getting a little low */
                title = _("Mouse battery low");

                /* TRANSLATORS: tell user more details */
                message = g_strdup_printf (_("Wireless mouse is low in power (%.0f%%)"), percentage);

        } else if (kind == UP_DEVICE_KIND_KEYBOARD) {
                /* TRANSLATORS: keyboard is getting a little low */
                title = _("Keyboard battery low");

                /* TRANSLATORS: tell user more details */
                message = g_strdup_printf (_("Wireless keyboard is low in power (%.0f%%)"), percentage);

        } else if (kind == UP_DEVICE_KIND_PDA) {
                /* TRANSLATORS: PDA is getting a little low */
                title = _("PDA battery low");

                /* TRANSLATORS: tell user more details */
                message = g_strdup_printf (_("PDA is low in power (%.0f%%)"), percentage);

        } else if (kind == UP_DEVICE_KIND_PHONE) {
                /* TRANSLATORS: cell phone (mobile) is getting a little low */
                title = _("Cell phone battery low");

                /* TRANSLATORS: tell user more details */
                message = g_strdup_printf (_("Cell phone is low in power (%.0f%%)"), percentage);

#if UP_CHECK_VERSION(0,9,5)
        } else if (kind == UP_DEVICE_KIND_MEDIA_PLAYER) {
                /* TRANSLATORS: media player, e.g. mp3 is getting a little low */
                title = _("Media player battery low");

                /* TRANSLATORS: tell user more details */
                message = g_strdup_printf (_("Media player is low in power (%.0f%%)"), percentage);

        } else if (kind == UP_DEVICE_KIND_TABLET) {
                /* TRANSLATORS: graphics tablet, e.g. wacom is getting a little low */
                title = _("Tablet battery low");

                /* TRANSLATORS: tell user more details */
                message = g_strdup_printf (_("Tablet is low in power (%.0f%%)"), percentage);

        } else if (kind == UP_DEVICE_KIND_COMPUTER) {
                /* TRANSLATORS: computer, e.g. ipad is getting a little low */
                title = _("Attached computer battery low");

                /* TRANSLATORS: tell user more details */
                message = g_strdup_printf (_("Attached computer is low in power (%.0f%%)"), percentage);
#endif
        }

        /* get correct icon */
        icon = gpm_upower_get_device_icon (device, TRUE);

        /* close any existing notification of this class */
        notify_close_if_showing (manager->priv->notification_low);

        /* create a new notification */
        create_notification (title, message,
                             get_first_themed_icon_name (icon),
                             &manager->priv->notification_low);
        notify_notification_set_timeout (manager->priv->notification_low,
                                         GSD_POWER_MANAGER_NOTIFY_TIMEOUT_LONG);
        notify_notification_set_urgency (manager->priv->notification_low,
                                         NOTIFY_URGENCY_NORMAL);
        notify_notification_set_app_name (manager->priv->notification_low, _("Power"));
        notify_notification_set_hint (manager->priv->notification_low,
                                      "transient", g_variant_new_boolean (TRUE));

        /* try to show */
        ret = notify_notification_show (manager->priv->notification_low,
                                        &error);
        if (!ret) {
                g_warning ("failed to show notification: %s", error->message);
                g_error_free (error);
                g_object_unref (manager->priv->notification_low);
        }

        /* play the sound, using sounds from the naming spec */
        ca_context_play (manager->priv->canberra_context, 0,
                         CA_PROP_EVENT_ID, "battery-low",
                         /* TRANSLATORS: this is the sound description */
                         CA_PROP_EVENT_DESCRIPTION, _("Battery is low"), NULL);

out:
        if (icon != NULL)
                g_object_unref (icon);
        g_free (message);
}

static void
engine_charge_critical (GsdPowerManager *manager, UpDevice *device)
{
        const gchar *title = NULL;
        gboolean ret;
        gchar *message = NULL;
        gdouble percentage;
        GIcon *icon = NULL;
        gint64 time_to_empty;
        GsdPowerActionType policy;
        UpDeviceKind kind;
        GError *error = NULL;

        /* get device properties */
        g_object_get (device,
                      "kind", &kind,
                      "percentage", &percentage,
                      "time-to-empty", &time_to_empty,
                      NULL);

        /* check to see if the batteries have not noticed we are on AC */
        if (kind == UP_DEVICE_KIND_BATTERY) {
                if (!up_client_get_on_battery (manager->priv->up_client)) {
                        g_warning ("ignoring critically low message as we are not on battery power");
                        goto out;
                }
        }

        if (kind == UP_DEVICE_KIND_BATTERY) {

                /* if the user has no other batteries, drop the "Laptop" wording */
                ret = engine_just_laptop_battery (manager);
                if (ret) {
                        /* TRANSLATORS: laptop battery critically low, and only have one kind of battery */
                        title = _("Battery critically low");
                } else {
                        /* TRANSLATORS: laptop battery critically low, and we have more than one type of battery */
                        title = _("Laptop battery critically low");
                }

                /* we have to do different warnings depending on the policy */
                policy = manager_critical_action_get (manager, FALSE);

                /* use different text for different actions */
                if (policy == GSD_POWER_ACTION_NOTHING) {
                        /* TRANSLATORS: tell the use to insert the plug, as we're not going to do anything */
                        message = g_strdup (_("Plug in your AC adapter to avoid losing data."));

                } else if (policy == GSD_POWER_ACTION_SUSPEND) {
                        /* TRANSLATORS: give the user a ultimatum */
                        message = g_strdup_printf (_("Computer will suspend very soon unless it is plugged in."));

                } else if (policy == GSD_POWER_ACTION_HIBERNATE) {
                        /* TRANSLATORS: give the user a ultimatum */
                        message = g_strdup_printf (_("Computer will hibernate very soon unless it is plugged in."));

                } else if (policy == GSD_POWER_ACTION_SHUTDOWN) {
                        /* TRANSLATORS: give the user a ultimatum */
                        message = g_strdup_printf (_("Computer will shutdown very soon unless it is plugged in."));
                }

        } else if (kind == UP_DEVICE_KIND_UPS) {
                gchar *remaining_text;
                gchar *tmp;

                /* TRANSLATORS: the UPS is very low */
                title = _("UPS critically low");
                tmp = gpm_get_timestring (time_to_empty);
                remaining_text = g_strconcat ("<b>", tmp, "</b>", NULL);
                g_free (tmp);

                /* TRANSLATORS: give the user a ultimatum */
                message = g_strdup_printf (_("Approximately %s of remaining UPS power (%.0f%%). "
                                             "Restore AC power to your computer to avoid losing data."),
                                           remaining_text, percentage);
                g_free (remaining_text);
        } else if (kind == UP_DEVICE_KIND_MOUSE) {
                /* TRANSLATORS: the mouse battery is very low */
                title = _("Mouse battery low");

                /* TRANSLATORS: the device is just going to stop working */
                message = g_strdup_printf (_("Wireless mouse is very low in power (%.0f%%). "
                                             "This device will soon stop functioning if not charged."),
                                           percentage);
        } else if (kind == UP_DEVICE_KIND_KEYBOARD) {
                /* TRANSLATORS: the keyboard battery is very low */
                title = _("Keyboard battery low");

                /* TRANSLATORS: the device is just going to stop working */
                message = g_strdup_printf (_("Wireless keyboard is very low in power (%.0f%%). "
                                             "This device will soon stop functioning if not charged."),
                                           percentage);
        } else if (kind == UP_DEVICE_KIND_PDA) {

                /* TRANSLATORS: the PDA battery is very low */
                title = _("PDA battery low");

                /* TRANSLATORS: the device is just going to stop working */
                message = g_strdup_printf (_("PDA is very low in power (%.0f%%). "
                                             "This device will soon stop functioning if not charged."),
                                           percentage);

        } else if (kind == UP_DEVICE_KIND_PHONE) {

                /* TRANSLATORS: the cell battery is very low */
                title = _("Cell phone battery low");

                /* TRANSLATORS: the device is just going to stop working */
                message = g_strdup_printf (_("Cell phone is very low in power (%.0f%%). "
                                             "This device will soon stop functioning if not charged."),
                                           percentage);

#if UP_CHECK_VERSION(0,9,5)
        } else if (kind == UP_DEVICE_KIND_MEDIA_PLAYER) {

                /* TRANSLATORS: the cell battery is very low */
                title = _("Cell phone battery low");

                /* TRANSLATORS: the device is just going to stop working */
                message = g_strdup_printf (_("Media player is very low in power (%.0f%%). "
                                             "This device will soon stop functioning if not charged."),
                                           percentage);
        } else if (kind == UP_DEVICE_KIND_TABLET) {

                /* TRANSLATORS: the cell battery is very low */
                title = _("Tablet battery low");

                /* TRANSLATORS: the device is just going to stop working */
                message = g_strdup_printf (_("Tablet is very low in power (%.0f%%). "
                                             "This device will soon stop functioning if not charged."),
                                           percentage);
        } else if (kind == UP_DEVICE_KIND_COMPUTER) {

                /* TRANSLATORS: the cell battery is very low */
                title = _("Attached computer battery low");

                /* TRANSLATORS: the device is just going to stop working */
                message = g_strdup_printf (_("Attached computer is very low in power (%.0f%%). "
                                             "The device will soon shutdown if not charged."),
                                           percentage);
#endif
        }

        /* get correct icon */
        icon = gpm_upower_get_device_icon (device, TRUE);

        /* close any existing notification of this class */
        notify_close_if_showing (manager->priv->notification_low);

        /* create a new notification */
        create_notification (title, message,
                             get_first_themed_icon_name (icon),
                             &manager->priv->notification_low);
        notify_notification_set_timeout (manager->priv->notification_low,
                                         GSD_POWER_MANAGER_NOTIFY_TIMEOUT_NEVER);
        notify_notification_set_urgency (manager->priv->notification_low,
                                         NOTIFY_URGENCY_CRITICAL);
        notify_notification_set_app_name (manager->priv->notification_low, _("Power"));

        /* try to show */
        ret = notify_notification_show (manager->priv->notification_low,
                                        &error);
        if (!ret) {
                g_warning ("failed to show notification: %s", error->message);
                g_error_free (error);
                g_object_unref (manager->priv->notification_low);
        }

        switch (kind) {

        case UP_DEVICE_KIND_BATTERY:
        case UP_DEVICE_KIND_UPS:
                g_debug ("critical charge level reached, starting sound loop");
                play_loop_start (manager,
                                 "battery-caution",
                                 _("Battery is critically low"),
                                 TRUE,
                                 GSD_POWER_MANAGER_CRITICAL_ALERT_TIMEOUT);
                break;

        default:
                /* play the sound, using sounds from the naming spec */
                ca_context_play (manager->priv->canberra_context, 0,
                                 CA_PROP_EVENT_ID, "battery-caution",
                                 /* TRANSLATORS: this is the sound description */
                                 CA_PROP_EVENT_DESCRIPTION, _("Battery is critically low"), NULL);
                break;
        }
out:
        if (icon != NULL)
                g_object_unref (icon);
        g_free (message);
}

static void
engine_charge_action (GsdPowerManager *manager, UpDevice *device)
{
        const gchar *title = NULL;
        gboolean ret;
        gchar *message = NULL;
        GError *error = NULL;
        GIcon *icon = NULL;
        GsdPowerActionType policy;
        guint timer_id;
        UpDeviceKind kind;

        /* get device properties */
        g_object_get (device,
                      "kind", &kind,
                      NULL);

        /* check to see if the batteries have not noticed we are on AC */
        if (kind == UP_DEVICE_KIND_BATTERY) {
                if (!up_client_get_on_battery (manager->priv->up_client)) {
                        g_warning ("ignoring critically low message as we are not on battery power");
                        goto out;
                }
        }

        if (kind == UP_DEVICE_KIND_BATTERY) {

                /* TRANSLATORS: laptop battery is really, really, low */
                title = _("Laptop battery critically low");

                /* we have to do different warnings depending on the policy */
                policy = manager_critical_action_get (manager, FALSE);

                /* use different text for different actions */
                if (policy == GSD_POWER_ACTION_NOTHING) {
                        /* TRANSLATORS: computer will shutdown without saving data */
                        message = g_strdup (_("The battery is below the critical level and "
                                              "this computer will <b>power-off</b> when the "
                                              "battery becomes completely empty."));

                } else if (policy == GSD_POWER_ACTION_SUSPEND) {
                        /* TRANSLATORS: computer will suspend */
                        message = g_strdup (_("The battery is below the critical level and "
                                              "this computer is about to suspend.\n"
                                              "<b>NOTE:</b> A small amount of power is required "
                                              "to keep your computer in a suspended state."));

                } else if (policy == GSD_POWER_ACTION_HIBERNATE) {
                        /* TRANSLATORS: computer will hibernate */
                        message = g_strdup (_("The battery is below the critical level and "
                                              "this computer is about to hibernate."));

                } else if (policy == GSD_POWER_ACTION_SHUTDOWN) {
                        /* TRANSLATORS: computer will just shutdown */
                        message = g_strdup (_("The battery is below the critical level and "
                                              "this computer is about to shutdown."));
                }

                /* wait 20 seconds for user-panic */
                timer_id = g_timeout_add_seconds (20, (GSourceFunc) manager_critical_action_do_cb, manager);
                g_source_set_name_by_id (timer_id, "[GsdPowerManager] battery critical-action");

        } else if (kind == UP_DEVICE_KIND_UPS) {
                /* TRANSLATORS: UPS is really, really, low */
                title = _("UPS critically low");

                /* we have to do different warnings depending on the policy */
                policy = manager_critical_action_get (manager, TRUE);

                /* use different text for different actions */
                if (policy == GSD_POWER_ACTION_NOTHING) {
                        /* TRANSLATORS: computer will shutdown without saving data */
                        message = g_strdup (_("UPS is below the critical level and "
                                              "this computer will <b>power-off</b> when the "
                                              "UPS becomes completely empty."));

                } else if (policy == GSD_POWER_ACTION_HIBERNATE) {
                        /* TRANSLATORS: computer will hibernate */
                        message = g_strdup (_("UPS is below the critical level and "
                                              "this computer is about to hibernate."));

                } else if (policy == GSD_POWER_ACTION_SHUTDOWN) {
                        /* TRANSLATORS: computer will just shutdown */
                        message = g_strdup (_("UPS is below the critical level and "
                                              "this computer is about to shutdown."));
                }

                /* wait 20 seconds for user-panic */
                timer_id = g_timeout_add_seconds (20, (GSourceFunc) manager_critical_ups_action_do_cb, manager);
                g_source_set_name_by_id (timer_id, "[GsdPowerManager] ups critical-action");
        }

        /* not all types have actions */
        if (title == NULL)
                return;

        /* get correct icon */
        icon = gpm_upower_get_device_icon (device, TRUE);

        /* close any existing notification of this class */
        notify_close_if_showing (manager->priv->notification_low);

        /* create a new notification */
        create_notification (title, message,
                             get_first_themed_icon_name (icon),
                             &manager->priv->notification_low);
        notify_notification_set_timeout (manager->priv->notification_low,
                                         GSD_POWER_MANAGER_NOTIFY_TIMEOUT_NEVER);
        notify_notification_set_urgency (manager->priv->notification_low,
                                         NOTIFY_URGENCY_CRITICAL);
        notify_notification_set_app_name (manager->priv->notification_low, _("Power"));

        /* try to show */
        ret = notify_notification_show (manager->priv->notification_low,
                                        &error);
        if (!ret) {
                g_warning ("failed to show notification: %s", error->message);
                g_error_free (error);
                g_object_unref (manager->priv->notification_low);
        }

        /* play the sound, using sounds from the naming spec */
        ca_context_play (manager->priv->canberra_context, 0,
                         CA_PROP_EVENT_ID, "battery-caution",
                         /* TRANSLATORS: this is the sound description */
                         CA_PROP_EVENT_DESCRIPTION, _("Battery is critically low"), NULL);
out:
        if (icon != NULL)
                g_object_unref (icon);
        g_free (message);
}

static void
engine_device_changed_cb (UpClient *client, UpDevice *device, GsdPowerManager *manager)
{
        UpDeviceKind kind;
        UpDeviceState state;
        UpDeviceState state_old;
        GsdPowerManagerWarning warning_old;
        GsdPowerManagerWarning warning;

        /* get device properties */
        g_object_get (device,
                      "kind", &kind,
                      NULL);

        /* if battery then use composite device to cope with multiple batteries */
        if (kind == UP_DEVICE_KIND_BATTERY) {
                g_debug ("updating because %s changed", up_device_get_object_path (device));
                device = engine_update_composite_device (manager, device);
        }

        /* get device properties (may be composite) */
        g_object_get (device,
                      "state", &state,
                      NULL);

        g_debug ("%s state is now %s", up_device_get_object_path (device), up_device_state_to_string (state));

        /* see if any interesting state changes have happened */
        state_old = GPOINTER_TO_INT(g_object_get_data (G_OBJECT(device), "engine-state-old"));
        if (state_old != state) {
                if (state == UP_DEVICE_STATE_DISCHARGING) {
                        g_debug ("discharging");
                        engine_ups_discharging (manager, device);
                } else if (state == UP_DEVICE_STATE_FULLY_CHARGED ||
                           state == UP_DEVICE_STATE_CHARGING) {
                        g_debug ("fully charged or charging, hiding notifications if any");
                        notify_close_if_showing (manager->priv->notification_low);
                        notify_close_if_showing (manager->priv->notification_discharging);
                }

                /* save new state */
                g_object_set_data (G_OBJECT(device), "engine-state-old", GUINT_TO_POINTER(state));
        }

        /* check the warning state has not changed */
        warning_old = GPOINTER_TO_INT(g_object_get_data (G_OBJECT(device), "engine-warning-old"));
        warning = engine_get_warning (manager, device);
        if (warning != warning_old) {
                if (warning == WARNING_LOW) {
                        g_debug ("** EMIT: charge-low");
                        engine_charge_low (manager, device);
                } else if (warning == WARNING_CRITICAL) {
                        g_debug ("** EMIT: charge-critical");
                        engine_charge_critical (manager, device);
                } else if (warning == WARNING_ACTION) {
                        g_debug ("charge-action");
                        engine_charge_action (manager, device);
                }
                /* save new state */
                g_object_set_data (G_OBJECT(device), "engine-warning-old", GUINT_TO_POINTER(warning));
        }

        engine_recalculate_state (manager);
}

static UpDevice *
engine_get_primary_device (GsdPowerManager *manager)
{
        guint i;
        UpDevice *device = NULL;
        UpDevice *device_tmp;
        UpDeviceKind kind;
        UpDeviceState state;
        gboolean is_present;

        for (i=0; i<manager->priv->devices_array->len; i++) {
                device_tmp = g_ptr_array_index (manager->priv->devices_array, i);

                /* get device properties */
                g_object_get (device_tmp,
                              "kind", &kind,
                              "state", &state,
                              "is-present", &is_present,
                              NULL);

                /* not present */
                if (!is_present)
                        continue;

                /* not discharging */
                if (state != UP_DEVICE_STATE_DISCHARGING)
                        continue;

                /* not battery */
                if (kind != UP_DEVICE_KIND_BATTERY)
                        continue;

                /* use composite device to cope with multiple batteries */
                device = g_object_ref (engine_get_composite_device (manager, device_tmp));
                break;
        }
        return device;
}

static void
phone_device_added_cb (GpmPhone *phone, guint idx, GsdPowerManager *manager)
{
        UpDevice *device;
        device = up_device_new ();

        g_debug ("phone added %i", idx);

        /* get device properties */
        g_object_set (device,
                      "kind", UP_DEVICE_KIND_PHONE,
                      "is-rechargeable", TRUE,
                      "native-path", g_strdup_printf ("dummy:phone_%i", idx),
                      "is-present", TRUE,
                      NULL);

        /* state changed */
        engine_device_add (manager, device);
        g_ptr_array_add (manager->priv->devices_array, g_object_ref (device));
        engine_recalculate_state (manager);
}

static void
phone_device_removed_cb (GpmPhone *phone, guint idx, GsdPowerManager *manager)
{
        guint i;
        UpDevice *device;
        UpDeviceKind kind;

        g_debug ("phone removed %i", idx);

        for (i=0; i<manager->priv->devices_array->len; i++) {
                device = g_ptr_array_index (manager->priv->devices_array, i);

                /* get device properties */
                g_object_get (device,
                              "kind", &kind,
                              NULL);

                if (kind == UP_DEVICE_KIND_PHONE) {
                        g_ptr_array_remove_index (manager->priv->devices_array, i);
                        break;
                }
        }

        /* state changed */
        engine_recalculate_state (manager);
}

static void
phone_device_refresh_cb (GpmPhone *phone, guint idx, GsdPowerManager *manager)
{
        guint i;
        UpDevice *device;
        UpDeviceKind kind;
        UpDeviceState state;
        gboolean is_present;
        gdouble percentage;

        g_debug ("phone refresh %i", idx);

        for (i=0; i<manager->priv->devices_array->len; i++) {
                device = g_ptr_array_index (manager->priv->devices_array, i);

                /* get device properties */
                g_object_get (device,
                              "kind", &kind,
                              "state", &state,
                              "percentage", &percentage,
                              "is-present", &is_present,
                              NULL);

                if (kind == UP_DEVICE_KIND_PHONE) {
                        is_present = gpm_phone_get_present (phone, idx);
                        state = gpm_phone_get_on_ac (phone, idx) ? UP_DEVICE_STATE_CHARGING : UP_DEVICE_STATE_DISCHARGING;
                        percentage = gpm_phone_get_percentage (phone, idx);
                        break;
                }
        }

        /* state changed */
        engine_recalculate_state (manager);
}

static void
gnome_session_shutdown_cb (GObject *source_object,
                           GAsyncResult *res,
                           gpointer user_data)
{
        GVariant *result;
        GError *error = NULL;

        result = g_dbus_proxy_call_finish (G_DBUS_PROXY (source_object),
                                           res,
                                           &error);
        if (result == NULL) {
                g_warning ("couldn't shutdown using gnome-session: %s",
                           error->message);
                g_error_free (error);
        } else {
                g_variant_unref (result);
        }
}

static void
gnome_session_shutdown (void)
{
        GError *error = NULL;
        GDBusProxy *proxy;

        /* ask gnome-session to show the shutdown dialog with a timeout */
        proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
                                               G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                               NULL,
                                               GNOME_SESSION_DBUS_NAME,
                                               GNOME_SESSION_DBUS_PATH,
                                               GNOME_SESSION_DBUS_INTERFACE,
                                               NULL, &error);
        if (proxy == NULL) {
                g_warning ("cannot connect to gnome-session: %s",
                           error->message);
                g_error_free (error);
                return;
        }
        g_dbus_proxy_call (proxy,
                           "Shutdown",
                           NULL,
                           G_DBUS_CALL_FLAGS_NONE,
                           -1, NULL,
                           gnome_session_shutdown_cb, NULL);
        g_object_unref (proxy);
}

static void
do_power_action_type (GsdPowerManager *manager,
                      GsdPowerActionType action_type)
{
        gboolean ret;
        GError *error = NULL;

        switch (action_type) {
        case GSD_POWER_ACTION_SUSPEND:
                gsd_power_suspend (manager->priv->upower_proxy);
                break;
        case GSD_POWER_ACTION_INTERACTIVE:
                gnome_session_shutdown ();
                break;
        case GSD_POWER_ACTION_HIBERNATE:
                gsd_power_hibernate (manager->priv->upower_proxy);
                break;
        case GSD_POWER_ACTION_SHUTDOWN:
                /* this is only used on critically low battery where
                 * hibernate is not available and is marginally better
                 * than just powering down the computer mid-write */
                gsd_power_poweroff ();
                break;
        case GSD_POWER_ACTION_BLANK:
                ret = gnome_rr_screen_set_dpms_mode (manager->priv->x11_screen,
                                                     GNOME_RR_DPMS_OFF,
                                                     &error);
                if (!ret) {
                        g_warning ("failed to turn the panel off for policy action: %s",
                                   error->message);
                        g_error_free (error);
                }
                break;
        case GSD_POWER_ACTION_NOTHING:
                break;
        }
}

static gboolean
upower_kbd_set_brightness (GsdPowerManager *manager, guint value, GError **error)
{
        GVariant *retval;

        /* same as before */
        if (manager->priv->kbd_brightness_now == value)
                return TRUE;

        /* update h/w value */
        retval = g_dbus_proxy_call_sync (manager->priv->upower_kdb_proxy,
                                         "SetBrightness",
                                         g_variant_new ("(i)", (gint) value),
                                         G_DBUS_CALL_FLAGS_NONE,
                                         -1,
                                         NULL,
                                         error);
        if (retval == NULL)
                return FALSE;

        /* save new value */
        manager->priv->kbd_brightness_now = value;
        g_variant_unref (retval);
        return TRUE;
}

static gboolean
upower_kbd_toggle (GsdPowerManager *manager,
                   GError **error)
{
        gboolean ret;

        if (manager->priv->kbd_brightness_old >= 0) {
                g_debug ("keyboard toggle off");
                ret = upower_kbd_set_brightness (manager,
                                                 manager->priv->kbd_brightness_old,
                                                 error);
                if (ret) {
                        /* succeeded, set to -1 since now no old value */
                        manager->priv->kbd_brightness_old = -1;
                }
        } else {
                g_debug ("keyboard toggle on");
                /* save the current value to restore later when untoggling */
                manager->priv->kbd_brightness_old = manager->priv->kbd_brightness_now;
                ret = upower_kbd_set_brightness (manager, 0, error);
                if (!ret) {
                        /* failed, reset back to -1 */
                        manager->priv->kbd_brightness_old = -1;
                }
        }

        return ret;
}

static void
do_lid_open_action (GsdPowerManager *manager)
{
        gboolean ret;
        GError *error = NULL;

        /* play a sound, using sounds from the naming spec */
        ca_context_play (manager->priv->canberra_context, 0,
                         CA_PROP_EVENT_ID, "lid-open",
                         /* TRANSLATORS: this is the sound description */
                         CA_PROP_EVENT_DESCRIPTION, _("Lid has been opened"),
                         NULL);

        /* ensure we turn the panel back on after lid open */
        ret = gnome_rr_screen_set_dpms_mode (manager->priv->x11_screen,
                                             GNOME_RR_DPMS_ON,
                                             &error);
        if (!ret) {
                g_warning ("failed to turn the panel on after lid open: %s",
                           error->message);
                g_clear_error (&error);
        }

        /* only toggle keyboard if present and already toggled off */
        if (manager->priv->upower_kdb_proxy != NULL &&
            manager->priv->kbd_brightness_old != -1) {
                ret = upower_kbd_toggle (manager, &error);
                if (!ret) {
                        g_warning ("failed to turn the kbd backlight on: %s",
                                   error->message);
                        g_error_free (error);
                }
        }

        kill_lid_close_safety_timer (manager);
}

static gboolean
is_on (GnomeRROutput *output)
{
	GnomeRRCrtc *crtc;

	crtc = gnome_rr_output_get_crtc (output);
	if (!crtc)
		return FALSE;
	return gnome_rr_crtc_get_current_mode (crtc) != NULL;
}

static gboolean
non_laptop_outputs_are_all_off (GnomeRRScreen *screen)
{
        GnomeRROutput **outputs;
        int i;

        outputs = gnome_rr_screen_list_outputs (screen);
        for (i = 0; outputs[i] != NULL; i++) {
                if (gnome_rr_output_is_laptop (outputs[i]))
                        continue;

                if (is_on (outputs[i]))
                        return FALSE;
        }

        return TRUE;
}

/* Timeout callback used to check conditions when the laptop's lid is closed but
 * the machine is not suspended yet.  We try to suspend again, so that the laptop
 * won't overheat if placed in a backpack.
 */
static gboolean
lid_close_safety_timer_cb (GsdPowerManager *manager)
{
        manager->priv->lid_close_safety_timer_id = 0;

        g_debug ("lid has been closed for a while; trying to suspend again");
        do_lid_closed_action (manager);

        return FALSE;
}

/* Sets up a timer to be triggered some seconds after closing the laptop lid
 * when the laptop is *not* suspended for some reason.  We'll check conditions
 * again in the timeout handler to see if we can suspend then.
 */
static void
setup_lid_close_safety_timer (GsdPowerManager *manager)
{
        if (manager->priv->lid_close_safety_timer_id != 0)
                return;

        manager->priv->lid_close_safety_timer_id = g_timeout_add_seconds (GSD_POWER_MANAGER_LID_CLOSE_SAFETY_TIMEOUT,
                                                                          (GSourceFunc) lid_close_safety_timer_cb,
                                                                          manager);
        g_source_set_name_by_id (manager->priv->lid_close_safety_timer_id, "[GsdPowerManager] lid close safety timer");
}

static void
kill_lid_close_safety_timer (GsdPowerManager *manager)
{
        if (manager->priv->lid_close_safety_timer_id != 0) {
                g_source_remove (manager->priv->lid_close_safety_timer_id);
                manager->priv->lid_close_safety_timer_id = 0;
        }
}

static void
suspend_with_lid_closed (GsdPowerManager *manager)
{
        gboolean ret;
        GError *error = NULL;
        GsdPowerActionType action_type;

        /* maybe lock the screen if the lid is closed */
        lock_screensaver (manager);

        /* we have different settings depending on AC state */
        if (up_client_get_on_battery (manager->priv->up_client)) {
                action_type = g_settings_get_enum (manager->priv->settings,
                                                   "lid-close-battery-action");
        } else {
                action_type = g_settings_get_enum (manager->priv->settings,
                                                   "lid-close-ac-action");
        }

        /* check we won't melt when the lid is closed */
        if (action_type != GSD_POWER_ACTION_SUSPEND &&
            action_type != GSD_POWER_ACTION_HIBERNATE) {
                if (up_client_get_lid_force_sleep (manager->priv->up_client)) {
                        g_warning ("to prevent damage, now forcing suspend");
                        do_power_action_type (manager, GSD_POWER_ACTION_SUSPEND);
                        return;
                }
        }

        /* ensure we turn the panel back on after resume */
        ret = gnome_rr_screen_set_dpms_mode (manager->priv->x11_screen,
                                             GNOME_RR_DPMS_OFF,
                                             &error);
        if (!ret) {
                g_warning ("failed to turn the panel off after lid close: %s",
                           error->message);
                g_clear_error (&error);
        }

        /* only toggle keyboard if present and not already toggled */
        if (manager->priv->upower_kdb_proxy &&
            manager->priv->kbd_brightness_old == -1) {
                ret = upower_kbd_toggle (manager, &error);
                if (!ret) {
                        g_warning ("failed to turn the kbd backlight off: %s",
                                   error->message);
                        g_error_free (error);
                }
        }

        do_power_action_type (manager, action_type);
}

static void
do_lid_closed_action (GsdPowerManager *manager)
{
        /* play a sound, using sounds from the naming spec */
        ca_context_play (manager->priv->canberra_context, 0,
                         CA_PROP_EVENT_ID, "lid-close",
                         /* TRANSLATORS: this is the sound description */
                         CA_PROP_EVENT_DESCRIPTION, _("Lid has been closed"),
                         NULL);

        /* refresh RANDR so we get an accurate view of what monitors are plugged in when the lid is closed */
        gnome_rr_screen_refresh (manager->priv->x11_screen, NULL); /* NULL-GError */

        /* perform policy action */
        if (g_settings_get_boolean (manager->priv->settings, "lid-close-suspend-with-external-monitor")
            || non_laptop_outputs_are_all_off (manager->priv->x11_screen)) {
                g_debug ("lid is closed; suspending or hibernating");
                suspend_with_lid_closed (manager);
        } else {
                g_debug ("lid is closed; not suspending nor hibernating since some external monitor outputs are still active");
                setup_lid_close_safety_timer (manager);
        }
}


static void
up_client_changed_cb (UpClient *client, GsdPowerManager *manager)
{
        gboolean tmp;

        if (!up_client_get_on_battery (client)) {
            /* if we are playing a critical charge sound loop on AC, stop it */
            if (manager->priv->critical_alert_timeout_id > 0) {
                 g_debug ("stopping alert loop due to ac being present");
                 play_loop_stop (manager);
            }
            notify_close_if_showing (manager->priv->notification_low);
        }

        /* same state */
        tmp = up_client_get_lid_is_closed (manager->priv->up_client);
        if (manager->priv->lid_is_closed == tmp)
                return;
        manager->priv->lid_is_closed = tmp;

        /* fake a keypress */
        if (tmp)
                do_lid_closed_action (manager);
        else
                do_lid_open_action (manager);
}

typedef enum {
        SESSION_STATUS_CODE_AVAILABLE = 0,
        SESSION_STATUS_CODE_INVISIBLE,
        SESSION_STATUS_CODE_BUSY,
        SESSION_STATUS_CODE_IDLE,
        SESSION_STATUS_CODE_UNKNOWN
} SessionStatusCode;

typedef enum {
        SESSION_INHIBIT_MASK_LOGOUT = 1,
        SESSION_INHIBIT_MASK_SWITCH = 2,
        SESSION_INHIBIT_MASK_SUSPEND = 4,
        SESSION_INHIBIT_MASK_IDLE = 8
} SessionInhibitMask;

static const gchar *
idle_mode_to_string (GsdPowerIdleMode mode)
{
        if (mode == GSD_POWER_IDLE_MODE_NORMAL)
                return "normal";
        if (mode == GSD_POWER_IDLE_MODE_DIM)
                return "dim";
        if (mode == GSD_POWER_IDLE_MODE_BLANK)
                return "blank";
        if (mode == GSD_POWER_IDLE_MODE_SLEEP)
                return "sleep";
        return "unknown";
}

static GnomeRROutput *
get_primary_output (GsdPowerManager *manager)
{
        GnomeRROutput *output = NULL;
        GnomeRROutput **outputs;
        guint i;

        /* search all X11 outputs for the device id */
        outputs = gnome_rr_screen_list_outputs (manager->priv->x11_screen);
        if (outputs == NULL)
                goto out;

        for (i = 0; outputs[i] != NULL; i++) {
                if (gnome_rr_output_is_connected (outputs[i]) &&
                    gnome_rr_output_is_laptop (outputs[i]) &&
                    gnome_rr_output_get_backlight_min (outputs[i]) >= 0 &&
                    gnome_rr_output_get_backlight_max (outputs[i]) > 0) {
                        output = outputs[i];
                        break;
                }
        }
out:
        return output;
}

/**
 * backlight_helper_get_value:
 *
 * Gets a brightness value from the PolicyKit helper.
 *
 * Return value: the signed integer value from the helper, or -1
 * for failure. If -1 then @error is set.
 **/
static gint64
backlight_helper_get_value (const gchar *argument, GError **error)
{
        gboolean ret;
        gchar *stdout_data = NULL;
        gint exit_status = 0;
        gint64 value = -1;
        gchar *command = NULL;
        gchar *endptr = NULL;

#ifndef __linux__
        /* non-Linux platforms won't have /sys/class/backlight */
        g_set_error_literal (error,
                             GSD_POWER_MANAGER_ERROR,
                             GSD_POWER_MANAGER_ERROR_FAILED,
                             "The sysfs backlight helper is only for Linux");
        goto out;
#endif

        /* get the data */
        command = g_strdup_printf (LIBEXECDIR "/gsd-backlight-helper --%s",
                                   argument);
        ret = g_spawn_command_line_sync (command,
                                         &stdout_data,
                                         NULL,
                                         &exit_status,
                                         error);
        g_debug ("executed %s retval: %i", command, exit_status);

        if (!ret)
                goto out;

        if (WEXITSTATUS (exit_status) != 0) {
                 g_set_error (error,
                             GSD_POWER_MANAGER_ERROR,
                             GSD_POWER_MANAGER_ERROR_FAILED,
                             "gsd-backlight-helper failed: %s",
                             stdout_data ? stdout_data : "No reason");
                goto out;
        }

        /* parse */
        value = g_ascii_strtoll (stdout_data, &endptr, 10);

        /* parsing error */
        if (endptr == stdout_data) {
                value = -1;
                g_set_error (error,
                             GSD_POWER_MANAGER_ERROR,
                             GSD_POWER_MANAGER_ERROR_FAILED,
                             "failed to parse value: %s",
                             stdout_data);
                goto out;
        }

        /* out of range */
        if (value > G_MAXINT) {
                value = -1;
                g_set_error (error,
                             GSD_POWER_MANAGER_ERROR,
                             GSD_POWER_MANAGER_ERROR_FAILED,
                             "value out of range: %s",
                             stdout_data);
                goto out;
        }

        /* Fetching the value failed, for some other reason */
        if (value < 0) {
                g_set_error (error,
                             GSD_POWER_MANAGER_ERROR,
                             GSD_POWER_MANAGER_ERROR_FAILED,
                             "value negative, but helper did not fail: %s",
                             stdout_data);
                goto out;
        }

out:
        g_free (command);
        g_free (stdout_data);
        return value;
}

/**
 * backlight_helper_set_value:
 *
 * Sets a brightness value using the PolicyKit helper.
 *
 * Return value: Success. If FALSE then @error is set.
 **/
static gboolean
backlight_helper_set_value (const gchar *argument,
                            gint value,
                            GError **error)
{
        gboolean ret;
        gint exit_status = 0;
        gchar *command = NULL;

#ifndef __linux__
        /* non-Linux platforms won't have /sys/class/backlight */
        g_set_error_literal (error,
                             GSD_POWER_MANAGER_ERROR,
                             GSD_POWER_MANAGER_ERROR_FAILED,
                             "The sysfs backlight helper is only for Linux");
        goto out;
#endif

        /* get the data */
        command = g_strdup_printf ("pkexec " LIBEXECDIR "/gsd-backlight-helper --%s %i",
                                   argument, value);
        ret = g_spawn_command_line_sync (command,
                                         NULL,
                                         NULL,
                                         &exit_status,
                                         error);

        g_debug ("executed %s retval: %i", command, exit_status);

        if (!ret || WEXITSTATUS (exit_status) != 0)
                goto out;

out:
        g_free (command);
        return ret;
}

static gint
backlight_get_abs (GsdPowerManager *manager, GError **error)
{
        GnomeRROutput *output;

        /* prefer xbacklight */
        output = get_primary_output (manager);
        if (output != NULL) {
                return gnome_rr_output_get_backlight (output,
                                                      error);
        }

        /* fall back to the polkit helper */
        return backlight_helper_get_value ("get-brightness", error);
}

static gint
backlight_get_percentage (GsdPowerManager *manager, GError **error)
{
        GnomeRROutput *output;
        gint now;
        gint value = -1;
        gint min = 0;
        gint max;

        /* prefer xbacklight */
        output = get_primary_output (manager);
        if (output != NULL) {

                min = gnome_rr_output_get_backlight_min (output);
                max = gnome_rr_output_get_backlight_max (output);
                now = gnome_rr_output_get_backlight (output, error);
                if (now < 0)
                        goto out;
                value = ABS_TO_PERCENTAGE (min, max, now);
                goto out;
        }

        /* fall back to the polkit helper */
        max = backlight_helper_get_value ("get-max-brightness", error);
        if (max < 0)
                goto out;
        now = backlight_helper_get_value ("get-brightness", error);
        if (now < 0)
                goto out;
        value = ABS_TO_PERCENTAGE (min, max, now);
out:
        return value;
}

static gint
backlight_get_min (GsdPowerManager *manager)
{
        GnomeRROutput *output;

        /* if we have no xbacklight device, then hardcode zero as sysfs
         * offsets everything to 0 as min */
        output = get_primary_output (manager);
        if (output == NULL)
                return 0;

        /* get xbacklight value, which maybe non-zero */
        return gnome_rr_output_get_backlight_min (output);
}

static gint
backlight_get_max (GsdPowerManager *manager, GError **error)
{
        gint value;
        GnomeRROutput *output;

        /* prefer xbacklight */
        output = get_primary_output (manager);
        if (output != NULL) {
                value = gnome_rr_output_get_backlight_max (output);
                if (value < 0) {
                        g_set_error (error,
                                     GSD_POWER_MANAGER_ERROR,
                                     GSD_POWER_MANAGER_ERROR_FAILED,
                                     "failed to get backlight max");
                }
                return value;
        }

        /* fall back to the polkit helper */
        return  backlight_helper_get_value ("get-max-brightness", error);
}

static void
backlight_emit_changed (GsdPowerManager *manager)
{
        gboolean ret;
        GError *error = NULL;

        /* not yet connected to the bus */
        if (manager->priv->connection == NULL)
                return;
        ret = g_dbus_connection_emit_signal (manager->priv->connection,
                                             GSD_DBUS_SERVICE,
                                             GSD_POWER_DBUS_PATH,
                                             GSD_POWER_DBUS_INTERFACE_SCREEN,
                                             "Changed",
                                             NULL,
                                             &error);
        if (!ret) {
                g_warning ("failed to emit Changed: %s", error->message);
                g_error_free (error);
        }
}

static gboolean
backlight_set_percentage (GsdPowerManager *manager,
                          guint value,
                          gboolean emit_changed,
                          GError **error)
{
        GnomeRROutput *output;
        gboolean ret = FALSE;
        gint min = 0;
        gint max;
        guint discrete;

        /* prefer xbacklight */
        output = get_primary_output (manager);
        if (output != NULL) {
                min = gnome_rr_output_get_backlight_min (output);
                max = gnome_rr_output_get_backlight_max (output);
                if (min < 0 || max < 0) {
                        g_warning ("no xrandr backlight capability");
                        goto out;
                }
                discrete = PERCENTAGE_TO_ABS (min, max, value);
                ret = gnome_rr_output_set_backlight (output,
                                                     discrete,
                                                     error);
                goto out;
        }

        /* fall back to the polkit helper */
        max = backlight_helper_get_value ("get-max-brightness", error);
        if (max < 0)
                goto out;
        discrete = PERCENTAGE_TO_ABS (min, max, value);
        ret = backlight_helper_set_value ("set-brightness",
                                          discrete,
                                          error);
out:
        if (ret && emit_changed)
                backlight_emit_changed (manager);
        return ret;
}

static gint
backlight_step_up (GsdPowerManager *manager, GError **error)
{
        GnomeRROutput *output;
        gboolean ret = FALSE;
        gint percentage_value = -1;
        gint min = 0;
        gint max;
        gint now;
        gint step;
        guint discrete;
        GnomeRRCrtc *crtc;

        /* prefer xbacklight */
        output = get_primary_output (manager);
        if (output != NULL) {

                crtc = gnome_rr_output_get_crtc (output);
                if (crtc == NULL) {
                        g_set_error (error,
                                     GSD_POWER_MANAGER_ERROR,
                                     GSD_POWER_MANAGER_ERROR_FAILED,
                                     "no crtc for %s",
                                     gnome_rr_output_get_name (output));
                        goto out;
                }
                min = gnome_rr_output_get_backlight_min (output);
                max = gnome_rr_output_get_backlight_max (output);
                now = gnome_rr_output_get_backlight (output, error);
                if (now < 0)
                       goto out;
                step = BRIGHTNESS_STEP_AMOUNT (max - min + 1);
                discrete = MIN (now + step, max);
                ret = gnome_rr_output_set_backlight (output,
                                                     discrete,
                                                     error);
                if (ret)
                        percentage_value = ABS_TO_PERCENTAGE (min, max, discrete);
                goto out;
        }

        /* fall back to the polkit helper */
        now = backlight_helper_get_value ("get-brightness", error);
        if (now < 0)
                goto out;
        max = backlight_helper_get_value ("get-max-brightness", error);
        if (max < 0)
                goto out;
        step = BRIGHTNESS_STEP_AMOUNT (max - min + 1);
        discrete = MIN (now + step, max);
        ret = backlight_helper_set_value ("set-brightness",
                                          discrete,
                                          error);
        if (ret)
                percentage_value = ABS_TO_PERCENTAGE (min, max, discrete);
out:
        if (ret)
                backlight_emit_changed (manager);
        return percentage_value;
}

static gint
backlight_step_down (GsdPowerManager *manager, GError **error)
{
        GnomeRROutput *output;
        gboolean ret = FALSE;
        gint percentage_value = -1;
        gint min = 0;
        gint max;
        gint now;
        gint step;
        guint discrete;
        GnomeRRCrtc *crtc;

        /* prefer xbacklight */
        output = get_primary_output (manager);
        if (output != NULL) {

                crtc = gnome_rr_output_get_crtc (output);
                if (crtc == NULL) {
                        g_set_error (error,
                                     GSD_POWER_MANAGER_ERROR,
                                     GSD_POWER_MANAGER_ERROR_FAILED,
                                     "no crtc for %s",
                                     gnome_rr_output_get_name (output));
                        goto out;
                }
                min = gnome_rr_output_get_backlight_min (output);
                max = gnome_rr_output_get_backlight_max (output);
                now = gnome_rr_output_get_backlight (output, error);
                if (now < 0)
                       goto out;
                step = BRIGHTNESS_STEP_AMOUNT (max - min + 1);
                discrete = MAX (now - step, 0);
                ret = gnome_rr_output_set_backlight (output,
                                                     discrete,
                                                     error);
                if (ret)
                        percentage_value = ABS_TO_PERCENTAGE (min, max, discrete);
                goto out;
        }

        /* fall back to the polkit helper */
        now = backlight_helper_get_value ("get-brightness", error);
        if (now < 0)
                goto out;
        max = backlight_helper_get_value ("get-max-brightness", error);
        if (max < 0)
                goto out;
        step = BRIGHTNESS_STEP_AMOUNT (max - min + 1);
        discrete = MAX (now - step, 0);
        ret = backlight_helper_set_value ("set-brightness",
                                          discrete,
                                          error);
        if (ret)
                percentage_value = ABS_TO_PERCENTAGE (min, max, discrete);
out:
        if (ret)
                backlight_emit_changed (manager);
        return percentage_value;
}

static gint
backlight_set_abs (GsdPowerManager *manager,
                   guint value,
                   gboolean emit_changed,
                   GError **error)
{
        GnomeRROutput *output;
        gboolean ret = FALSE;

        /* prefer xbacklight */
        output = get_primary_output (manager);
        if (output != NULL) {
                ret = gnome_rr_output_set_backlight (output,
                                                     value,
                                                     error);
                goto out;
        }

        /* fall back to the polkit helper */
        ret = backlight_helper_set_value ("set-brightness",
                                          value,
                                          error);
out:
        if (ret && emit_changed)
                backlight_emit_changed (manager);
        return ret;
}

static gboolean
display_backlight_dim (GsdPowerManager *manager,
                       gint idle_percentage,
                       GError **error)
{
        gint min;
        gint max;
        gint now;
        gint idle;
        gboolean ret = FALSE;

        now = backlight_get_abs (manager, error);
        if (now < 0) {
                goto out;
        }

        /* is the dim brightness actually *dimmer* than the
         * brightness we have now? */
        min = backlight_get_min (manager);
        max = backlight_get_max (manager, error);
        if (max < 0) {
                goto out;
        }
        idle = PERCENTAGE_TO_ABS (min, max, idle_percentage);
        if (idle > now) {
                g_debug ("brightness already now %i/%i, so "
                         "ignoring dim to %i/%i",
                         now, max, idle, max);
                ret = TRUE;
                goto out;
        }
        ret = backlight_set_abs (manager,
                                 idle,
                                 FALSE,
                                 error);
        if (!ret) {
                goto out;
        }

        /* save for undim */
        manager->priv->pre_dim_brightness = now;

out:
        return ret;
}

static gboolean
kbd_backlight_dim (GsdPowerManager *manager,
                   gint idle_percentage,
                   GError **error)
{
        gboolean ret;
        gint idle;
        gint max;
        gint now;

        if (manager->priv->upower_kdb_proxy == NULL)
                return TRUE;

        now = manager->priv->kbd_brightness_now;
        max = manager->priv->kbd_brightness_max;
        idle = PERCENTAGE_TO_ABS (0, max, idle_percentage);
        if (idle > now) {
                g_debug ("kbd brightness already now %i/%i, so "
                         "ignoring dim to %i/%i",
                         now, max, idle, max);
                return TRUE;
        }
        ret = upower_kbd_set_brightness (manager, idle, error);
        if (!ret)
                return FALSE;

        /* save for undim */
        manager->priv->kbd_brightness_pre_dim = now;
        return TRUE;
}

static void
idle_set_mode (GsdPowerManager *manager, GsdPowerIdleMode mode)
{
        gboolean ret = FALSE;
        GError *error = NULL;
        gint idle_percentage;
        GsdPowerActionType action_type;
        GnomeSettingsSessionState state;

        if (mode == manager->priv->current_idle_mode)
                return;

        /* Ignore attempts to set "less idle" modes */
        if (mode < manager->priv->current_idle_mode &&
            mode != GSD_POWER_IDLE_MODE_NORMAL)
                return;

        /* ensure we're still on an active console */
        state = gnome_settings_session_get_state (manager->priv->session);
        if (state == GNOME_SETTINGS_SESSION_STATE_INACTIVE) {
                g_debug ("ignoring state transition to %s as inactive",
                         idle_mode_to_string (mode));
                return;
        }

        manager->priv->current_idle_mode = mode;
        g_debug ("Doing a state transition: %s", idle_mode_to_string (mode));

        /* don't do any power saving if we're a VM */
        if (manager->priv->is_virtual_machine) {
                g_debug ("ignoring state transition to %s as virtual machine",
                         idle_mode_to_string (mode));
                return;
        }

        /* save current brightness, and set dim level */
        if (mode == GSD_POWER_IDLE_MODE_DIM) {

                /* have we disabled the action */
                if (up_client_get_on_battery (manager->priv->up_client)) {
                        ret = g_settings_get_boolean (manager->priv->settings,
                                                      "idle-dim-battery");
                } else {
                        ret = g_settings_get_boolean (manager->priv->settings,
                                                      "idle-dim-ac");
                }
                if (!ret) {
                        g_debug ("not dimming due to policy");
                        return;
                }

                /* display backlight */
                idle_percentage = g_settings_get_int (manager->priv->settings,
                                                      "idle-brightness");
                ret = display_backlight_dim (manager, idle_percentage, &error);
                if (!ret) {
                        g_warning ("failed to set dim backlight to %i%%: %s",
                                   idle_percentage,
                                   error->message);
                        g_clear_error (&error);
                }

                /* keyboard backlight */
                ret = kbd_backlight_dim (manager, idle_percentage, &error);
                if (!ret) {
                        g_warning ("failed to set dim kbd backlight to %i%%: %s",
                                   idle_percentage,
                                   error->message);
                        g_clear_error (&error);
                }

        /* turn off screen and kbd */
        } else if (mode == GSD_POWER_IDLE_MODE_BLANK) {

                ret = gnome_rr_screen_set_dpms_mode (manager->priv->x11_screen,
                                                     GNOME_RR_DPMS_OFF,
                                                     &error);
                if (!ret) {
                        g_warning ("failed to turn the panel off: %s",
                                   error->message);
                        g_clear_error (&error);
                }

                /* only toggle keyboard if present and not already toggled */
                if (manager->priv->upower_kdb_proxy &&
                    manager->priv->kbd_brightness_old == -1) {
                        ret = upower_kbd_toggle (manager, &error);
                        if (!ret) {
                                g_warning ("failed to turn the kbd backlight off: %s",
                                           error->message);
                                g_error_free (error);
                        }
                }

        /* sleep */
        } else if (mode == GSD_POWER_IDLE_MODE_SLEEP) {

                if (up_client_get_on_battery (manager->priv->up_client)) {
                        action_type = g_settings_get_enum (manager->priv->settings,
                                                           "sleep-inactive-battery-type");
                } else {
                        action_type = g_settings_get_enum (manager->priv->settings,
                                                           "sleep-inactive-ac-type");
                }
                do_power_action_type (manager, action_type);

        /* turn on screen and restore user-selected brightness level */
        } else if (mode == GSD_POWER_IDLE_MODE_NORMAL) {

                ret = gnome_rr_screen_set_dpms_mode (manager->priv->x11_screen,
                                                     GNOME_RR_DPMS_ON,
                                                     &error);
                if (!ret) {
                        g_warning ("failed to turn the panel on: %s",
                                   error->message);
                        g_clear_error (&error);
                }

                /* reset brightness if we dimmed */
                if (manager->priv->pre_dim_brightness >= 0) {
                        ret = backlight_set_abs (manager,
                                                 manager->priv->pre_dim_brightness,
                                                 FALSE,
                                                 &error);
                        if (!ret) {
                                g_warning ("failed to restore backlight to %i: %s",
                                           manager->priv->pre_dim_brightness,
                                           error->message);
                                g_clear_error (&error);
                        } else {
                                manager->priv->pre_dim_brightness = -1;
                        }
                }

                /* only toggle keyboard if present and already toggled off */
                if (manager->priv->upower_kdb_proxy &&
                    manager->priv->kbd_brightness_old != -1) {
                        ret = upower_kbd_toggle (manager, &error);
                        if (!ret) {
                                g_warning ("failed to turn the kbd backlight on: %s",
                                           error->message);
                                g_clear_error (&error);
                        }
                }

                /* reset kbd brightness if we dimmed */
                if (manager->priv->kbd_brightness_pre_dim >= 0) {
                        ret = upower_kbd_set_brightness (manager,
                                                         manager->priv->kbd_brightness_pre_dim,
                                                         &error);
                        if (!ret) {
                                g_warning ("failed to restore kbd backlight to %i: %s",
                                           manager->priv->kbd_brightness_pre_dim,
                                           error->message);
                                g_error_free (error);
                        }
                        manager->priv->kbd_brightness_pre_dim = -1;
                }

        }
}

static gboolean
idle_is_session_idle (GsdPowerManager *manager)
{
        gboolean ret;
        GVariant *result;
        guint status;

        /* not yet connected to gnome-session */
        if (manager->priv->session_presence_proxy == NULL) {
                g_warning ("session idleness not available, gnome-session is not available");
                return FALSE;
        }

        /* get the session status */
        result = g_dbus_proxy_get_cached_property (manager->priv->session_presence_proxy,
                                                   "status");
        if (result == NULL) {
                g_warning ("no readable status property on %s",
                           g_dbus_proxy_get_interface_name (manager->priv->session_presence_proxy));
                return FALSE;
        }

        g_variant_get (result, "u", &status);
        ret = (status == SESSION_STATUS_CODE_IDLE);
        g_variant_unref (result);

        return ret;
}

static gboolean
idle_is_session_inhibited (GsdPowerManager *manager, guint mask)
{
        gboolean ret;
        GVariant *retval = NULL;
        GError *error = NULL;

        /* not yet connected to gnome-session */
        if (manager->priv->session_proxy == NULL) {
                g_warning ("session inhibition not available, gnome-session is not available");
                return FALSE;
        }

        retval = g_dbus_proxy_call_sync (manager->priv->session_proxy,
                                         "IsInhibited",
                                         g_variant_new ("(u)",
                                                        mask),
                                         G_DBUS_CALL_FLAGS_NONE,
                                         -1, NULL,
                                         &error);
        if (retval == NULL) {
                /* abort as the DBUS method failed */
                g_warning ("IsInhibited failed: %s", error->message);
                g_error_free (error);
                return FALSE;
        }

        g_variant_get (retval, "(b)", &ret);
        g_variant_unref (retval);

        return ret;
}

/**
 *  idle_adjust_timeout:
 *  @idle_time: Current idle time, in seconds.
 *  @timeout: The new timeout we want to set, in seconds.
 *
 *  On slow machines, or machines that have lots to load duing login,
 *  the current idle time could be bigger than the requested timeout.
 *  In this case the scheduled idle timeout will never fire, unless
 *  some user activity (keyboard, mouse) resets the current idle time.
 *  Instead of relying on user activity to correct this issue, we need
 *  to adjust timeout, as related to current idle time, so the idle
 *  timeout will fire as designed.
 *
 *  Return value: timeout to set, adjusted acccording to current idle time.
 **/
static guint
idle_adjust_timeout (guint idle_time, guint timeout)
{
        /* allow 2 sec margin for messaging delay. */
        idle_time += 2;

        /* Double timeout until it's larger than current idle time.
         * Give up for ultra slow machines. (86400 sec = 24 hours) */
        while (timeout < idle_time &&
               timeout < 86400 &&
               timeout > 0) {
                timeout *= 2;
        }
        return timeout;
}

/**
 * idle_adjust_timeout_blank:
 * @idle_time: current idle time, in seconds.
 * @timeout: the new timeout we want to set, in seconds.
 *
 * Same as idle_adjust_timeout(), but also accounts for the duration
 * of the fading animation in the screensaver (so that blanking happens
 * exactly at the end of it, if configured with the same timeouts)
 */
static guint
idle_adjust_timeout_blank (guint idle_time, guint timeout)
{
        return idle_adjust_timeout (idle_time,
                                    timeout + SCREENSAVER_FADE_TIME);
}

static void
idle_configure (GsdPowerManager *manager)
{
        gboolean is_idle_inhibited;
        guint current_idle_time;
        guint timeout_blank;
        guint timeout_sleep;
        gboolean on_battery;

        /* are we inhibited from going idle */
        is_idle_inhibited = idle_is_session_inhibited (manager,
                                                       SESSION_INHIBIT_MASK_IDLE);
        if (is_idle_inhibited) {
                g_debug ("inhibited, so using normal state");
                idle_set_mode (manager, GSD_POWER_IDLE_MODE_NORMAL);

                gpm_idletime_alarm_remove (manager->priv->idletime,
                                           GSD_POWER_IDLETIME_BLANK_ID);
                gpm_idletime_alarm_remove (manager->priv->idletime,
                                           GSD_POWER_IDLETIME_SLEEP_ID);
                return;
        }

        current_idle_time = gpm_idletime_get_time (manager->priv->idletime) / 1000;

        /* set up blank callback even when session is not idle,
         * but only if we actually want to blank. */
        on_battery = up_client_get_on_battery (manager->priv->up_client);
        if (on_battery) {
                timeout_blank = g_settings_get_int (manager->priv->settings,
                                                    "sleep-display-battery");
        } else {
                timeout_blank = g_settings_get_int (manager->priv->settings,
                                                    "sleep-display-ac");
        }
        if (timeout_blank != 0) {
                g_debug ("setting up blank callback for %is", timeout_blank);

                gpm_idletime_alarm_set (manager->priv->idletime,
                                        GSD_POWER_IDLETIME_BLANK_ID,
                                        idle_adjust_timeout_blank (current_idle_time, timeout_blank) * 1000);
        } else {
                gpm_idletime_alarm_remove (manager->priv->idletime,
                                           GSD_POWER_IDLETIME_BLANK_ID);
        }

        /* only do the sleep timeout when the session is idle
         * and we aren't inhibited from sleeping */
        if (on_battery) {
                timeout_sleep = g_settings_get_int (manager->priv->settings,
                                                    "sleep-inactive-battery-timeout");
        } else {
                timeout_sleep = g_settings_get_int (manager->priv->settings,
                                                    "sleep-inactive-ac-timeout");
        }
        if (timeout_sleep != 0) {
                g_debug ("setting up sleep callback %is", timeout_sleep);

                gpm_idletime_alarm_set (manager->priv->idletime,
                                        GSD_POWER_IDLETIME_SLEEP_ID,
                                        idle_adjust_timeout (current_idle_time, timeout_sleep) * 1000);
        } else {
                gpm_idletime_alarm_remove (manager->priv->idletime,
                                           GSD_POWER_IDLETIME_SLEEP_ID);
        }
}

/**
 * @timeout: The new timeout we want to set, in seconds
 **/
static gboolean
idle_set_timeout_dim (GsdPowerManager *manager, guint timeout)
{
        guint idle_time;

        idle_time = gpm_idletime_get_time (manager->priv->idletime) / 1000;
        if (idle_time == 0)
                return FALSE;

        g_debug ("Setting dim idle timeout: %ds", timeout);
        if (timeout > 0) {
                gpm_idletime_alarm_set (manager->priv->idletime,
                                        GSD_POWER_IDLETIME_DIM_ID,
                                        idle_adjust_timeout (idle_time, timeout) * 1000);
        } else {
                gpm_idletime_alarm_remove (manager->priv->idletime,
                                           GSD_POWER_IDLETIME_DIM_ID);
        }
        return TRUE;
}

static void
refresh_idle_dim_settings (GsdPowerManager *manager)
{
        gint timeout_dim;
        timeout_dim = g_settings_get_int (manager->priv->settings,
                                          "idle-dim-time");
        g_debug ("idle dim set with timeout %i", timeout_dim);
        idle_set_timeout_dim (manager, timeout_dim);
}

static void
gsd_power_manager_class_init (GsdPowerManagerClass *klass)
{
        GObjectClass   *object_class = G_OBJECT_CLASS (klass);

        object_class->finalize = gsd_power_manager_finalize;

        g_type_class_add_private (klass, sizeof (GsdPowerManagerPrivate));
}

static void
sleep_cb_screensaver_proxy_ready_cb (GObject *source_object,
                            GAsyncResult *res,
                            gpointer user_data)
{
        GError *error = NULL;
        GsdPowerManager *manager = GSD_POWER_MANAGER (user_data);

        manager->priv->screensaver_proxy = g_dbus_proxy_new_for_bus_finish (res, &error);
        if (manager->priv->screensaver_proxy == NULL) {
                g_warning ("Could not connect to gnome-screensaver: %s",
                           error->message);
                g_error_free (error);
                return;
        }

        /* Finish the upower_notify_sleep_cb() call by locking the screen */
        g_debug ("gnome-screensaver activated, doing gnome-screensaver lock");
        g_dbus_proxy_call (manager->priv->screensaver_proxy,
                           "Lock",
                           NULL, G_DBUS_CALL_FLAGS_NONE, -1,
                           NULL, NULL, NULL);
}

static void
idle_dbus_signal_cb (GDBusProxy *proxy,
                     const gchar *sender_name,
                     const gchar *signal_name,
                     GVariant *parameters,
                     gpointer user_data)
{
        GsdPowerManager *manager = GSD_POWER_MANAGER (user_data);

        if (g_strcmp0 (signal_name, "InhibitorAdded") == 0 ||
            g_strcmp0 (signal_name, "InhibitorRemoved") == 0) {
                g_debug ("Received gnome session inhibitor change");
                idle_configure (manager);
        }
        if (g_strcmp0 (signal_name, "StatusChanged") == 0) {
                guint status;

                g_variant_get (parameters, "(u)", &status);
                g_dbus_proxy_set_cached_property (proxy, "status",
                                                  g_variant_new ("u", status));
                g_debug ("Received gnome session status change");
                idle_configure (manager);
        }
}

static void
session_proxy_ready_cb (GObject *source_object,
                        GAsyncResult *res,
                        gpointer user_data)
{
        GError *error = NULL;
        GsdPowerManager *manager = GSD_POWER_MANAGER (user_data);

        manager->priv->session_proxy = g_dbus_proxy_new_for_bus_finish (res, &error);
        if (manager->priv->session_proxy == NULL) {
                g_warning ("Could not connect to gnome-session: %s",
                           error->message);
                g_error_free (error);
        } else {
                g_signal_connect (manager->priv->session_proxy, "g-signal",
                                  G_CALLBACK (idle_dbus_signal_cb), manager);
        }

        idle_configure (manager);
}

static void
session_presence_proxy_ready_cb (GObject *source_object,
                                 GAsyncResult *res,
                                 gpointer user_data)
{
        GError *error = NULL;
        GsdPowerManager *manager = GSD_POWER_MANAGER (user_data);

        manager->priv->session_presence_proxy = g_dbus_proxy_new_for_bus_finish (res, &error);
        if (manager->priv->session_presence_proxy == NULL) {
                g_warning ("Could not connect to gnome-sesson: %s",
                           error->message);
                g_error_free (error);
                return;
        }
        g_signal_connect (manager->priv->session_presence_proxy, "g-signal",
                          G_CALLBACK (idle_dbus_signal_cb), manager);
}

static void
power_proxy_ready_cb (GObject             *source_object,
                      GAsyncResult        *res,
                      gpointer             user_data)
{
        GError *error = NULL;
        GsdPowerManager *manager = GSD_POWER_MANAGER (user_data);

        manager->priv->upower_proxy = g_dbus_proxy_new_for_bus_finish (res, &error);
        if (manager->priv->upower_proxy == NULL) {
                g_warning ("Could not connect to UPower: %s",
                           error->message);
                g_error_free (error);
        }
}

static void
power_keyboard_proxy_ready_cb (GObject             *source_object,
                               GAsyncResult        *res,
                               gpointer             user_data)
{
        GVariant *k_now = NULL;
        GVariant *k_max = NULL;
        GError *error = NULL;
        GsdPowerManager *manager = GSD_POWER_MANAGER (user_data);

        manager->priv->upower_kdb_proxy = g_dbus_proxy_new_for_bus_finish (res, &error);
        if (manager->priv->upower_kdb_proxy == NULL) {
                g_warning ("Could not connect to UPower: %s",
                           error->message);
                g_error_free (error);
                goto out;
        }

        k_now = g_dbus_proxy_call_sync (manager->priv->upower_kdb_proxy,
                                        "GetBrightness",
                                        NULL,
                                        G_DBUS_CALL_FLAGS_NONE,
                                        -1,
                                        NULL,
                                        &error);
        if (k_now == NULL) {
                if (error->domain != G_DBUS_ERROR ||
                    error->code != G_DBUS_ERROR_UNKNOWN_METHOD) {
                        g_warning ("Failed to get brightness: %s",
                                   error->message);
                }
                g_error_free (error);
                goto out;
        }

        k_max = g_dbus_proxy_call_sync (manager->priv->upower_kdb_proxy,
                                        "GetMaxBrightness",
                                        NULL,
                                        G_DBUS_CALL_FLAGS_NONE,
                                        -1,
                                        NULL,
                                        &error);
        if (k_max == NULL) {
                g_warning ("Failed to get max brightness: %s", error->message);
                g_error_free (error);
                goto out;
        }

        g_variant_get (k_now, "(i)", &manager->priv->kbd_brightness_now);
        g_variant_get (k_max, "(i)", &manager->priv->kbd_brightness_max);

        /* set brightness to max if not currently set so is something
         * sensible */
        if (manager->priv->kbd_brightness_now <= 0) {
                gboolean ret;
                ret = upower_kbd_set_brightness (manager,
                                                 manager->priv->kbd_brightness_max,
                                                 &error);
                if (!ret) {
                        g_warning ("failed to initialize kbd backlight to %i: %s",
                                   manager->priv->kbd_brightness_max,
                                   error->message);
                        g_error_free (error);
                }
        }
out:
        if (k_now != NULL)
                g_variant_unref (k_now);
        if (k_max != NULL)
                g_variant_unref (k_max);
}

static void
lock_screensaver (GsdPowerManager *manager)
{
        gboolean do_lock;

        do_lock = g_settings_get_boolean (manager->priv->settings_screensaver,
                                          "lock-enabled");
        if (!do_lock)
                return;

        if (manager->priv->screensaver_proxy != NULL) {
                g_debug ("doing gnome-screensaver lock");
                g_dbus_proxy_call (manager->priv->screensaver_proxy,
                                   "Lock",
                                   NULL, G_DBUS_CALL_FLAGS_NONE, -1,
                                   NULL, NULL, NULL);
        } else {
                /* connect to the screensaver first */
                g_dbus_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                                          G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                          NULL,
                                          GS_DBUS_NAME,
                                          GS_DBUS_PATH,
                                          GS_DBUS_INTERFACE,
                                          NULL,
                                          sleep_cb_screensaver_proxy_ready_cb,
                                          manager);
        }
}

static void
upower_notify_sleep_cb (UpClient *client,
                        UpSleepKind sleep_kind,
                        GsdPowerManager *manager)
{
        lock_screensaver (manager);
}

static void
upower_notify_resume_cb (UpClient *client,
                         UpSleepKind sleep_kind,
                         GsdPowerManager *manager)
{
        gboolean ret;
        GError *error = NULL;

        /* this displays the unlock dialogue so the user doesn't have
         * to move the mouse or press any key before the window comes up */
        if (manager->priv->screensaver_proxy != NULL) {
                g_dbus_proxy_call (manager->priv->screensaver_proxy,
                                   "SimulateUserActivity",
                                   NULL,
                                   G_DBUS_CALL_FLAGS_NONE,
                                   -1, NULL, NULL, NULL);
        }

        /* close existing notifications on resume, the system power
         * state is probably different now */
        notify_close_if_showing (manager->priv->notification_low);
        notify_close_if_showing (manager->priv->notification_discharging);

        /* ensure we turn the panel back on after resume */
        ret = gnome_rr_screen_set_dpms_mode (manager->priv->x11_screen,
                                             GNOME_RR_DPMS_ON,
                                             &error);
        if (!ret) {
                g_warning ("failed to turn the panel on after resume: %s",
                           error->message);
                g_error_free (error);
        }
}

static void
idle_send_to_sleep (GsdPowerManager *manager)
{
        gboolean is_inhibited;
        gboolean is_idle;

        /* check the session is not now inhibited */
        is_inhibited = idle_is_session_inhibited (manager,
                                                  SESSION_INHIBIT_MASK_SUSPEND);
        if (is_inhibited) {
                g_debug ("suspend inhibited");
                return;
        }

        /* check the session is really idle*/
        is_idle = idle_is_session_idle (manager);
        if (!is_idle) {
                g_debug ("session is not idle, cannot SLEEP");
                return;
        }

        /* send to sleep, and cancel timeout */
        g_debug ("sending to SLEEP");
        idle_set_mode (manager, GSD_POWER_IDLE_MODE_SLEEP);
}

static void
idle_idletime_alarm_expired_cb (GpmIdletime *idletime,
                                guint alarm_id,
                                GsdPowerManager *manager)
{
        g_debug ("idletime alarm: %i", alarm_id);

        switch (alarm_id) {
        case GSD_POWER_IDLETIME_DIM_ID:
                idle_set_mode (manager, GSD_POWER_IDLE_MODE_DIM);
                break;
        case GSD_POWER_IDLETIME_BLANK_ID:
                idle_set_mode (manager, GSD_POWER_IDLE_MODE_BLANK);
                break;
        case GSD_POWER_IDLETIME_SLEEP_ID:
                idle_send_to_sleep (manager);
                break;
        }
}

static void
idle_idletime_reset_cb (GpmIdletime *idletime,
                        GsdPowerManager *manager)
{
        g_debug ("idletime reset");

        idle_set_mode (manager, GSD_POWER_IDLE_MODE_NORMAL);
}

static void
engine_settings_key_changed_cb (GSettings *settings,
                                const gchar *key,
                                GsdPowerManager *manager)
{
        if (g_strcmp0 (key, "use-time-for-policy") == 0) {
                manager->priv->use_time_primary = g_settings_get_boolean (settings, key);
                return;
        }
        if (g_strcmp0 (key, "idle-dim-time") == 0) {
                refresh_idle_dim_settings (manager);
                return;
        }
        if (g_str_has_prefix (key, "sleep-inactive") ||
            g_str_has_prefix (key, "sleep-display")) {
                idle_configure (manager);
                return;
        }
}

static void
engine_session_active_changed_cb (GnomeSettingsSession *session,
                                  GParamSpec *pspec,
                                  GsdPowerManager *manager)
{
        /* when doing the fast-user-switch into a new account,
         * ensure the new account is undimmed and with the backlight on */
        idle_set_mode (manager, GSD_POWER_IDLE_MODE_NORMAL);
}

/* This timer goes off every few minutes, whether the user is idle or not,
   to try and clean up anything that has gone wrong.

   It calls disable_builtin_screensaver() so that if xset has been used,
   or some other program (like xlock) has messed with the XSetScreenSaver()
   settings, they will be set back to sensible values (if a server extension
   is in use, messing with xlock can cause the screensaver to never get a wakeup
   event, and could cause monitor power-saving to occur, and all manner of
   heinousness.)

   This code was originally part of gnome-screensaver, see
   http://git.gnome.org/browse/gnome-screensaver/tree/src/gs-watcher-x11.c?id=fec00b12ec46c86334cfd36b37771cc4632f0d4d#n530
 */
static gboolean
disable_builtin_screensaver (gpointer unused)
{
        int current_server_timeout, current_server_interval;
        int current_prefer_blank,   current_allow_exp;
        int desired_server_timeout, desired_server_interval;
        int desired_prefer_blank,   desired_allow_exp;

        XGetScreenSaver (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()),
                         &current_server_timeout,
                         &current_server_interval,
                         &current_prefer_blank,
                         &current_allow_exp);

        desired_server_timeout  = current_server_timeout;
        desired_server_interval = current_server_interval;
        desired_prefer_blank    = current_prefer_blank;
        desired_allow_exp       = current_allow_exp;

        desired_server_interval = 0;

        /* I suspect (but am not sure) that DontAllowExposures might have
           something to do with powering off the monitor as well, at least
           on some systems that don't support XDPMS?  Who know... */
        desired_allow_exp = AllowExposures;

        /* When we're not using an extension, set the server-side timeout to 0,
           so that the server never gets involved with screen blanking, and we
           do it all ourselves.  (However, when we *are* using an extension,
           we tell the server when to notify us, and rather than blanking the
           screen, the server will send us an X event telling us to blank.)
        */
        desired_server_timeout = 0;

        if (desired_server_timeout     != current_server_timeout
            || desired_server_interval != current_server_interval
            || desired_prefer_blank    != current_prefer_blank
            || desired_allow_exp       != current_allow_exp) {

                g_debug ("disabling server builtin screensaver:"
                         " (xset s %d %d; xset s %s; xset s %s)",
                         desired_server_timeout,
                         desired_server_interval,
                         (desired_prefer_blank ? "blank" : "noblank"),
                         (desired_allow_exp ? "expose" : "noexpose"));

                XSetScreenSaver (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()),
                                 desired_server_timeout,
                                 desired_server_interval,
                                 desired_prefer_blank,
                                 desired_allow_exp);

                XSync (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()), FALSE);
        }

        return TRUE;
}

static gboolean
is_hardware_a_virtual_machine (void)
{
        const gchar *str;
        gboolean ret = FALSE;
        GError *error = NULL;
        GVariant *inner;
        GVariant *variant = NULL;
        GDBusConnection *connection;

        connection = g_bus_get_sync (G_BUS_TYPE_SYSTEM,
                                     NULL,
                                     &error);
        if (connection == NULL) {
                g_warning ("system bus not available: %s", error->message);
                g_error_free (error);
                goto out;
        }
        variant = g_dbus_connection_call_sync (connection,
                                               "org.freedesktop.systemd1",
                                               "/org/freedesktop/systemd1",
                                               "org.freedesktop.DBus.Properties",
                                               "Get",
                                               g_variant_new ("(ss)",
                                                              "org.freedesktop.systemd1.Manager",
                                                              "Virtualization"),
                                               NULL,
                                               G_DBUS_CALL_FLAGS_NONE,
                                               -1,
                                               NULL,
                                               &error);
        if (variant == NULL) {
                g_debug ("Failed to get property '%s': %s", "Virtualization", error->message);
                g_error_free (error);
                goto out;
        }

        /* on bare-metal hardware this is the empty string,
         * otherwise an identifier such as "kvm", "vmware", etc. */
        g_variant_get (variant, "(v)", &inner);
        str = g_variant_get_string (inner, NULL);
        if (str != NULL && str[0] != '\0')
                ret = TRUE;
out:
        if (connection != NULL)
                g_object_unref (connection);
        if (variant != NULL)
                g_variant_unref (variant);
        return ret;
}

gboolean
gsd_power_manager_start (GsdPowerManager *manager,
                         GError **error)
{
        gboolean ret;

        g_debug ("Starting power manager");
        gnome_settings_profile_start (NULL);

        /* coldplug the list of screens */
        manager->priv->x11_screen = gnome_rr_screen_new (gdk_screen_get_default (), error);
        if (manager->priv->x11_screen == NULL)
                return FALSE;

        /* track the active session */
        manager->priv->session = gnome_settings_session_new ();
        g_signal_connect (manager->priv->session, "notify::state",
                          G_CALLBACK (engine_session_active_changed_cb),
                          manager);

        manager->priv->kbd_brightness_old = -1;
        manager->priv->kbd_brightness_pre_dim = -1;
        manager->priv->pre_dim_brightness = -1;
        manager->priv->settings = g_settings_new (GSD_POWER_SETTINGS_SCHEMA);
        g_signal_connect (manager->priv->settings, "changed",
                          G_CALLBACK (engine_settings_key_changed_cb), manager);
        manager->priv->settings_screensaver = g_settings_new ("org.gnome.desktop.screensaver");
        manager->priv->up_client = up_client_new ();
        g_signal_connect (manager->priv->up_client, "notify-sleep",
                          G_CALLBACK (upower_notify_sleep_cb), manager);
        g_signal_connect (manager->priv->up_client, "notify-resume",
                          G_CALLBACK (upower_notify_resume_cb), manager);
        manager->priv->lid_is_closed = up_client_get_lid_is_closed (manager->priv->up_client);
        g_signal_connect (manager->priv->up_client, "device-added",
                          G_CALLBACK (engine_device_added_cb), manager);
        g_signal_connect (manager->priv->up_client, "device-removed",
                          G_CALLBACK (engine_device_removed_cb), manager);
        g_signal_connect (manager->priv->up_client, "device-changed",
                          G_CALLBACK (engine_device_changed_cb), manager);
        g_signal_connect_after (manager->priv->up_client, "changed",
                                G_CALLBACK (up_client_changed_cb), manager);

        /* use the fallback name from gnome-power-manager so the shell
         * blocks this, and uses the power extension instead */
        manager->priv->status_icon = gtk_status_icon_new ();
        gtk_status_icon_set_name (manager->priv->status_icon,
                                  "gnome-power-manager");
        /* TRANSLATORS: this is the title of the power manager status icon
         * that is only shown in fallback mode */
        gtk_status_icon_set_title (manager->priv->status_icon, _("Power Manager"));

        /* connect to UPower for async power operations */
        g_dbus_proxy_new_for_bus (G_BUS_TYPE_SYSTEM,
                                  G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                  NULL,
                                  UPOWER_DBUS_NAME,
                                  UPOWER_DBUS_PATH,
                                  UPOWER_DBUS_INTERFACE,
                                  NULL,
                                  power_proxy_ready_cb,
                                  manager);

        /* connect to UPower for keyboard backlight control */
        g_dbus_proxy_new_for_bus (G_BUS_TYPE_SYSTEM,
                                  G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                  NULL,
                                  UPOWER_DBUS_NAME,
                                  UPOWER_DBUS_PATH_KBDBACKLIGHT,
                                  UPOWER_DBUS_INTERFACE_KBDBACKLIGHT,
                                  NULL,
                                  power_keyboard_proxy_ready_cb,
                                  manager);

        /* connect to the session */
        g_dbus_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                                  G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                  NULL,
                                  GNOME_SESSION_DBUS_NAME,
                                  GNOME_SESSION_DBUS_PATH,
                                  GNOME_SESSION_DBUS_INTERFACE,
                                  NULL,
                                  session_proxy_ready_cb,
                                  manager);
        g_dbus_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                                  0,
                                  NULL,
                                  GNOME_SESSION_DBUS_NAME,
                                  GNOME_SESSION_DBUS_PATH_PRESENCE,
                                  GNOME_SESSION_DBUS_INTERFACE_PRESENCE,
                                  NULL,
                                  session_presence_proxy_ready_cb,
                                  manager);

        manager->priv->devices_array = g_ptr_array_new_with_free_func (g_object_unref);
        manager->priv->canberra_context = ca_gtk_context_get_for_screen (gdk_screen_get_default ());

        manager->priv->phone = gpm_phone_new ();
        g_signal_connect (manager->priv->phone, "device-added",
                          G_CALLBACK (phone_device_added_cb), manager);
        g_signal_connect (manager->priv->phone, "device-removed",
                          G_CALLBACK (phone_device_removed_cb), manager);
        g_signal_connect (manager->priv->phone, "device-refresh",
                          G_CALLBACK (phone_device_refresh_cb), manager);

        /* create a fake virtual composite battery */
        manager->priv->device_composite = up_device_new ();
        g_object_set (manager->priv->device_composite,
                      "kind", UP_DEVICE_KIND_BATTERY,
                      "is-rechargeable", TRUE,
                      "native-path", "dummy:composite_battery",
                      "power-supply", TRUE,
                      "is-present", TRUE,
                      NULL);

        /* get percentage policy */
        manager->priv->low_percentage = g_settings_get_int (manager->priv->settings,
                                                            "percentage-low");
        manager->priv->critical_percentage = g_settings_get_int (manager->priv->settings,
                                                                 "percentage-critical");
        manager->priv->action_percentage = g_settings_get_int (manager->priv->settings,
                                                               "percentage-action");

        /* get time policy */
        manager->priv->low_time = g_settings_get_int (manager->priv->settings,
                                                      "time-low");
        manager->priv->critical_time = g_settings_get_int (manager->priv->settings,
                                                           "time-critical");
        manager->priv->action_time = g_settings_get_int (manager->priv->settings,
                                                         "time-action");

        /* we can disable this if the time remaining is inaccurate or just plain wrong */
        manager->priv->use_time_primary = g_settings_get_boolean (manager->priv->settings,
                                                                  "use-time-for-policy");

        /* create IDLETIME watcher */
        manager->priv->idletime = gpm_idletime_new ();
        g_signal_connect (manager->priv->idletime, "reset",
                          G_CALLBACK (idle_idletime_reset_cb), manager);
        g_signal_connect (manager->priv->idletime, "alarm-expired",
                          G_CALLBACK (idle_idletime_alarm_expired_cb), manager);

        /* ensure the default dpms timeouts are cleared */
        ret = gnome_rr_screen_set_dpms_mode (manager->priv->x11_screen,
                                             GNOME_RR_DPMS_ON,
                                             error);
        if (!ret) {
                g_warning ("Failed set DPMS mode: %s", (*error)->message);
                g_clear_error (error);
        }

        /* coldplug the engine */
        engine_coldplug (manager);

        /* set the initial dim time that can adapt for the user */
        refresh_idle_dim_settings (manager);

        manager->priv->xscreensaver_watchdog_timer_id = g_timeout_add_seconds (XSCREENSAVER_WATCHDOG_TIMEOUT,
                                                                               disable_builtin_screensaver,
                                                                               NULL);
        /* don't blank inside a VM */
        manager->priv->is_virtual_machine = is_hardware_a_virtual_machine ();

        gnome_settings_profile_end (NULL);
        return TRUE;
}

void
gsd_power_manager_stop (GsdPowerManager *manager)
{
        g_debug ("Stopping power manager");

        if (manager->priv->bus_cancellable != NULL) {
                g_cancellable_cancel (manager->priv->bus_cancellable);
                g_object_unref (manager->priv->bus_cancellable);
                manager->priv->bus_cancellable = NULL;
        }

        if (manager->priv->introspection_data) {
                g_dbus_node_info_unref (manager->priv->introspection_data);
                manager->priv->introspection_data = NULL;
        }

        kill_lid_close_safety_timer (manager);

        g_signal_handlers_disconnect_by_data (manager->priv->up_client, manager);

        g_clear_object (&manager->priv->connection);
        g_clear_object (&manager->priv->session);
        g_clear_object (&manager->priv->settings);
        g_clear_object (&manager->priv->settings_screensaver);
        g_clear_object (&manager->priv->up_client);
        g_clear_object (&manager->priv->x11_screen);

        g_ptr_array_unref (manager->priv->devices_array);
        manager->priv->devices_array = NULL;
        g_clear_object (&manager->priv->phone);
        g_clear_object (&manager->priv->device_composite);
        g_clear_object (&manager->priv->previous_icon);

        g_free (manager->priv->previous_summary);
        manager->priv->previous_summary = NULL;

        g_clear_object (&manager->priv->upower_proxy);
        g_clear_object (&manager->priv->session_proxy);
        g_clear_object (&manager->priv->session_presence_proxy);

        if (manager->priv->critical_alert_timeout_id > 0) {
                g_source_remove (manager->priv->critical_alert_timeout_id);
                manager->priv->critical_alert_timeout_id = 0;
        }

        g_clear_object (&manager->priv->idletime);
        g_clear_object (&manager->priv->status_icon);

        if (manager->priv->xscreensaver_watchdog_timer_id > 0) {
                g_source_remove (manager->priv->xscreensaver_watchdog_timer_id);
                manager->priv->xscreensaver_watchdog_timer_id = 0;
        }
}

static void
gsd_power_manager_init (GsdPowerManager *manager)
{
        manager->priv = GSD_POWER_MANAGER_GET_PRIVATE (manager);
}

static void
gsd_power_manager_finalize (GObject *object)
{
        GsdPowerManager *manager;

        manager = GSD_POWER_MANAGER (object);

        g_return_if_fail (manager->priv != NULL);


        G_OBJECT_CLASS (gsd_power_manager_parent_class)->finalize (object);
}

/* returns new level */
static void
handle_method_call_keyboard (GsdPowerManager *manager,
                             const gchar *method_name,
                             GVariant *parameters,
                             GDBusMethodInvocation *invocation)
{
        gint step;
        gint value = -1;
        gboolean ret;
        guint percentage;
        GError *error = NULL;

        if (g_strcmp0 (method_name, "StepUp") == 0) {
                g_debug ("keyboard step up");
                step = BRIGHTNESS_STEP_AMOUNT (manager->priv->kbd_brightness_max);
                value = MIN (manager->priv->kbd_brightness_now + step,
                             manager->priv->kbd_brightness_max);
                ret = upower_kbd_set_brightness (manager, value, &error);

        } else if (g_strcmp0 (method_name, "StepDown") == 0) {
                g_debug ("keyboard step down");
                step = BRIGHTNESS_STEP_AMOUNT (manager->priv->kbd_brightness_max);
                value = MAX (manager->priv->kbd_brightness_now - step, 0);
                ret = upower_kbd_set_brightness (manager, value, &error);

        } else if (g_strcmp0 (method_name, "Toggle") == 0) {
                ret = upower_kbd_toggle (manager, &error);
        } else {
                g_assert_not_reached ();
        }

        /* return value */
        if (!ret) {
                g_dbus_method_invocation_return_gerror (invocation,
                                                        error);
                g_error_free (error);
        } else {
                percentage = ABS_TO_PERCENTAGE (0,
                                                manager->priv->kbd_brightness_max,
                                                value);
                g_dbus_method_invocation_return_value (invocation,
                                                       g_variant_new ("(u)",
                                                                      percentage));
        }
}

static void
handle_method_call_screen (GsdPowerManager *manager,
                           const gchar *method_name,
                           GVariant *parameters,
                           GDBusMethodInvocation *invocation)
{
        gboolean ret = FALSE;
        gint value = -1;
        guint value_tmp;
        GError *error = NULL;

        if (g_strcmp0 (method_name, "GetPercentage") == 0) {
                g_debug ("screen get percentage");
                value = backlight_get_percentage (manager, &error);

        } else if (g_strcmp0 (method_name, "SetPercentage") == 0) {
                g_debug ("screen set percentage");
                g_variant_get (parameters, "(u)", &value_tmp);
                ret = backlight_set_percentage (manager, value_tmp, TRUE, &error);
                if (ret)
                        value = value_tmp;

        } else if (g_strcmp0 (method_name, "StepUp") == 0) {
                g_debug ("screen step up");
                value = backlight_step_up (manager, &error);
        } else if (g_strcmp0 (method_name, "StepDown") == 0) {
                g_debug ("screen step down");
                value = backlight_step_down (manager, &error);
        } else {
                g_assert_not_reached ();
        }

        /* return value */
        if (value < 0) {
                g_dbus_method_invocation_return_gerror (invocation,
                                                        error);
                g_error_free (error);
        } else {
                g_dbus_method_invocation_return_value (invocation,
                                                       g_variant_new ("(u)",
                                                                      value));
        }
}

static GVariant *
device_to_variant_blob (UpDevice *device)
{
        const gchar *object_path;
        gchar *device_icon;
        gdouble percentage;
        GIcon *icon;
        guint64 time_empty, time_full;
        guint64 time_state = 0;
        GVariant *value;
        UpDeviceKind kind;
        UpDeviceState state;

        icon = gpm_upower_get_device_icon (device, TRUE);
        device_icon = g_icon_to_string (icon);
        g_object_get (device,
                      "kind", &kind,
                      "percentage", &percentage,
                      "state", &state,
                      "time-to-empty", &time_empty,
                      "time-to-full", &time_full,
                      NULL);

        /* only return time for these simple states */
        if (state == UP_DEVICE_STATE_DISCHARGING)
                time_state = time_empty;
        else if (state == UP_DEVICE_STATE_CHARGING)
                time_state = time_full;

        /* get an object path, even for the composite device */
        object_path = up_device_get_object_path (device);
        if (object_path == NULL)
                object_path = GSD_DBUS_PATH;

        /* format complex object */
        value = g_variant_new ("(susdut)",
                               object_path,
                               kind,
                               device_icon,
                               percentage,
                               state,
                               time_state);
        g_free (device_icon);
        g_object_unref (icon);
        return value;
}

static void
handle_method_call_main (GsdPowerManager *manager,
                         const gchar *method_name,
                         GVariant *parameters,
                         GDBusMethodInvocation *invocation)
{
        GPtrArray *array;
        guint i;
        GVariantBuilder *builder;
        GVariant *tuple = NULL;
        GVariant *value = NULL;
        UpDevice *device;

        /* return object */
        if (g_strcmp0 (method_name, "GetPrimaryDevice") == 0) {

                /* get the virtual device */
                device = engine_get_primary_device (manager);
                if (device == NULL) {
                        g_dbus_method_invocation_return_dbus_error (invocation,
                                                                    "org.gnome.SettingsDaemon.Power.Failed",
                                                                    "There is no primary device.");
                        return;
                }

                /* return the value */
                value = device_to_variant_blob (device);
                tuple = g_variant_new_tuple (&value, 1);
                g_dbus_method_invocation_return_value (invocation, tuple);
                g_object_unref (device);
                return;
        }

        /* return array */
        if (g_strcmp0 (method_name, "GetDevices") == 0) {

                /* create builder */
                builder = g_variant_builder_new (G_VARIANT_TYPE("a(susdut)"));

                /* add each tuple to the array */
                array = manager->priv->devices_array;
                for (i=0; i<array->len; i++) {
                        device = g_ptr_array_index (array, i);
                        value = device_to_variant_blob (device);
                        g_variant_builder_add_value (builder, value);
                }

                /* return the value */
                value = g_variant_builder_end (builder);
                tuple = g_variant_new_tuple (&value, 1);
                g_dbus_method_invocation_return_value (invocation, tuple);
                g_variant_builder_unref (builder);
                return;
        }

        g_assert_not_reached ();
}

static void
handle_method_call (GDBusConnection       *connection,
                    const gchar           *sender,
                    const gchar           *object_path,
                    const gchar           *interface_name,
                    const gchar           *method_name,
                    GVariant              *parameters,
                    GDBusMethodInvocation *invocation,
                    gpointer               user_data)
{
        GsdPowerManager *manager = GSD_POWER_MANAGER (user_data);

        /* Check session pointer as a proxy for whether the manager is in the
           start or stop state */
        if (manager->priv->session == NULL) {
                return;
        }

        g_debug ("Calling method '%s.%s' for Power",
                 interface_name, method_name);

        if (g_strcmp0 (interface_name, GSD_POWER_DBUS_INTERFACE) == 0) {
                handle_method_call_main (manager,
                                         method_name,
                                         parameters,
                                         invocation);
        } else if (g_strcmp0 (interface_name, GSD_POWER_DBUS_INTERFACE_SCREEN) == 0) {
                handle_method_call_screen (manager,
                                           method_name,
                                           parameters,
                                           invocation);
        } else if (g_strcmp0 (interface_name, GSD_POWER_DBUS_INTERFACE_KEYBOARD) == 0) {
                handle_method_call_keyboard (manager,
                                             method_name,
                                             parameters,
                                             invocation);
        } else {
                g_warning ("not recognised interface: %s", interface_name);
        }
}

static GVariant *
handle_get_property (GDBusConnection *connection,
                     const gchar *sender,
                     const gchar *object_path,
                     const gchar *interface_name,
                     const gchar *property_name,
                     GError **error, gpointer user_data)
{
        GsdPowerManager *manager = GSD_POWER_MANAGER (user_data);
        GVariant *retval = NULL;

        /* Check session pointer as a proxy for whether the manager is in the
           start or stop state */
        if (manager->priv->session == NULL) {
                return NULL;
        }

        if (g_strcmp0 (property_name, "Icon") == 0) {
                retval = engine_get_icon_property_variant (manager);
        } else if (g_strcmp0 (property_name, "Tooltip") == 0) {
                retval = engine_get_tooltip_property_variant (manager);
        }

        return retval;
}

static const GDBusInterfaceVTable interface_vtable =
{
        handle_method_call,
        handle_get_property,
        NULL, /* SetProperty */
};

static void
on_bus_gotten (GObject             *source_object,
               GAsyncResult        *res,
               GsdPowerManager     *manager)
{
        GDBusConnection *connection;
        GDBusInterfaceInfo **infos;
        GError *error = NULL;
        guint i;

        if (manager->priv->bus_cancellable == NULL ||
            g_cancellable_is_cancelled (manager->priv->bus_cancellable)) {
                g_warning ("Operation has been cancelled, so not retrieving session bus");
                return;
        }

        connection = g_bus_get_finish (res, &error);
        if (connection == NULL) {
                g_warning ("Could not get session bus: %s", error->message);
                g_error_free (error);
                return;
        }
        manager->priv->connection = connection;
        infos = manager->priv->introspection_data->interfaces;
        for (i = 0; infos[i] != NULL; i++) {
                g_dbus_connection_register_object (connection,
                                                   GSD_POWER_DBUS_PATH,
                                                   infos[i],
                                                   &interface_vtable,
                                                   manager,
                                                   NULL,
                                                   NULL);
        }
}

static void
register_manager_dbus (GsdPowerManager *manager)
{
        manager->priv->introspection_data = g_dbus_node_info_new_for_xml (introspection_xml, NULL);
        manager->priv->bus_cancellable = g_cancellable_new ();
        g_assert (manager->priv->introspection_data != NULL);

        g_bus_get (G_BUS_TYPE_SESSION,
                   manager->priv->bus_cancellable,
                   (GAsyncReadyCallback) on_bus_gotten,
                   manager);
}

GsdPowerManager *
gsd_power_manager_new (void)
{
        if (manager_object != NULL) {
                g_object_ref (manager_object);
        } else {
                manager_object = g_object_new (GSD_TYPE_POWER_MANAGER, NULL);
                g_object_add_weak_pointer (manager_object,
                                           (gpointer *) &manager_object);
                register_manager_dbus (manager_object);
        }
        return GSD_POWER_MANAGER (manager_object);
}
