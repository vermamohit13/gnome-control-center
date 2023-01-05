#include <config.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <glib.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <time.h>
#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-common/error.h>
#include <avahi-common/timeval.h>
#include <avahi-common/thread-watch.h>
#include <avahi-glib/glib-watch.h>
#include <avahi-glib/glib-malloc.h>


#include "pp-dns-window.h"


struct _PpDnsWindow
{
    GtkWindow  parent;

    GtkBuilder *builder;

    GtkButton   *dw_top_box_in;
    GtkLabel    *dw_main;

    GtkListBox  *dw_right_list;
    GHashTable  *service_hash_table;
    gboolean is_authorized;

    cups_dest_t dummy_print_dest;

    const AvahiPoll *poll_api;
    AvahiThreadedPoll *avahi_threaded_poll;
    AvahiClient *client;
    struct timeval tv;
    const char *version;
};


G_DEFINE_TYPE(PpDnsWindow, pp_dns_window, GTK_TYPE_WINDOW);

/* Callback for Avahi API Timeout Event */
static void
avahi_timeout_event (AVAHI_GCC_UNUSED AvahiTimeout *timeout, AVAHI_GCC_UNUSED void *userdata)
{
    g_message ("Avahi API Timeout reached!");
}

/* Callback for GLIB API Timeout Event */
static gboolean
avahi_timeout_event_glib (void *userdata)
{
    GMainLoop *loop = userdata;
    g_message ("GLIB API Timeout reached, quitting main loop!");
    /* Quit the application */
    g_main_loop_quit (loop);
    return FALSE; /* Don't re-schedule timeout event */
}

/* Callback for state changes on the Client */
static void
avahi_client_callback (AVAHI_GCC_UNUSED AvahiClient *client, AvahiClientState state, void *userdata)
{
    GMainLoop *loop = userdata;
    g_message ("Avahi Client State Change: %d", state);
    if (state == AVAHI_CLIENT_FAILURE)
    {
        /* We we're disconnected from the Daemon */
        g_message ("Disconnected from the Avahi Daemon: %s", avahi_strerror(avahi_client_errno(client)));
        /* Quit the application */
        g_main_loop_quit (loop);
    }
}

static void resolve_callback(
    AvahiServiceResolver *r,
    AVAHI_GCC_UNUSED AvahiIfIndex interface,
    AVAHI_GCC_UNUSED AvahiProtocol protocol,
    AvahiResolverEvent event,
    const char *name,
    const char *type,
    const char *domain,
    const char *host_name,
    const AvahiAddress *address,
    uint16_t port,
    AvahiStringList *txt,
    AvahiLookupResultFlags flags,
    AVAHI_GCC_UNUSED PpDnsWindow *self)
{

    assert(r);
    gchar* port_str = g_strdup_printf("%i", port);

    switch (event)
    {
    case AVAHI_RESOLVER_FAILURE:
         g_fprintf(stderr, "(Resolver) Failed to resolve service '%s' of type '%s' in domain '%s': %s\n", name, type, domain, avahi_strerror(avahi_client_errno(avahi_service_resolver_get_client(r))));
         break;
    case AVAHI_RESOLVER_FOUND:
    {
        char* admin_url = NULL;

        AvahiStringList* list = avahi_string_list_find( txt, "adminurl" );

      /* If the key adminurl is found in the TXT records */
        if ( list != NULL){
          char* key, * value;

          avahi_string_list_get_pair( list, &key, &value, NULL );
          admin_url = g_strdup( value );

          avahi_free( key );
          avahi_free( value );
        }

        gchar* service_name_unique = g_strjoin(":",name,type,domain, NULL);

        if (  g_hash_table_lookup (self->service_hash_table, service_name_unique) == NULL){

          PpPrinterDnsEntry* row_list = pp_printer_dns_entry_new ( name, type, domain, host_name, port_str, admin_url , self->is_authorized);

          gtk_widget_show ((GtkWidget*)row_list);
          g_hash_table_insert (self->service_hash_table, service_name_unique, row_list);
          gtk_list_box_insert (self->dw_right_list,GTK_WIDGET (row_list), -1);
        }

        char a[AVAHI_ADDRESS_STR_MAX], *t;
        g_fprintf(stderr, "Service '%s' of type '%s' in domain '%s':\n", name, type, domain);
        avahi_address_snprint(a, sizeof(a), address);
        t = avahi_string_list_to_string(txt);


        g_fprintf(stderr,
                "\t%s:%u (%s)\n"
                "\tTXT=%s\n"
                "\tcookie is %u\n"
                "\tis_local: %i\n"
                "\tour_own: %i\n"
                "\twide_area: %i\n"
                "\64tmulticast: %i\n"
                "\tcached: %i\n",
                host_name, port, a,
                t,
                avahi_string_list_get_service_cookie(txt),
                !!(flags & AVAHI_LOOKUP_RESULT_LOCAL),
                !!(flags & AVAHI_LOOKUP_RESULT_OUR_OWN),
                !!(flags & AVAHI_LOOKUP_RESULT_WIDE_AREA),
                !!(flags & AVAHI_LOOKUP_RESULT_MULTICAST),
                !!(flags & AVAHI_LOOKUP_RESULT_CACHED));
        avahi_free(t);
    }
    }
    avahi_service_resolver_free(r);


}

static void browse_callback(
    AvahiServiceBrowser *b,
    AvahiIfIndex interface,
    AvahiProtocol protocol,
    AvahiBrowserEvent event,
    const char *name,
    const char *type,
    const char *domain,
    AVAHI_GCC_UNUSED AvahiLookupResultFlags flags,
    PpDnsWindow *self)
{
    AvahiClient *c = self->client;
    assert(b);
    /* Called whenever a new services becomes available on the LAN or is removed from the LAN */
    switch (event)
    {
    case AVAHI_BROWSER_FAILURE:
        g_fprintf(stderr, "(Browser) %s\n", avahi_strerror(avahi_client_errno(avahi_service_browser_get_client(b))));

        return;
    case AVAHI_BROWSER_NEW:
        g_fprintf(stderr, "(Browser) NEW: service '%s' of type '%s' in domain '%s'\n", name, type, domain);
        /* We ignore the returned resolver object. In the callback
               function we free it. If the server is terminated before
               the callback function is called the server will free
               the resolver for us. */
        if (!(avahi_service_resolver_new(c, interface, protocol, name, type, domain, AVAHI_PROTO_UNSPEC, 0, resolve_callback, self)))
            g_fprintf(stderr, "Failed to resolve service '%s': %s\n", name, avahi_strerror(avahi_client_errno(c)));

        break;
    case AVAHI_BROWSER_REMOVE:
        {
           gchar* service_name_unique = g_strjoin(":",name,type,domain, NULL);

           PpPrinterDnsEntry* row_to_be_deleted  = g_hash_table_lookup(self->service_hash_table,service_name_unique);

            if (row_to_be_deleted != NULL){

              gtk_window_destroy(GTK_WIDGET (row_to_be_deleted));
              g_hash_table_remove (self->service_hash_table, service_name_unique);

            }
          g_fprintf(stderr, "(Browser) REMOVE: service '%s' of type '%s' in domain '%s'\n", name, type, domain);
          break;
        }
    case AVAHI_BROWSER_ALL_FOR_NOW:
    case AVAHI_BROWSER_CACHE_EXHAUSTED:
        g_fprintf(stderr, "(Browser) %s\n", event == AVAHI_BROWSER_CACHE_EXHAUSTED ? "CACHE_EXHAUSTED" : "ALL_FOR_NOW");
        break;
    }
}

void destroy(PpDnsWindow * self){
  /* Clean up */

  avahi_threaded_poll_stop(self->avahi_threaded_poll);
  avahi_client_free (self->client);
  avahi_threaded_poll_free (self->avahi_threaded_poll);
}

static void
pp_dns_window_class_init(PpDnsWindowClass *klass){

  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  gtk_widget_class_set_template_from_resource(widget_class, "/org/gnome/control-center/printers/pp-dns-window.ui");

  gtk_widget_class_bind_template_child (widget_class, PpDnsWindow, dw_top_box_in);
  gtk_widget_class_bind_template_child (widget_class, PpDnsWindow, dw_main);

  gtk_widget_class_bind_template_child (widget_class, PpDnsWindow, dw_right_list);

}

static void start_avahi_in_background(PpDnsWindow *self){
  int error;


  /* Optional: Tell avahi to use g_malloc and g_free */
  avahi_set_allocator (avahi_glib_allocator ());

  /* Create the threaded poll Adaptor */
  self->avahi_threaded_poll = avahi_threaded_poll_new ();
  self->poll_api = avahi_threaded_poll_get (self->avahi_threaded_poll);

  /* Create a new AvahiClient instance */
  self->client = avahi_client_new (self->poll_api,  /* AvahiPoll object from above */
                               0,
            avahi_client_callback,                  /* Callback function for Client state changes */
            NULL,                                   /* User data */
            &error);                                /* Error return */

  /* Check the error return code */
  if (self->client == NULL)
  {
      /* Print out the error string */
      g_warning ("Error initializing Avahi: %s", avahi_strerror (error));
      destroy(self);
  }

  AvahiServiceBrowser *sb_ipps = NULL;
  /* Create the service browser */
  if (!(sb_ipps = avahi_service_browser_new(self->client, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, "_ipps._tcp", NULL, 0, browse_callback, self)))
  {
      g_fprintf(stderr, "Failed to create service browser: %s\n", avahi_strerror(avahi_client_errno(self->client)));
      destroy(self);
  }

  AvahiServiceBrowser *sb_ipp = NULL;
  /* Create the service browser */
  if (!(sb_ipp = avahi_service_browser_new(self->client, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, "_ipp._tcp", NULL, 0, browse_callback, self)))
  {
      g_fprintf(stderr, "Failed to create service browser: %s\n", avahi_strerror(avahi_client_errno(self->client)));
      destroy(self);
  }

  AvahiServiceBrowser *sb_ftp = NULL;
  /* Create the service browser */
  if (!(sb_ftp = avahi_service_browser_new(self->client, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, "_ftp._tcp", NULL, 0, browse_callback, self)))
  {
      g_fprintf(stderr, "Failed to create service browser: %s\n", avahi_strerror(avahi_client_errno(self->client)));
      destroy(self);
  }

  AvahiServiceBrowser *sb_http = NULL;
  /* Create the service browser */
  if (!(sb_http = avahi_service_browser_new(self->client, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, "_http._tcp", NULL, 0, browse_callback, self)))
  {
      g_fprintf(stderr, "Failed to create service browser: %s\n", avahi_strerror(avahi_client_errno(self->client)));
      destroy(self);
  }

  AvahiServiceBrowser *sb_https = NULL;
  /* Create the service browser */
  if (!(sb_http = avahi_service_browser_new(self->client, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, "_https._tcp", NULL, 0, browse_callback, self)))
  {
      g_fprintf(stderr, "Failed to create service browser: %s\n", avahi_strerror(avahi_client_errno(self->client)));
      destroy(self);
  }

  /* Make a call to get the version string from the daemon */
  self->version = avahi_client_get_version_string (self->client);

  /* Check if the call suceeded */
  if (self->version == NULL)
  {
      g_warning ("Error getting version string: %s", avahi_strerror (avahi_client_errno (self->client)));
      destroy(self);
  }
  g_message ("Avahi Server Version: %s", self->version);

  /* Finally, start the event loop thread */
  avahi_threaded_poll_start(self->avahi_threaded_poll);
}

static void
pp_dns_window_init(PpDnsWindow *self){

  self->is_authorized = TRUE;

  gtk_widget_init_template(GTK_WIDGET (self));

//   GdkColor color;
//   gdk_color_parse("#f6f5f4", &color);
//   gtk_widget_modify_bg((GtkWidget*)(self->dw_right_list), GTK_STATE_NORMAL, &color);

  start_avahi_in_background (self);
//   gtk_widget_set_no_show_all (self->dw_right_list, TRUE);
  self->service_hash_table = g_hash_table_new_full (g_str_hash,
                                                 g_str_equal,
                                                 g_free,
                                                 NULL);

}

PpDnsWindow*
pp_dns_window_new()
{
  PpDnsWindow *self;
  self = g_object_new(PP_DNS_WINDOW_TYPE, NULL);

  return self;
}


