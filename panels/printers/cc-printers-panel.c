/*
 * Copyright (C) 2010 Red Hat, Inc
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
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <config.h>

#include "shell/cc-object-storage.h"

#include "cc-printers-panel.h"
#include "cc-printers-resources.h"
#include "pp-printer.h"

#include <string.h>
#include <glib/gi18n-lib.h>
#include <glib/gstdio.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <gio/gio.h>

#include <polkit/polkit.h>
#include <gdesktop-enums.h>

#include <cups/cups.h>
#include <cups/ppd.h>

#include <math.h>

#include "pp-new-printer-dialog.h"
#include "pp-utils.h"
#include "pp-cups.h"
#include "pp-printer-entry.h"
#include "pp-job.h"
#include "pp-new-printer.h"

#include "cc-permission-infobar.h"
#include "cc-util.h"

#define JOB_DEFAULT_PRIORITY  50
#define RENEW_INTERVAL        500
#define SUBSCRIPTION_DURATION 600

#define CUPS_DBUS_NAME      "org.cups.cupsd.Notifier"
#define CUPS_DBUS_PATH      "/org/cups/cupsd/Notifier"
#define CUPS_DBUS_INTERFACE "org.cups.cupsd.Notifier"

#define CUPS_STATUS_CHECK_INTERVAL 5

#if (CUPS_VERSION_MAJOR > 1) || (CUPS_VERSION_MINOR > 5)
#define HAVE_CUPS_1_6 1

#define OBJ_ATTR_SIZE 1024
#define AVAHI_IF_UNSPEC -1
#define AVAHI_PROTO_INET 0
#define AVAHI_PROTO_INET6 1
#define AVAHI_PROTO_UNSPEC -1
#define SYSTEMD1_OBJ "/org/freedesktop/systemd1"
#define SYSTEMD1_BUS "org.freedesktop.systemd1"
#define SYSTEMD1_MANAGER_IFACE "org.freedesktop.systemd1.Manager"
#define SYSTEMD1_SERVICE_IFACE "org.freedesktop.systemd1.Service"
#define AVAHI_BUS "org.freedesktop.Avahi"
#define AVAHI_SERVER_IFACE "org.freedesktop.Avahi.Server"
#define AVAHI_SERVICE_BROWSER_IFACE "org.freedesktop.Avahi.ServiceBrowser"
#define AVAHI_SERVICE_RESOLVER_IFACE "org.freedesktop.Avahi.ServiceResolver"

#endif

#ifndef HAVE_CUPS_1_6
#define ippGetState(ipp) ipp->state
#define ippGetStatusCode(ipp) ipp->request.status.status_code
#define ippGetString(attr, element, language) attr->values[element].string.text
#endif

// #######################################################################
enum
{
    SYSTEM_OBJECT,
    PRINTER_OBJECT,
    SCANNER_OBJECT,
    PRINTER_QUEUE

} obj_type;

typedef struct
{

	ipp_t *response;
	gchar *buff;
	int buff_size;
        cups_dest_t* service;

} add_attribute_data;

typedef struct 
{
        char                *avahi_service_browser_path;
        guint                avahi_service_browser_subscription_id;
        guint                avahi_service_type_browser_subscription_id;           
        guint                unsubscribe_general_subscription_id;
        guint                done,done_1,done_2,done_3,done_4;
        GDBusConnection     *dbus_connection;
        GCancellable        *avahi_cancellable;
        GList               *system_objects;
        GMainLoop           *loop;
        gpointer             user_data;
        char*                service_type;
} Avahi;

typedef struct
{
        GList                *services;
        gchar                *location;
        gchar                *address;
        gchar                *hostname;
        gchar                *name;
        gchar                *resource_path;
        gchar                *type;
        gchar                *domain;
        gchar                *UUID;
        gchar                *object_type;
        gchar                *admin_url;
        gchar                *uri;
        gchar                *objAttr;
        gint64               printer_type,
                             printer_state;
        gboolean             got_printer_state,
                             got_printer_type;
        int                  port;
        int                  family;
        gpointer             user_data;
} AvahiData;

// #############################################################################

struct _CcPrintersPanel
{
  CcPanel parent_instance;

  GtkBuilder *builder;

  PpCups *cups;
  Avahi  *printer_device_backend;
  cups_dest_t *dests;
  int num_dests;

  GPermission *permission;
  gboolean is_authorized;

  GSettings *lockdown_settings;
  CcPermissionInfobar *permission_infobar;

  PpNewPrinterDialog   *pp_new_printer_dialog;

  GDBusProxy      *cups_proxy;
  GDBusConnection *cups_bus_connection;
  gint             subscription_id;
  guint            subscription_renewal_id;
  guint            cups_status_check_id;
  guint            dbus_subscription_id;
  guint            remove_printer_timeout_id;

  GtkRevealer  *notification;
  PPDList      *all_ppds_list;

  gchar    *new_printer_name;

  gchar    *renamed_printer_name;
  gchar    *old_printer_name;
  gchar    *deleted_printer_name;
  GList    *deleted_printers;
  GObject  *reference;

  GHashTable *printer_entries;
  gboolean    entries_filled;
  GVariant   *action;

  GtkSizeGroup *size_group;
};

CC_PANEL_REGISTER (CcPrintersPanel, cc_printers_panel)

typedef struct
{
  gchar        *printer_name;
  GCancellable *cancellable;
} SetPPDItem;

enum {
  PROP_0,
  PROP_PARAMETERS
};

static void actualize_printers_list (CcPrintersPanel *self);
static void update_sensitivity (gpointer user_data);
static void detach_from_cups_notifier (gpointer data);
static void free_dests (CcPrintersPanel *self);

static void
execute_action (CcPrintersPanel *self,
                GVariant        *action)
{
  PpPrinterEntry         *printer_entry;
  const gchar            *action_name;
  const gchar            *printer_name;
  gint                    count;

  count = g_variant_n_children (action);
  if (count == 2)
    {
      g_autoptr(GVariant) action_variant = NULL;

      g_variant_get_child (action, 0, "v", &action_variant);
      action_name = g_variant_get_string (action_variant, NULL);

      /* authenticate-jobs printer-name */
      if (g_strcmp0 (action_name, "authenticate-jobs") == 0)
        {
          g_autoptr(GVariant) variant = NULL;

          g_variant_get_child (action, 1, "v", &variant);
          printer_name = g_variant_get_string (variant, NULL);

          printer_entry = PP_PRINTER_ENTRY (g_hash_table_lookup (self->printer_entries, printer_name));
          if (printer_entry != NULL)
            pp_printer_entry_authenticate_jobs (printer_entry);
          else
            g_warning ("Could not find printer \"%s\"!", printer_name);
        }
      /* show-jobs printer-name */
      else if (g_strcmp0 (action_name, "show-jobs") == 0)
        {
          g_autoptr(GVariant) variant = NULL;

          g_variant_get_child (action, 1, "v", &variant);
          printer_name = g_variant_get_string (variant, NULL);

          printer_entry = PP_PRINTER_ENTRY (g_hash_table_lookup (self->printer_entries, printer_name));
          if (printer_entry != NULL)
            pp_printer_entry_show_jobs_dialog (printer_entry);
          else
            g_warning ("Could not find printer \"%s\"!", printer_name);
        }
    }
}

static void
cc_printers_panel_get_property (GObject    *object,
                               guint       property_id,
                               GValue     *value,
                               GParamSpec *pspec)
{
  switch (property_id)
    {
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
cc_printers_panel_set_property (GObject      *object,
                                guint         property_id,
                                const GValue *value,
                                GParamSpec   *pspec)
{
  CcPrintersPanel        *self = CC_PRINTERS_PANEL (object);
  GVariant               *parameters;

  switch (property_id)
    {
      case PROP_PARAMETERS:
        parameters = g_value_get_variant (value);
        if (parameters != NULL && g_variant_n_children (parameters) > 0)
          {
            if (self->entries_filled)
              {
                execute_action (CC_PRINTERS_PANEL (object), parameters);
              }
            else
              {
                if (self->action != NULL)
                  g_variant_unref (self->action);
                self->action = g_variant_ref (parameters);
              }
          }
        break;

      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
cc_printers_panel_constructed (GObject *object)
{
  CcPrintersPanel *self = CC_PRINTERS_PANEL (object);
  GtkWidget *widget;
  CcShell *shell;

  G_OBJECT_CLASS (cc_printers_panel_parent_class)->constructed (object);

  shell = cc_panel_get_shell (CC_PANEL (self));

  widget = (GtkWidget*)
    gtk_builder_get_object (self->builder, "search-bar");
  gtk_search_bar_set_key_capture_widget (GTK_SEARCH_BAR (widget),
                                         GTK_WIDGET (shell));
}

static void
printer_removed_cb (GObject      *source_object,
                    GAsyncResult *result,
                    gpointer      user_data)
{
  PpPrinter *printer = PP_PRINTER (source_object);
  g_autoptr(GError) error = NULL;

  pp_printer_delete_finish (printer, result, &error);

  if (user_data != NULL)
    {
      g_autoptr(GObject) reference = G_OBJECT (user_data);

      if (g_object_get_data (reference, "self") != NULL)
        {
          CcPrintersPanel *self = CC_PRINTERS_PANEL (g_object_get_data (reference, "self"));
          GList           *iter;

          for (iter = self->deleted_printers; iter != NULL; iter = iter->next)
            {
              if (g_strcmp0 (iter->data, pp_printer_get_name (printer)) == 0)
                {
                  g_free (iter->data);
                  self->deleted_printers = g_list_delete_link (self->deleted_printers, iter);
                  break;
                }
            }
        }
    }

  if (error != NULL)
    g_warning ("Printer could not be deleted: %s", error->message);
}

static gboolean
unsubscribe_general_subscription_cb (gpointer user_data)
{
        Avahi *printer_device_backend = user_data;

        for(int i = 0; i < 4; i++)
        {
          g_dbus_connection_signal_unsubscribe (printer_device_backend[i].dbus_connection,
                                              printer_device_backend[i].avahi_service_browser_subscription_id);
          printer_device_backend[i].avahi_service_browser_subscription_id = 0;
          printer_device_backend[i].unsubscribe_general_subscription_id = 0;
        }
        return G_SOURCE_REMOVE;
}

static void
cc_printers_panel_dispose (GObject *object)
{
  CcPrintersPanel *self = CC_PRINTERS_PANEL (object);

  detach_from_cups_notifier (CC_PRINTERS_PANEL (object));
  Avahi *printer_device_backend = self->printer_device_backend;
  // unsubscribe_general_subscription_cb (self->printer_device_backend);
    for (int i = 0; i < 4; i++)
  {
    if (printer_device_backend[i].avahi_service_browser_subscription_id > 0)
      {
        g_dbus_connection_signal_unsubscribe (printer_device_backend[i].dbus_connection,
                                              printer_device_backend[i].avahi_service_browser_subscription_id);
        printer_device_backend[i].avahi_service_browser_subscription_id = 0;
      }

    if (printer_device_backend[i].avahi_service_type_browser_subscription_id > 0)
      {
        g_dbus_connection_signal_unsubscribe (printer_device_backend[i].dbus_connection,
                                              printer_device_backend[i].avahi_service_type_browser_subscription_id);
        printer_device_backend[i].avahi_service_type_browser_subscription_id = 0;
      }

    if (printer_device_backend[i].avahi_service_browser_path)
      {
        g_dbus_connection_call (printer_device_backend[i].dbus_connection,
                                AVAHI_BUS,
                                printer_device_backend[i].avahi_service_browser_path,
                                AVAHI_SERVICE_BROWSER_IFACE,
                                "Free",
                                NULL,
                                NULL,
                                G_DBUS_CALL_FLAGS_NONE,
                                -1,
                                NULL,
                                NULL,
                                NULL);
        g_clear_pointer (&printer_device_backend[i].avahi_service_browser_path, g_free);
      }
  }

  if (self->deleted_printer_name != NULL)
    {
      g_autoptr(PpPrinter) printer = pp_printer_new (self->deleted_printer_name);
      pp_printer_delete_async (printer,
                               NULL,
                               printer_removed_cb,
                               NULL);
    }

  // g_clear_object(&self->printer_device_backend);
  // g_free (self->printer_device_backend);
  g_clear_object (&self->cups);
  g_clear_pointer (&self->new_printer_name, g_free);
  g_clear_pointer (&self->renamed_printer_name, g_free);
  g_clear_pointer (&self->old_printer_name, g_free);
  g_clear_object (&self->builder);
  g_clear_object (&self->lockdown_settings);
  g_clear_object (&self->permission);
  g_clear_handle_id (&self->cups_status_check_id, g_source_remove);
  g_clear_handle_id (&self->remove_printer_timeout_id, g_source_remove);
  g_clear_pointer (&self->deleted_printer_name, g_free);
  g_clear_pointer (&self->action, g_variant_unref);
  g_clear_pointer (&self->printer_entries, g_hash_table_destroy);
  g_clear_pointer (&self->all_ppds_list, ppd_list_free);
  free_dests (self);
  g_list_free_full (self->deleted_printers, g_free);
  self->deleted_printers = NULL;
  if (self->reference != NULL)
    g_object_set_data (self->reference, "self", NULL);
  g_clear_object (&self->reference);

  G_OBJECT_CLASS (cc_printers_panel_parent_class)->dispose (object);
}

static const char *
cc_printers_panel_get_help_uri (CcPanel *panel)
{
  return "help:gnome-help/printing";
}

static void
cc_printers_panel_class_init (CcPrintersPanelClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  CcPanelClass *panel_class = CC_PANEL_CLASS (klass);

  object_class->get_property = cc_printers_panel_get_property;
  object_class->set_property = cc_printers_panel_set_property;
  object_class->constructed = cc_printers_panel_constructed;
  object_class->dispose = cc_printers_panel_dispose;

  panel_class->get_help_uri = cc_printers_panel_get_help_uri;

  g_object_class_override_property (object_class, PROP_PARAMETERS, "parameters");
}

static void
on_get_job_attributes_cb (GObject      *source_object,
                          GAsyncResult *res,
                          gpointer      user_data)
{
  CcPrintersPanel        *self = (CcPrintersPanel*) user_data;
  const gchar            *job_originating_user_name;
  const gchar            *job_printer_uri;
  g_autoptr(GVariant)     attributes = NULL;
  g_autoptr(GError)       error = NULL;

  attributes = pp_job_get_attributes_finish (PP_JOB (source_object), res, &error);

  if (attributes != NULL)
    {
      g_autoptr(GVariant) username = NULL;

      if ((username = g_variant_lookup_value (attributes, "job-originating-user-name", G_VARIANT_TYPE ("as"))) != NULL)
        {
          g_autoptr(GVariant) printer_uri = NULL;

          if ((printer_uri = g_variant_lookup_value (attributes, "job-printer-uri", G_VARIANT_TYPE ("as"))) != NULL)
            {
              job_originating_user_name = g_variant_get_string (g_variant_get_child_value (username, 0), NULL);
              job_printer_uri = g_variant_get_string (g_variant_get_child_value (printer_uri, 0), NULL);

              if (job_originating_user_name != NULL && job_printer_uri != NULL &&
                  g_strcmp0 (job_originating_user_name, cupsUser ()) == 0 &&
                  g_strrstr (job_printer_uri, "/") != 0 &&
                  self->dests != NULL)
                {
                  PpPrinterEntry *printer_entry;
                  gchar *printer_name;

                  printer_name = g_strrstr (job_printer_uri, "/") + 1;
                  printer_entry = PP_PRINTER_ENTRY (g_hash_table_lookup (self->printer_entries, printer_name));

                  pp_printer_entry_update_jobs_count (printer_entry);
                }
            }
        }
    }
}

static void
on_cups_notification (GDBusConnection *connection,
                      const char      *sender_name,
                      const char      *object_path,
                      const char      *interface_name,
                      const char      *signal_name,
                      GVariant        *parameters,
                      gpointer         user_data)
{
  CcPrintersPanel        *self = (CcPrintersPanel*) user_data;
  gboolean                printer_is_accepting_jobs;
  gchar                  *printer_name = NULL;
  gchar                  *text = NULL;
  gchar                  *printer_uri = NULL;
  gchar                  *printer_state_reasons = NULL;
  gchar                  *job_state_reasons = NULL;
  gchar                  *job_name = NULL;
  guint                   job_id;
  gint                    printer_state;
  gint                    job_state;
  gint                    job_impressions_completed;
  static gchar *requested_attrs[] = {
    "job-printer-uri",
    "job-originating-user-name",
    NULL };

  if (g_strcmp0 (signal_name, "PrinterAdded") != 0 &&
      g_strcmp0 (signal_name, "PrinterDeleted") != 0 &&
      g_strcmp0 (signal_name, "PrinterStateChanged") != 0 &&
      g_strcmp0 (signal_name, "PrinterStopped") != 0 &&
      g_strcmp0 (signal_name, "JobCreated") != 0 &&
      g_strcmp0 (signal_name, "JobCompleted") != 0)
    return;

  if (g_variant_n_children (parameters) == 1)
    g_variant_get (parameters, "(&s)", &text);
 else if (g_variant_n_children (parameters) == 6)
    {
      g_variant_get (parameters, "(&s&s&su&sb)",
                     &text,
                     &printer_uri,
                     &printer_name,
                     &printer_state,
                     &printer_state_reasons,
                     &printer_is_accepting_jobs);
    }
  else if (g_variant_n_children (parameters) == 11)
    {
      g_variant_get (parameters, "(&s&s&su&sbuu&s&su)",
                     &text,
                     &printer_uri,
                     &printer_name,
                     &printer_state,
                     &printer_state_reasons,
                     &printer_is_accepting_jobs,
                     &job_id,
                     &job_state,
                     &job_state_reasons,
                     &job_name,
                     &job_impressions_completed);
    }

  if (g_strcmp0 (signal_name, "PrinterAdded") == 0 ||
      g_strcmp0 (signal_name, "PrinterDeleted") == 0 ||
      g_strcmp0 (signal_name, "PrinterStateChanged") == 0 ||
      g_strcmp0 (signal_name, "PrinterStopped") == 0)
    actualize_printers_list (self);
  else if (g_strcmp0 (signal_name, "JobCreated") == 0 ||
           g_strcmp0 (signal_name, "JobCompleted") == 0)
    {
      g_autoptr(PpJob) job = NULL;

      job = pp_job_new (job_id, NULL, 0, JOB_DEFAULT_PRIORITY, NULL);
      pp_job_get_attributes_async (job,
                                   requested_attrs,
                                   cc_panel_get_cancellable (CC_PANEL (self)),
                                   on_get_job_attributes_cb,
                                   self);
    }
}

static gchar *subscription_events[] = {
  "printer-added",
  "printer-deleted",
  "printer-stopped",
  "printer-state-changed",
  "job-created",
  "job-completed",
  NULL};

static void
renew_subscription_cb (GObject      *source_object,
           GAsyncResult *result,
           gpointer      user_data)
{
  CcPrintersPanel        *self = (CcPrintersPanel*) user_data;
  gint                    subscription_id;

  subscription_id = pp_cups_renew_subscription_finish (PP_CUPS (source_object), result);

  if (subscription_id > 0)
      self->subscription_id = subscription_id;
}

static gboolean
renew_subscription (gpointer data)
{
  CcPrintersPanel        *self = (CcPrintersPanel*) data;

  pp_cups_renew_subscription_async (self->cups,
                                    self->subscription_id,
                                    subscription_events,
                                    SUBSCRIPTION_DURATION,
                                    cc_panel_get_cancellable (CC_PANEL (self)),
                                    renew_subscription_cb,
                                    data);

  return G_SOURCE_CONTINUE;
}

static void
attach_to_cups_notifier_cb (GObject      *source_object,
                            GAsyncResult *result,
                            gpointer      user_data)
{
  CcPrintersPanel        *self = (CcPrintersPanel*) user_data;
  g_autoptr(GError)       error = NULL;
  gint                    subscription_id;

  subscription_id = pp_cups_renew_subscription_finish (PP_CUPS (source_object), result);

  if (subscription_id > 0)
    {
      self->subscription_id = subscription_id;

      self->subscription_renewal_id =
        g_timeout_add_seconds (RENEW_INTERVAL, renew_subscription, self);

      self->cups_proxy = cc_object_storage_create_dbus_proxy_sync (G_BUS_TYPE_SYSTEM,
                                                                   G_DBUS_PROXY_FLAGS_NONE,
                                                                   CUPS_DBUS_NAME,
                                                                   CUPS_DBUS_PATH,
                                                                   CUPS_DBUS_INTERFACE,
                                                                   NULL,
                                                                   &error);

      if (!self->cups_proxy)
        {
          g_warning ("%s", error->message);
          return;
        }

      self->cups_bus_connection = g_dbus_proxy_get_connection (self->cups_proxy);

      self->dbus_subscription_id =
        g_dbus_connection_signal_subscribe (self->cups_bus_connection,
                                            NULL,
                                            CUPS_DBUS_INTERFACE,
                                            NULL,
                                            CUPS_DBUS_PATH,
                                            NULL,
                                            0,
                                            on_cups_notification,
                                            self,
                                            NULL);
    }
}

static void
attach_to_cups_notifier (gpointer data)
{
  CcPrintersPanel        *self = (CcPrintersPanel*) data;

  pp_cups_renew_subscription_async (self->cups,
                                    self->subscription_id,
                                    subscription_events,
                                    SUBSCRIPTION_DURATION,
                                    cc_panel_get_cancellable (CC_PANEL (self)),
                                    attach_to_cups_notifier_cb,
                                    data);
}

static void
subscription_cancel_cb (GObject      *source_object,
                        GAsyncResult *result,
                        gpointer      user_data)
{
  pp_cups_cancel_subscription_finish (PP_CUPS (source_object), result);
}

static void
detach_from_cups_notifier (gpointer data)
{
  CcPrintersPanel        *self = (CcPrintersPanel*) data;

  if (self->dbus_subscription_id != 0) {
    g_dbus_connection_signal_unsubscribe (self->cups_bus_connection,
                                          self->dbus_subscription_id);
    self->dbus_subscription_id = 0;
  }

  pp_cups_cancel_subscription_async (self->cups,
                                     self->subscription_id,
                                     subscription_cancel_cb,
                                     NULL);

  self->subscription_id = 0;

  if (self->subscription_renewal_id != 0) {
    g_source_remove (self->subscription_renewal_id);
    self->subscription_renewal_id = 0;
  }

  g_clear_object (&self->cups_proxy);
}

static void
free_dests (CcPrintersPanel *self)
{
  if (self->num_dests > 0)
    {
      cupsFreeDests (self->num_dests, self->dests);
    }
  self->dests = NULL;
  self->num_dests = 0;
}

static void
on_printer_deletion_undone (CcPrintersPanel *self)
{
  GtkWidget *widget;

  gtk_revealer_set_reveal_child (self->notification, FALSE);

  g_clear_pointer (&self->deleted_printer_name, g_free);

  widget = (GtkWidget*) gtk_builder_get_object (self->builder, "content");
  gtk_list_box_invalidate_filter (GTK_LIST_BOX (widget));

  g_clear_handle_id (&self->remove_printer_timeout_id, g_source_remove);
}

static void
on_notification_dismissed (CcPrintersPanel *self)
{
  g_clear_handle_id (&self->remove_printer_timeout_id, g_source_remove);

  if (self->deleted_printer_name != NULL)
    {
      g_autoptr(PpPrinter) printer = NULL;

      printer = pp_printer_new (self->deleted_printer_name);
      /* The reference tells to the callback whether
         printers panel was already destroyed so
         it knows whether it can access the list
         of deleted printers in it (see below).
       */
      pp_printer_delete_async (printer,
                               NULL,
                               printer_removed_cb,
                               g_object_ref (self->reference));

      /* List of printers which were recently deleted but are still available
         in CUPS due to async nature of the method (e.g. quick deletion
         of several printers).
       */
      self->deleted_printers = g_list_prepend (self->deleted_printers, self->deleted_printer_name);
      self->deleted_printer_name = NULL;
    }

  gtk_revealer_set_reveal_child (self->notification, FALSE);
}

static gboolean
on_remove_printer_timeout (CcPrintersPanel *self)
{
  self->remove_printer_timeout_id = 0;

  on_notification_dismissed (self);

  return G_SOURCE_REMOVE;
}

static void
on_printer_deleted (CcPrintersPanel *self,
                    PpPrinterEntry  *printer_entry)
{
  GtkLabel         *label;
  g_autofree gchar *notification_message = NULL;
  GtkWidget        *widget;

  on_notification_dismissed (self);

  /* Translators: %s is the printer name */
  notification_message = g_strdup_printf (_("Printer “%s” has been deleted"),
                                          pp_printer_entry_get_name (printer_entry));
  label = (GtkLabel*)
    gtk_builder_get_object (self->builder, "notification-label");
  gtk_label_set_label (label, notification_message);

  self->deleted_printer_name = g_strdup (pp_printer_entry_get_name (printer_entry));

  widget = (GtkWidget*) gtk_builder_get_object (self->builder, "content");
  gtk_list_box_invalidate_filter (GTK_LIST_BOX (widget));

  gtk_revealer_set_reveal_child (self->notification, TRUE);

  self->remove_printer_timeout_id = g_timeout_add_seconds (10, G_SOURCE_FUNC (on_remove_printer_timeout), self);
}

static void
on_printer_renamed (CcPrintersPanel *self,
                    gchar           *new_name,
                    PpPrinterEntry  *printer_entry)
{
  self->old_printer_name = g_strdup (pp_printer_entry_get_name (printer_entry));
  self->renamed_printer_name = g_strdup (new_name);
}

static void
on_printer_changed (CcPrintersPanel *self)
{
  actualize_printers_list (self);
}

static void
add_printer_entry (CcPrintersPanel *self,
                   cups_dest_t      printer)
{
  PpPrinterEntry         *printer_entry;
  GtkWidget              *content;
  GSList                 *widgets, *l;

  content = (GtkWidget*) gtk_builder_get_object (self->builder, "content");

  printer_entry = pp_printer_entry_new (printer, self->is_authorized);
  gtk_widget_show (GTK_WIDGET (printer_entry));

  widgets = pp_printer_entry_get_size_group_widgets (printer_entry);
  for (l = widgets; l != NULL; l = l->next)
    gtk_size_group_add_widget (self->size_group, GTK_WIDGET (l->data));
  g_slist_free (widgets);

  g_signal_connect_object (printer_entry,
                           "printer-changed",
                           G_CALLBACK (on_printer_changed),
                           self,
                           G_CONNECT_SWAPPED);
  g_signal_connect_object (printer_entry,
                           "printer-delete",
                           G_CALLBACK (on_printer_deleted),
                           self,
                           G_CONNECT_SWAPPED);
  g_signal_connect_object (printer_entry,
                           "printer-renamed",
                           G_CALLBACK (on_printer_renamed),
                           self,
                           G_CONNECT_SWAPPED);

  gtk_list_box_insert (GTK_LIST_BOX (content), GTK_WIDGET (printer_entry), -1);

  g_hash_table_insert (self->printer_entries, g_strdup (printer.name), printer_entry);
}

static void
set_current_page (GObject      *source_object,
                  GAsyncResult *result,
                  gpointer      user_data)
{
  CcPrintersPanel        *self = (CcPrintersPanel *) user_data;
  GtkWidget              *widget;
  gboolean               success;

  success = pp_cups_connection_test_finish (PP_CUPS (source_object), result, NULL);

  widget = (GtkWidget*) gtk_builder_get_object (self->builder, "main-vbox");
  if (success)
    gtk_stack_set_visible_child_name (GTK_STACK (widget), "empty-state");
  else
    gtk_stack_set_visible_child_name (GTK_STACK (widget), "no-cups-page");

  update_sensitivity (user_data);
}

static gboolean
remove_nonexisting_entry (CcPrintersPanel *self,
                          PpPrinterEntry  *entry)
{
  if (pp_printer_entry_get_web_interface(entry) != NULL) return FALSE;

  gboolean exists = FALSE;
  gint     i;

  for (i = 0; i < self->num_dests; i++)
    {
      if (g_strcmp0 (self->dests[i].name, pp_printer_entry_get_name (entry)) == 0)
        {
          exists = TRUE;
          break;
        }
    }

  if (!exists)
    g_hash_table_remove (self->printer_entries, pp_printer_entry_get_name (entry));

  return !exists;
}

static void 
add_ipp_device_cb (AvahiData*   data,
                   cups_dest_t* dest)
{
  CcPrintersPanel        *self = (CcPrintersPanel*) data->user_data;
  GtkWidget              *widget;
  gpointer                item;

  g_message("%d\n", data->user_data == NULL);
  /*Making the stack visible*/
  widget = (GtkWidget*) gtk_builder_get_object (self->builder, "main-vbox");
  gtk_stack_set_visible_child_name (GTK_STACK (widget), "printers-list");

  item = g_hash_table_lookup (self->printer_entries, dest->name);
     
  if(item == NULL)
    add_printer_entry (self, *dest);
 
  update_sensitivity (data->user_data);
}

static void
actualize_printers_list_cb (GObject      *source_object,
                            GAsyncResult *result,
                            gpointer      user_data)
{
  CcPrintersPanel        *self = (CcPrintersPanel*) user_data;
  GtkWidget              *widget;
  PpCupsDests            *cups_dests;
  GtkWidget              *child;
  gboolean                new_printer_available = FALSE;
  g_autoptr(GError)       error = NULL;
  gpointer                item;
  int                     i;

  cups_dests = pp_cups_get_dests_finish (PP_CUPS (source_object), result, &error);

  if (cups_dests == NULL && error != NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_warning ("Could not get dests: %s", error->message);
        }

      return;
    }

  free_dests (self);
  self->dests = cups_dests->dests;
  self->num_dests = cups_dests->num_of_dests;
  g_free (cups_dests);

  widget = (GtkWidget*) gtk_builder_get_object (self->builder, "main-vbox");
  if (self->num_dests == 0 && !self->new_printer_name)
    pp_cups_connection_test_async (PP_CUPS (source_object), NULL, set_current_page, self);
  else
    gtk_stack_set_visible_child_name (GTK_STACK (widget), "printers-list");

  widget = (GtkWidget*) gtk_builder_get_object (self->builder, "content");
  child = gtk_widget_get_first_child (widget);
  while (child)
    {
      GtkWidget *next = gtk_widget_get_next_sibling (child);

      if (remove_nonexisting_entry (self, PP_PRINTER_ENTRY (child)))
        gtk_list_box_remove (GTK_LIST_BOX (widget), child);

      child = next;
    }

  for (i = 0; i < self->num_dests; i++)
    {
      new_printer_available = g_strcmp0 (self->dests[i].name, self->renamed_printer_name) == 0;
      if (new_printer_available)
        break;
    }

  for (i = 0; i < self->num_dests; i++)
    {
      if (new_printer_available && g_strcmp0 (self->dests[i].name, self->old_printer_name) == 0)
          continue;
      item = g_hash_table_lookup (self->printer_entries, self->dests[i].name);
      if (item != NULL)
        pp_printer_entry_update (PP_PRINTER_ENTRY (item), self->dests[i], self->is_authorized);
      else
        add_printer_entry (self, self->dests[i]);
    }

  if (!self->entries_filled)
    {
      if (self->action != NULL)
        {
          execute_action (self, self->action);
          g_variant_unref (self->action);
          self->action = NULL;
        }

      self->entries_filled = TRUE;
    }
  update_sensitivity (user_data);

  if (self->new_printer_name != NULL)
    {
      GtkScrolledWindow      *scrolled_window;
      GtkAllocation           allocation;
      GtkAdjustment          *adjustment;
      GtkWidget              *printer_entry;

      /* Scroll the view to show the newly added printer-entry. */
      scrolled_window = GTK_SCROLLED_WINDOW (gtk_builder_get_object (self->builder,
                                                                     "scrolled-window"));
      adjustment = gtk_scrolled_window_get_vadjustment (scrolled_window);

      printer_entry = GTK_WIDGET (g_hash_table_lookup (self->printer_entries,
                                                       self->new_printer_name));
      if (printer_entry != NULL)
        {
          gtk_widget_get_allocation (printer_entry, &allocation);
          g_clear_pointer (&self->new_printer_name, g_free);

          gtk_adjustment_set_value (adjustment,
                                    allocation.y - gtk_widget_get_margin_top (printer_entry));
        }
    }
}

static void
actualize_printers_list (CcPrintersPanel *self)
{
  pp_cups_get_dests_async (self->cups,
                           cc_panel_get_cancellable (CC_PANEL (self)),
                           actualize_printers_list_cb,
                           self);
}

static int 
compare_services (gconstpointer      data1,
                  gconstpointer      data2)
{
        AvahiData *data_1 = (AvahiData*)data1;
        AvahiData *data_2 = (AvahiData*)data2;
        
        return g_strcmp0 (data_1->name,data_2->name);
}

static void 
add_option (cups_dest_t* dest,
            gchar*       attr_name,
            gchar*       attr_val)
{
  dest->options[dest->num_options].name = g_strdup (attr_name);
  dest->options[dest->num_options].value = g_strdup (attr_val);
  dest->num_options++;
  
  return;
}

static gboolean
avahi_txt_get_key_value_pair (const gchar  *entry,
                              gchar       **key,
                              gchar       **value)
{
  const gchar *equal_sign;

  *key = NULL;
  *value = NULL;

  if (entry != NULL)
    {
      equal_sign = strstr (entry, "=");

      if (equal_sign != NULL)
        {
          *key = g_strndup (entry, equal_sign - entry);
          *value = g_strdup (equal_sign + 1);

          return TRUE;
        }
    }

  return FALSE;
}

static void
add_device (AvahiData* data)
{
    cups_dest_t* dest = g_new0 (cups_dest_t, 1);
    dest->options = g_new0 (cups_option_t, 20);
    dest->name = g_strdup (data->name);
    add_option (dest, "UUID", data->UUID);
    add_option (dest, "device-uri", data->uri);

    if (data->admin_url != NULL)
      add_option (dest, "printer-more-info", data->admin_url);
    else        
      add_option (dest, "printer-more-info", g_strdup_printf("http://%s:%d", data->hostname, data->port));
		
    add_option (dest, "printer-location", data->location);
    add_option(dest, "hostname", data->hostname);
    add_option(dest, "OBJ_TYPE", "PRINTER_OBJECT");
    // data->services = g_list_append(data->services, dest);
    add_ipp_device_cb(data, dest);
    return;
}

static void
avahi_service_resolver_cb (GObject      *source_object,
                           GAsyncResult *res,
                           gpointer      user_data)

{
        AvahiData               *data;
        Avahi                   *backend;
        const char              *name;
        const char              *hostname;
        const char              *type;
        const char              *domain;
        const char              *address;
        char                    *key;
        char                    *value;
        char                    *tmp;
        char                    *endptr;
        GVariant                *txt,
                                *child,
                                *output;
        guint32                  flags;
        guint16                  port;
        GError                  *error = NULL;
        GList                   *iter;
        gsize                    length; 
        int                      interface;
        int                      protocol;
        int                      aprotocol;
        int                      i;


        backend = user_data;

        output = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source_object),
                                          res,
                                          &error);

        if (output)
        {

                g_variant_get (output, "(ii&s&s&s&si&sq@aayu)",
                               &interface,
                               &protocol,
                               &name,
                               &type,
                               &domain,
                               &hostname,
                               &aprotocol,
                               &address,
                               &port,
                               &txt,
                               &flags);

                data = g_new0 (AvahiData, 1);
                data->user_data = backend->user_data;
              
                if (g_strcmp0 (type, "_ipps-system._tcp") == 0 ||
                    g_strcmp0 (type, "_ipp-system._tcp") == 0)
                  {
                       data->object_type = g_strdup("SYSTEM_OBJECT");
                  } 
                else
                  {
                      data->object_type = g_strdup("PRINTER_OBJECT");
                  }

            for (i = 0; i < g_variant_n_children (txt); i++)
            {
              child = g_variant_get_child_value (txt, i);

              length = g_variant_get_size (child);
              if (length > 0)
                {
                  tmp = g_strndup (g_variant_get_data (child), length);
                  g_variant_unref (child);

                  if (!avahi_txt_get_key_value_pair (tmp, &key, &value))
                    {
                      g_free (tmp);
                      continue;
                    }

                  if (g_strcmp0 (key, "rp") == 0)
                    {
                      data->resource_path = g_strdup (value);
                    }
                  else if (g_strcmp0 (key, "note") == 0)
                    {
                      data->location = g_strdup (value);
                    }
                  else if (g_strcmp0 (key, "printer-type") == 0)
                    {
                      endptr = NULL;
                      data->printer_type = g_ascii_strtoull (value, &endptr, 16);
                      if (data->printer_type != 0 || endptr != value)
                        data->got_printer_type = TRUE;
                    }
                  else if (g_strcmp0 (key, "printer-state") == 0)
                    {
                      endptr = NULL;
                      data->printer_state = g_ascii_strtoull (value, &endptr, 10);
                      if (data->printer_state != 0 || endptr != value)
                        data->got_printer_state = TRUE;
                    }
                  else if (g_strcmp0 (key, "UUID") == 0)
                    {
                      if (*value != '\0')
                        data->UUID = g_strdup (value);
                    }
                  else if (g_strcmp0 (key, "adminurl") == 0)
                    {
                      if (*value != '\0')
                        data->admin_url = g_strdup (value);
                    }
                  g_clear_pointer (&key, g_free);
                  g_clear_pointer (&value, g_free);
                  g_free (tmp);
                }
              else
                {
                  g_variant_unref (child);
                }
            }
                
                data->address = g_strdup (address);
                data->hostname = g_strdup (hostname);
                data->port = port;
                data->family = protocol;
                data->name = g_strdup (name);
                data->type = g_strdup (type);
                data->domain = g_strdup (domain);
                data->services = NULL;

                g_variant_unref (txt);
                g_variant_unref (output);

                iter = g_list_find_custom (backend->system_objects, (gconstpointer) data, (GCompareFunc) compare_services);
                if (iter == NULL)
                  {
                     backend->system_objects = g_list_append (backend->system_objects, data);
                    //  if (g_strcmp0(data->object_type, "SYSTEM_OBJECT") == 0)
                    //   get_services (data);
                    //  else    Check new method for getting device from IPP request
                      add_device (data);

                  }
                else 
                 {
                     g_free (data->location);
                     g_free (data->address);
                     g_free (data->hostname);
                     g_free (data->name);
                     g_free (data->resource_path);
                     g_free (data->type);
                     g_free (data->domain);
                     g_free (data);
                 }
         }   
        else
         {
                if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                  {
                        char *message = g_strdup_printf ("%s", error->message); 
                        // _cph_cups_set_internal_status ( backend->cups, message );
                        g_free (message);
                  }
                g_error_free (error);
         }

        return;
}

static void
avahi_service_browser_signal_handler (GDBusConnection *connection,
                                      const char      *sender_name,
                                      const char      *object_path,
                                      const char      *interface_name,
                                      const char      *signal_name,
                                      GVariant        *parameters,
                                      gpointer         user_data)
{
        Avahi               *backend;
        char                *name;
        char                *type;
        char                *domain;
        guint                flags;
        int                  interface;
        int                  protocol;

        backend = user_data;  

        if (g_strcmp0 (signal_name, "ItemNew") == 0)
          {
            g_variant_get (parameters, "(ii&s&s&su)",
                           &interface,
                           &protocol,
                           &name,
                           &type,
                           &domain,
                           &flags);

              g_dbus_connection_call (backend->dbus_connection,
                        AVAHI_BUS,
                        "/",
                        AVAHI_SERVER_IFACE,
                        "ResolveService",
                        g_variant_new ("(iisssiu)",
                                       interface,
                                       protocol,
                                       name,
                                       type,
                                       domain,
                                       AVAHI_PROTO_UNSPEC,
                                       0),
                        G_VARIANT_TYPE ("(iissssisqaayu)"),
                        G_DBUS_CALL_FLAGS_NONE,
                        -1,
                        backend->avahi_cancellable,
                        avahi_service_resolver_cb,
                        backend);
              
          }
        else if (g_strcmp0 (signal_name, "ItemRemove") == 0)
          {
            g_variant_get (parameters, "(ii&s&s&su)",
                           &interface,
                           &protocol,
                           &name,
                           &type,
                           &domain,
                           &flags);


                 GList *iter = g_list_find_custom (backend->system_objects, name , (GCompareFunc) compare_services);
                if (iter != NULL)
                  {
                    backend->system_objects = g_list_delete_link (backend->system_objects, iter);
                    g_free (iter->data);
                  }

          }
        else if (g_strcmp0 (signal_name, "AllForNow"))
          {
            
          }

   return;
}

static void
avahi_service_browser_new_cb (GObject           *source_object,
                              GAsyncResult      *res,
                              gpointer           user_data)
{
        Avahi               *printer_device_backend;
        GError              *error = NULL;
        GVariant            *output = NULL;

        output = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source_object),
                                          res,
                                          &error);
        printer_device_backend = user_data;

        if (output)
          {

            g_variant_get (output, "(o)", &printer_device_backend->avahi_service_browser_path);
            printer_device_backend->avahi_service_type_browser_subscription_id =
              g_dbus_connection_signal_subscribe (printer_device_backend->dbus_connection,
                                                  NULL,
                                                  AVAHI_SERVICE_BROWSER_IFACE,
                                                  NULL,
                                                  printer_device_backend->avahi_service_browser_path,
                                                  NULL,
                                                  G_DBUS_SIGNAL_FLAGS_NONE,
                                                  avahi_service_browser_signal_handler,
                                                  printer_device_backend,
                                                  NULL);

            if (printer_device_backend->avahi_service_browser_path &&
                printer_device_backend->avahi_service_type_browser_subscription_id > 0)
              {
                g_dbus_connection_signal_unsubscribe (printer_device_backend->dbus_connection,
                                                      printer_device_backend->avahi_service_browser_subscription_id);
                printer_device_backend->avahi_service_browser_subscription_id = 0;
              }

            g_variant_unref (output);
          }
        else
          {
            /*
             * The creation of ServiceBrowser fails with G_IO_ERROR_DBUS_ERROR
             * if Avahi is disabled.
             */
            if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_DBUS_ERROR) &&
                !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                //  _cph_cups_set_internal_status(printer_device_backend->cups, g_strdup_printf("%s", error->message));
            g_error_free (error);
          }
        
}

static void
cups_get_ipp_devices_cb (GObject     *source_object,
                        GAsyncResult *result,
                        gpointer      user_data)
{ 
        Avahi*                  printer_device_backend;  
        g_autoptr(GError)       error = NULL;
        pp_cups_get_dests_finish (source_object, result, &error);

        CcPrintersPanel  *self = (CcPrintersPanel*) user_data;
        printer_device_backend = self -> printer_device_backend; 
        for(int i = 0; i < 4; i++)
        {/*
         * We need to subscribe to signals of service browser before
         * we actually create it because it starts to emit them right
         * after its creation.
         */
        printer_device_backend[i].user_data = user_data;

        printer_device_backend[i].avahi_service_browser_subscription_id =
        g_dbus_connection_signal_subscribe  (printer_device_backend[i].dbus_connection,
                                               NULL,
                                               AVAHI_SERVICE_BROWSER_IFACE,
                                               NULL,
                                               NULL,
                                               NULL,
                                               G_DBUS_SIGNAL_FLAGS_NONE,
                                               avahi_service_browser_signal_handler,
                                               &printer_device_backend[i],
                                               NULL);

        g_dbus_connection_call (printer_device_backend[i].dbus_connection,
                                AVAHI_BUS,
                                "/",
                                AVAHI_SERVER_IFACE,
                                "ServiceBrowserNew",
                                g_variant_new ("(iissu)",
                                               AVAHI_IF_UNSPEC,
                                               AVAHI_PROTO_UNSPEC,
                                               printer_device_backend[i].service_type,
                                               "",
                                               0),
                                G_VARIANT_TYPE ("(o)"),
                                G_DBUS_CALL_FLAGS_NONE,
                                -1,
                                printer_device_backend[i].avahi_cancellable,
                                avahi_service_browser_new_cb,
                                &printer_device_backend[i]);
        }
        return;
}

static gboolean
comp_entries (gconstpointer a,
              gconstpointer b)
{
    char *entry_1 = (char*)a;
    char *entry_2 = (char*)b;

    return !g_strcmp0 (entry_1, entry_2);
}

static void
add_interface_data (cups_dest_t* dest,
                    cups_dest_t* src)
{
  src->instance = dest->instance;
  src->is_default = dest->is_default;

    for (int i = 0; i < dest->num_options; i++)
      add_option (src, dest->options[i].name, dest->options[i].value);

    // add_option (src, "sanitize-name", "TRUE");
    
    *dest = *src;
    
    return;
}


static void 
actualize_ipp_device_list (CcPrintersPanel *self)
{ 
  pp_cups_get_new_dests_async (self->cups,
                           cc_panel_get_cancellable (CC_PANEL (self)),
                           cups_get_ipp_devices_cb,
                           self);
}

static void
printer_add_async_cb (GObject      *source_object,
                      GAsyncResult *res,
                      gpointer      user_data)
{
  CcPrintersPanel  *self = (CcPrintersPanel*) user_data;
  gboolean          success;
  g_autoptr(GError) error = NULL;

  success = pp_new_printer_add_finish (PP_NEW_PRINTER (source_object), res, &error);

  if (!success)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_warning ("%s", error->message);

          GtkWidget *message_dialog;

          message_dialog = gtk_message_dialog_new (NULL,
                                                   0,
                                                   GTK_MESSAGE_ERROR,
                                                   GTK_BUTTONS_CLOSE,
          /* Translators: Addition of the new printer failed. */
                                                   _("Failed to add new printer."));
          g_signal_connect (message_dialog,
                            "response",
                            G_CALLBACK (gtk_window_destroy),
                            NULL);
          gtk_widget_show (message_dialog);
        }
    }

  actualize_printers_list (self);
}

static void
new_printer_dialog_response_cb (GtkDialog *_dialog,
                                gint       response_id,
                                gpointer   user_data)
{
  CcPrintersPanel         *self = (CcPrintersPanel*) user_data;
  PpNewPrinterDialog      *pp_new_printer_dialog =  PP_NEW_PRINTER_DIALOG (_dialog);
  g_autoptr(PpNewPrinter)  new_printer = NULL;

  if (response_id == GTK_RESPONSE_OK)
    {
      new_printer = pp_new_printer_dialog_get_new_printer (pp_new_printer_dialog);
      g_object_get(G_OBJECT (new_printer), "name", &self->new_printer_name, NULL);

      actualize_printers_list (self);
      actualize_ipp_device_list (self);

      pp_new_printer_add_async (new_printer,
                                cc_panel_get_cancellable (CC_PANEL (self)),
                                printer_add_async_cb,
                                self);
    }

  gtk_window_destroy (GTK_WINDOW (pp_new_printer_dialog));
  self->pp_new_printer_dialog = NULL;
}

static void
printer_add_cb (CcPrintersPanel *self)
{
  GtkNative *native;

  native = gtk_widget_get_native (GTK_WIDGET (self));
  self->pp_new_printer_dialog = pp_new_printer_dialog_new (self->all_ppds_list,
                                                           new_printer_dialog_response_cb,
                                                           self);

  gtk_window_set_transient_for (GTK_WINDOW (self->pp_new_printer_dialog),
                                            GTK_WINDOW (native));

  gtk_widget_show (GTK_WIDGET (self->pp_new_printer_dialog));
}

static void
update_sensitivity (gpointer user_data)
{
  CcPrintersPanel         *self = (CcPrintersPanel*) user_data;
  const char              *cups_server = NULL;
  GtkWidget               *widget;
  gboolean                 local_server = TRUE;
  gboolean                 no_cups = FALSE;

  self->is_authorized =
    self->permission &&
    g_permission_get_allowed (G_PERMISSION (self->permission)) &&
    self->lockdown_settings &&
    !g_settings_get_boolean (self->lockdown_settings, "disable-print-setup");

  widget = (GtkWidget*) gtk_builder_get_object (self->builder, "main-vbox");
  if (g_strcmp0 (gtk_stack_get_visible_child_name (GTK_STACK (widget)), "no-cups-page") == 0)
    no_cups = TRUE;

  cups_server = cupsServer ();
  if (cups_server &&
      g_ascii_strncasecmp (cups_server, "localhost", 9) != 0 &&
      g_ascii_strncasecmp (cups_server, "127.0.0.1", 9) != 0 &&
      g_ascii_strncasecmp (cups_server, "::1", 3) != 0 &&
      cups_server[0] != '/')
    local_server = FALSE;

  widget = (GtkWidget*) gtk_builder_get_object (self->builder, "search-button");
  gtk_widget_set_visible (widget, !no_cups);

  widget = (GtkWidget*) gtk_builder_get_object (self->builder, "search-bar");
  gtk_widget_set_visible (widget, !no_cups);

  widget = (GtkWidget*) gtk_builder_get_object (self->builder, "printer-add-button");
  gtk_widget_set_sensitive (widget, local_server && self->is_authorized && !no_cups && !self->new_printer_name);

  widget = (GtkWidget*) gtk_builder_get_object (self->builder, "printer-add-button2");
  gtk_widget_set_sensitive (widget, local_server && self->is_authorized && !no_cups && !self->new_printer_name);
}

static void
on_permission_changed (CcPrintersPanel *self)
{
  actualize_printers_list (self);
  actualize_ipp_device_list (self);
  update_sensitivity (self);
}

static void
on_lockdown_settings_changed (CcPrintersPanel *self,
                              const char      *key)
{
  if (g_str_equal (key, "disable-print-setup") == FALSE)
    return;

#if 0
  /* FIXME */
  gtk_widget_set_sensitive (self->lock_button,
    !g_settings_get_boolean (self->lockdown_settings, "disable-print-setup"));
#endif

  on_permission_changed (self);
}

static void
cups_status_check_cb (GObject      *source_object,
                      GAsyncResult *result,
                      gpointer      user_data)
{
  CcPrintersPanel        *self = (CcPrintersPanel*) user_data;
  gboolean                success;

  success = pp_cups_connection_test_finish (PP_CUPS (source_object), result, NULL);
  if (success)
    {
      actualize_printers_list (self);
      attach_to_cups_notifier (self);

      g_source_remove (self->cups_status_check_id);
      self->cups_status_check_id = 0;
    }
}

static gboolean
cups_status_check (gpointer user_data)
{
  CcPrintersPanel         *self = (CcPrintersPanel*) user_data;

  pp_cups_connection_test_async (self->cups, NULL, cups_status_check_cb, self);

  return self->cups_status_check_id != 0;
}

static void
connection_test_cb (GObject      *source_object,
                    GAsyncResult *result,
                    gpointer      user_data)
{
  CcPrintersPanel        *self;
  gboolean                success;
  g_autoptr(GError)       error = NULL;

  success = pp_cups_connection_test_finish (PP_CUPS (source_object), result, &error);

  if (error != NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_warning ("Could not test connection: %s", error->message);
        }

      return;
    }

  self = CC_PRINTERS_PANEL (user_data);

  if (!success)
    {
      self->cups_status_check_id =
        g_timeout_add_seconds (CUPS_STATUS_CHECK_INTERVAL, cups_status_check, self);
    }
}

static void
get_all_ppds_async_cb (PPDList  *ppds,
                       gpointer  user_data)
{
  CcPrintersPanel        *self = (CcPrintersPanel*) user_data;

  self->all_ppds_list = ppd_list_copy (ppds);

  if (self->pp_new_printer_dialog)
    pp_new_printer_dialog_set_ppd_list (self->pp_new_printer_dialog,
                                        self->all_ppds_list);
}

static gint	
sort_function (GtkListBoxRow *row1,	
               GtkListBoxRow *row2,	
               gpointer       user_data)	
{	
  PpPrinterEntry *entry1 = PP_PRINTER_ENTRY (row1);	
  PpPrinterEntry *entry2 = PP_PRINTER_ENTRY (row2);	

  int val;

  if (pp_printer_entry_get_hostname (entry1) != NULL)	
    {	
      if (pp_printer_entry_get_hostname (entry2) != NULL)	
        {
          val = g_ascii_strcasecmp (pp_printer_entry_get_hostname (entry1), pp_printer_entry_get_hostname (entry2)); 
          
          if (val == 0)
           {
              if (pp_printer_entry_get_name (entry1) != NULL)	
               {	
                  if (pp_printer_entry_get_name (entry2) != NULL)	
                    return g_ascii_strcasecmp (pp_printer_entry_get_name (entry1), pp_printer_entry_get_name (entry2));
                  else
                    return 1;
               }
               else 
               {
                   if (pp_printer_entry_get_name (entry2) != NULL)	
                      return -1;	
                    else	
                      return 0;
               }
          }

          return val; 
        }
      else	
        return 1;	
    }	
  else	
    {	
      if (pp_printer_entry_get_hostname (entry2) != NULL)	
        return -1;	
      else	
        return 0;	
    }	
}

static gboolean
filter_function (GtkListBoxRow *row,
                 gpointer       user_data)
{
  CcPrintersPanel        *self = (CcPrintersPanel*) user_data;
  PpPrinterEntry         *entry = PP_PRINTER_ENTRY (row);
  GtkWidget              *search_entry;
  gboolean                retval;
  g_autofree gchar       *search = NULL;
  g_autofree gchar       *name = NULL;
  g_autofree gchar       *location = NULL;
  GList                  *iter;
  const gchar            *search_text;

  search_entry = (GtkWidget*)
    gtk_builder_get_object (self->builder, "search-entry");
  search_text = gtk_editable_get_text (GTK_EDITABLE (search_entry));

  if (g_utf8_strlen (search_text, -1) == 0)
    {
      retval = TRUE;
    }
  else
    {
      name = cc_util_normalize_casefold_and_unaccent (pp_printer_entry_get_name (entry));
      location = cc_util_normalize_casefold_and_unaccent (pp_printer_entry_get_location (entry));

      search = cc_util_normalize_casefold_and_unaccent (search_text);

      retval = strstr (name, search) != NULL;
      if (location != NULL)
          retval = retval || (strstr (location, search) != NULL);
    }

  if (self->deleted_printer_name != NULL &&
      g_strcmp0 (self->deleted_printer_name, pp_printer_entry_get_name (entry)) == 0)
    {
      retval = FALSE;
    }

  if (self->deleted_printers != NULL)
    {
      for (iter = self->deleted_printers; iter != NULL; iter = iter->next)
        {
          if (g_strcmp0 (iter->data, pp_printer_entry_get_name (entry)) == 0)
            {
              retval = FALSE;
              break;
            }
        }
    }

  return retval;
}

static void
cc_printers_panel_init (CcPrintersPanel *self)
{
  GtkWidget              *top_widget;
  GtkWidget              *widget;
  g_autoptr(GError)       error = NULL;
  gchar                  *objects[] = { "overlay", "permission-infobar", "top-right-buttons", "printer-add-button", "search-button", NULL };
  guint                   builder_result;

  g_resources_register (cc_printers_get_resource ());
  /* initialize main data structure */
  self->builder = gtk_builder_new ();
  self->reference = g_object_new (G_TYPE_OBJECT, NULL);

  self->cups = pp_cups_new ();

  self->printer_entries = g_hash_table_new_full (g_str_hash,
                                                 g_str_equal,
                                                 g_free,
                                                 NULL);

  g_type_ensure (CC_TYPE_PERMISSION_INFOBAR);

  g_object_set_data_full (self->reference, "self", self, NULL);

  builder_result = gtk_builder_add_objects_from_resource (self->builder,
                                                          "/org/gnome/control-center/printers/printers.ui",
                                                          objects, &error);

  if (builder_result == 0)
    {
      /* Translators: The XML file containing user interface can not be loaded */
      g_warning (_("Could not load ui: %s"), error->message);
      return;
    }

  self->notification = (GtkRevealer*)
    gtk_builder_get_object (self->builder, "notification");

  widget = (GtkWidget*)
    gtk_builder_get_object (self->builder, "notification-undo-button");
  g_signal_connect_object (widget, "clicked", G_CALLBACK (on_printer_deletion_undone), self, G_CONNECT_SWAPPED);

  widget = (GtkWidget*)
    gtk_builder_get_object (self->builder, "notification-dismiss-button");
  g_signal_connect_object (widget, "clicked", G_CALLBACK (on_notification_dismissed), self, G_CONNECT_SWAPPED);

  self->permission_infobar = (CcPermissionInfobar*)
    gtk_builder_get_object (self->builder, "permission-infobar");

  /* add the top level widget */
  top_widget = (GtkWidget*)
    gtk_builder_get_object (self->builder, "overlay");

  /* connect signals */
  widget = (GtkWidget*)
    gtk_builder_get_object (self->builder, "printer-add-button");
  g_signal_connect_object (widget, "clicked", G_CALLBACK (printer_add_cb), self, G_CONNECT_SWAPPED);
  
  widget = (GtkWidget*) 
        gtk_builder_get_object (self->builder, "printer-add-button2");
  g_signal_connect_object (widget, "clicked", G_CALLBACK (printer_add_cb), self, G_CONNECT_SWAPPED);

  widget = (GtkWidget*)
    gtk_builder_get_object (self->builder, "content");
  gtk_list_box_set_filter_func (GTK_LIST_BOX (widget),
                                filter_function,
                                self,
                                NULL);
  g_signal_connect_swapped (gtk_builder_get_object (self->builder, "search-entry"),
                            "search-changed",
                            G_CALLBACK (gtk_list_box_invalidate_filter),
                            widget);
  
  // gtk_list_box_set_sort_func (GTK_LIST_BOX (widget),	
  //                             sort_function,	
  //                             NULL,	
  //                             NULL);
  
  self->lockdown_settings = g_settings_new ("org.gnome.desktop.lockdown");
  if (self->lockdown_settings)
    g_signal_connect_object (self->lockdown_settings,
                             "changed",
                             G_CALLBACK (on_lockdown_settings_changed),
                             self,
                             G_CONNECT_SWAPPED | G_CONNECT_AFTER);

  /* Add unlock button */
  self->permission = (GPermission *)polkit_permission_new_sync (
    "org.opensuse.cupspkhelper.mechanism.all-edit", NULL, NULL, NULL);
  if (self->permission != NULL)
    {
      g_signal_connect_object (self->permission,
                               "notify",
                               G_CALLBACK (on_permission_changed),
                               self,
                               G_CONNECT_SWAPPED | G_CONNECT_AFTER);

      cc_permission_infobar_set_permission (self->permission_infobar,
                                            self->permission);
      cc_permission_infobar_set_title (self->permission_infobar,
				       _("Unlock to Add Printers and Change Settings"));

      on_permission_changed (self);
    }
  else
    g_warning ("Your system does not have the cups-pk-helper's policy \
\"org.opensuse.cupspkhelper.mechanism.all-edit\" installed. \
Please check your installation");

  self->size_group = gtk_size_group_new (GTK_SIZE_GROUP_HORIZONTAL);

  actualize_printers_list (self);
  
  self->printer_device_backend = g_new0(Avahi, 4);
    for (int x = 0; x < 4; x++)
    {
      // printer_device_backend[x] = g_new0 (Avahi, 1);
      self->printer_device_backend[x].system_objects = NULL; 
      self->printer_device_backend[x].avahi_cancellable = g_cancellable_new ();
      self->printer_device_backend[x].dbus_connection = g_bus_get_sync (G_BUS_TYPE_SYSTEM, self->printer_device_backend[x].avahi_cancellable, NULL);
      self->printer_device_backend[x].loop =  g_main_loop_new(NULL, FALSE);
      // self->printer_device_backend[x].user_data = user_data;
    }
    self->printer_device_backend[0].service_type = "_ipps-system._tcp";
    self->printer_device_backend[1].service_type = "_ipp-system._tcp";
    self->printer_device_backend[2].service_type = "_ipps._tcp";
    self->printer_device_backend[3].service_type = "_ipp._tcp";

  actualize_ipp_device_list (self);

  attach_to_cups_notifier (self);

  get_all_ppds_async (cc_panel_get_cancellable (CC_PANEL (self)),
                      get_all_ppds_async_cb,
                      self);

  pp_cups_connection_test_async (self->cups, cc_panel_get_cancellable (CC_PANEL (self)), connection_test_cb, self);
  cc_panel_set_content (CC_PANEL (self), top_widget);

  widget = (GtkWidget*)
    gtk_builder_get_object (self->builder, "top-right-buttons");
  adw_header_bar_pack_end (ADW_HEADER_BAR (cc_panel_get_titlebar (CC_PANEL (self))),
                           widget);
}
