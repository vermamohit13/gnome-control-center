
#pragma once


#include <cups/cups.h>
#include <pappl/device.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>
#include <grp.h>
#include <pwd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include <gio/gio.h>

#include "glibconfig.h"
#include "pp-cups.h"

typedef enum
{
    SYSTEM_OBJECT,
    PRINTER_OBJECT,
    SCANNER_OBJECT,
    PRINTER_QUEUE

} obj_type;

typedef struct add_attribute_data
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
        guint                avahi_service_browser_subscription_id_ind;           
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

// int                           cupsGetIPPDevices (gpointer user_data);
Avahi*                           cupsGetIPPDevices (gpointer user_data);