#include "pp-cups.h"
#include "pp-ipp-device.h"
#include <cups/cups.h>

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


static int 
compare_services (gconstpointer      data1,
                  gconstpointer      data2)
{
        AvahiData *data_1 = (AvahiData*)data1;
        AvahiData *data_2 = (AvahiData*)data2;
        
        return g_strcmp0 (data_1->name,data_2->name);
}


static 
int check_if_cups_request_error ()
{

	if (cupsLastError () >= IPP_STATUS_ERROR_BAD_REQUEST)
	{
		/* request failed */
		// printf("Request failed: %s\n", cupsLastErrorString());
		return 1;
	}

	else
	{
		// printf("Request succeeded!\n");
		return 0;
	}
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

static void 
add_attribute(
	gchar *attr_name,		 // IPP Attribute to be added to object description
	ipp_tag_t value_tag,	 // Type of Attribute
	add_attribute_data data) // Contains IPP response, buffer to add attribute to, and buffer size
{

	ipp_t *response = data.response;
	gchar *buff = data.buff;
	int buff_size = data.buff_size;

	ipp_attribute_t *attr;

	if ((attr = ippFindAttribute(response, attr_name,
								 value_tag)) != NULL)
	{
		const gchar *attr_val;

		if (value_tag == IPP_TAG_ENUM)
		{
			attr_val = ippEnumString(attr_name, ippGetInteger(attr, 0));
		}

		else
		{
			attr_val = ippGetString(attr, 0, NULL);
		}

		snprintf(buff + strlen(buff), buff_size - strlen(buff),
				 "<b>\t %s:</b> = %s\n",
				 attr_name,
				 attr_val);
    add_option (data.service, attr_name, (gchar*)attr_val); /*PRO*/
	}

	else
	{
		snprintf(buff + strlen(buff), buff_size - strlen(buff),
				 "<b>\t %s:</b> = unknown\n",
				 attr_name);
	}
  
}

/*
 * Get-System-Attributes or Get-(Object)-Attributes Operation
 * Returns:
 * 			1 if success
 * 			0 if failure
 */

static int 
get_attributes (int     obj_type_enum, // type of object (enum value)
              	http_t  *http,	   // http connection
              	gchar   *uri,		   // object uri
              	gchar   *buff,	   // buffer to add attributes to
              	int     buff_size,	   // buffer size
                cups_dest_t* dest)
{

	int operation;
	char *uri_tag;
	char *resource;

  
	if (obj_type_enum == SYSTEM_OBJECT)
	{
		operation = IPP_OP_GET_SYSTEM_ATTRIBUTES;
    add_option (dest, "OBJ_TYPE", "SYSTEM_OBJECT");
		uri_tag = "system-uri";
		resource = "/ipp/system";
	}

	else
	{

		/* Add other conditions for scanner, print-queue etc. */
		operation = IPP_OP_GET_PRINTER_ATTRIBUTES;
    add_option(dest, "OBJ_TYPE", "PRINTER_OBJECT");
		uri_tag = "printer-uri";
		resource = "/ipp/print";
	}

	
  ipp_t *request = ippNewRequest(operation);
	ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, uri_tag, NULL, uri);
	ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsUser());

	ipp_t *response = cupsDoRequest(http, request, "/ipp/system");

	if (check_if_cups_request_error())
	{

		return 0;
	}

	ipp_attribute_t *attr;
	strcpy(buff, "");

	add_attribute_data data = {response, buff, buff_size, dest};

	if (obj_type_enum == SYSTEM_OBJECT)
	{
		add_attribute ("system-state", IPP_TAG_ENUM, data);
		add_attribute ("system-make-and-model", IPP_TAG_TEXT, data);
		add_attribute ("system-dns-sd-name", IPP_TAG_NAME, data);
		add_attribute ("system-location", IPP_TAG_TEXT, data);
		add_attribute ("system-geo-location", IPP_TAG_URI, data);
	}

	else
	{

		/* Add other conditions for scanner, print-queue etc. */
		add_attribute ("printer-state", IPP_TAG_ENUM, data);
		add_attribute ("printer-make-and-model", IPP_TAG_TEXT, data);
		add_attribute ("printer-dns-sd-name", IPP_TAG_NAME, data);
		add_attribute ("printer-location", IPP_TAG_TEXT, data);
		add_attribute ("printer-geo-location", IPP_TAG_URI, data);
		add_attribute ("printer-more-info", IPP_TAG_URI, data);
		add_attribute ("printer-supply-info-uri", IPP_TAG_URI, data);
	}

	return 1;
}

static int
get_printers (http_t            *http,			   // http connection
				      AvahiData         *so,	   // system object (on which get_printers is to be run)
				      int               buff_size)			   // size of object attribute field
{
	gchar *uri = so->uri;
	int check = 1;

	ipp_t *request = ippNewRequest(IPP_OP_GET_PRINTERS);
	ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "system-uri", NULL, uri);
	ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsUser());

	ipp_t *response = cupsDoRequest(http, request, "/ipp/system");

	if (check_if_cups_request_error())
	{
		return 0;
	}

	ipp_attribute_t *attr = NULL;
	GList *printer_names = NULL;
	GList *printer_uris = NULL;

	for (attr = ippFindAttribute(response, "printer-name", IPP_TAG_NAME); attr; attr = ippFindNextAttribute(response, "printer-name", IPP_TAG_NAME))
	{
		gchar *str = g_strdup(ippGetString(attr, 0, NULL));
		printer_names = g_list_prepend(printer_names, str);
	}

	for (attr = ippFindAttribute(response, "printer-uri-supported", IPP_TAG_URI); attr; attr = ippFindNextAttribute(response, "printer-uri-supported", IPP_TAG_URI))
	{
		gchar *str = g_strdup(ippGetString(attr, 0, NULL));
		printer_uris = g_list_prepend(printer_uris, str);
	}

	if (g_list_length(printer_names) != g_list_length(printer_uris))
	{
		// puts("Error: printer-name and printer-uri-supported attributes not returning correct number of values");
		return 0;
	}

	for (GList *l1 = printer_names, *l2 = printer_uris; (l1 && l2); l1 = l1->next, l2 = l2->next)
	{

		char *printer_name = l1->data;
		char *printer_uri = l2->data;

		gchar buff[buff_size];

		/* Get Printer Attributes */

		cups_dest_t *dest = g_new0 (cups_dest_t, 1);
    dest->options = g_new0 (cups_option_t, 20);
    add_option (dest, "UUID", so->UUID);

		dest->name = g_strdup (printer_name);
    add_option (dest, "device-uri", printer_uri);
   
    if (get_attributes(PRINTER_OBJECT, http, printer_uri, buff, buff_size, dest))
		{
			so->services = g_list_append(so->services, dest);

			// printf("Get-Printer-attributes: Success\n");
		} else
		{
			// printf("Error: Get-Printer-attributes: Failed\n");
			check = 0;
		}
	}

	return check;
}

static void 
get_services (AvahiData* data)
{

    cups_dest_t* dest = g_new0 (cups_dest_t, 1);
    dest->options = g_new0 (cups_option_t, 20);
    
    dest->name = g_strdup (data->name);
    if (data->uri == NULL)
    {

        char uri[1024];
        httpAssembleURI (HTTP_URI_CODING_ALL, uri, sizeof(uri), "ipp", NULL,
                        data->hostname, data->port, "/ipp/system");

        data->uri = g_strdup(uri);
    }
    add_option (dest, "UUID", data->UUID);
    add_option (dest, "device-uri", data->uri);
    
    if ((data->uri != NULL) && (data->objAttr == NULL))
    {

        http_t *http = httpConnect2(data->hostname, data->port, NULL, AF_UNSPEC, HTTP_ENCRYPTION_ALWAYS, 1, 0, NULL);
        if (data->admin_url != NULL)
          add_option (dest, "printer-more-info", data->admin_url);
        else        
          add_option (dest, "printer-more-info", g_strdup_printf("http://%s:%d", data->hostname, data->port));
        
        gchar buff[OBJ_ATTR_SIZE];

        /* Get System Attributes */

        if (get_attributes(SYSTEM_OBJECT, http, data->uri, buff, OBJ_ATTR_SIZE, dest))
        {
            data->objAttr = g_strdup(buff);
            data->services = g_list_append(data->services, dest);
        }

        else
        {
            // printf("Error: Get-system-attributes: Failed\n");
        }
    }

    if ((data->uri != NULL) && (g_list_length(data->services) == 1))
    {

        http_t *http = httpConnect2(data->hostname, data->port, NULL, AF_UNSPEC, HTTP_ENCRYPTION_ALWAYS, 1, 0, NULL);

        /* Get Printers */

        if (get_printers (http, data, OBJ_ATTR_SIZE))
        {
            // printf("Get-Printers: Success\n");
        }
        else
        {
            // printf("Error: Get-Printers: Failed\n");
        }

    }
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
    data->services = g_list_append(data->services, dest);

		add_option (dest, "printer-location", data->location);
    add_option(dest, "hostname", data->hostname);
    return;
}

static void
avahi_service_resolver_cb (GVariant*     output,
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
                                *child;
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
                     if (g_strcmp0(data->object_type, "SYSTEM_OBJECT") == 0)
                      get_services (data);
                     else 
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

static gboolean
unsubscribe_general_subscription_cb (gpointer user_data)
{
        Avahi *printer_device_backend = user_data;

        g_dbus_connection_signal_unsubscribe (printer_device_backend->dbus_connection,
                                              printer_device_backend->avahi_service_browser_subscription_id);
        printer_device_backend->avahi_service_browser_subscription_id = 0;
        printer_device_backend->unsubscribe_general_subscription_id = 0;

        return G_SOURCE_REMOVE;
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

                GVariant *output = g_dbus_connection_call_sync (backend->dbus_connection,
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
                                        NULL);
              avahi_service_resolver_cb (output, backend);

              
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
             backend->done = 1;
          }

   return;
}

static void
avahi_service_browser_new_cb (GVariant*     output,
                              gpointer      user_data)
{
        Avahi               *printer_device_backend;
        GError              *error = NULL;
        
        printer_device_backend = user_data;
        
        if (output)
          {

            g_variant_get (output, "(o)", &printer_device_backend->avahi_service_browser_path);
            printer_device_backend->avahi_service_browser_subscription_id =
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

            if (printer_device_backend->avahi_service_browser_path)
                printer_device_backend->unsubscribe_general_subscription_id = g_idle_add (unsubscribe_general_subscription_cb, printer_device_backend);

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
avahi_create_browsers (gpointer    user_data,
                       char*       service_type)
{ 
        Avahi               *printer_device_backend;  
        printer_device_backend =    user_data;

        /*
         * We need to subscribe to signals of service browser before
         * we actually create it because it starts to emit them right
         * after its creation.
         */
        printer_device_backend->avahi_service_browser_subscription_id =
          g_dbus_connection_signal_subscribe  (printer_device_backend->dbus_connection,
                                               NULL,
                                               AVAHI_SERVICE_BROWSER_IFACE,
                                               NULL,
                                               NULL,
                                               NULL,
                                               G_DBUS_SIGNAL_FLAGS_NONE,
                                               avahi_service_browser_signal_handler,
                                               printer_device_backend,
                                               NULL);
        /*
         * Create service browser for services type.
         */
        GVariant* output = g_dbus_connection_call_sync (printer_device_backend->dbus_connection,
                                AVAHI_BUS,
                                "/",
                                AVAHI_SERVER_IFACE,
                                "ServiceBrowserNew",
                                g_variant_new ("(iissu)",
                                               AVAHI_IF_UNSPEC,
                                               AVAHI_PROTO_UNSPEC,
                                               service_type,
                                               "",
                                               0),
                                G_VARIANT_TYPE ("(o)"),
                                G_DBUS_CALL_FLAGS_NONE,
                                -1,
                                printer_device_backend->avahi_cancellable,
                                NULL);
        
  
        avahi_service_browser_new_cb (output, user_data);

        while (printer_device_backend->done == 0)
                { 
                  g_main_context_iteration (NULL, FALSE);
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

    add_option (src, "sanitize-name", "TRUE");
    
    *dest = *src;
    
    return;
}

int 
cupsGetIPPDevices (cups_dest_t** dests,
                   int          num_of_dests)
{
    int                 no_sys_objs,
                        no_services,
                        it;
    cups_dest_t*        new_dest,
                        temp_dest;
    Avahi*              printer_device_backend[4];
    AvahiData*          sys_obj;
    GList*              sys_objs;
    GHashTable         *service_entries,
                       *unique_entries;


    for (int x = 0; x < 4; x++)
    {
      printer_device_backend[x] = g_new0 (Avahi, 1);
      printer_device_backend[x]->system_objects = NULL; 
      printer_device_backend[x]->avahi_cancellable = g_cancellable_new ();
      printer_device_backend[x]->dbus_connection = g_bus_get_sync (G_BUS_TYPE_SYSTEM, printer_device_backend[x]->avahi_cancellable, NULL);
    }

    avahi_create_browsers (printer_device_backend[0], "_ipps-system._tcp");
    avahi_create_browsers (printer_device_backend[1], "_ipp-system._tcp");
    avahi_create_browsers (printer_device_backend[2], "_ipps._tcp");
    avahi_create_browsers (printer_device_backend[3], "_ipp._tcp");
    
    service_entries = g_hash_table_new (g_str_hash, comp_entries);
    unique_entries = g_hash_table_new(g_str_hash, comp_entries);
   

    /* Max no of devices is 100 */
    new_dest = g_new0 (cups_dest_t, 100);
    it = 0;

    for (int i = 0; i < num_of_dests; i++)
          g_hash_table_insert (service_entries, (*dests)[i].name, GINT_TO_POINTER (i));

    for (int serv_ind = 0; serv_ind < 4; serv_ind++)
    {
      sys_objs = printer_device_backend[serv_ind]->system_objects;
      no_sys_objs = g_list_length (sys_objs);

      for (int i = 0; i < no_sys_objs; i++)
      {
          sys_obj = g_list_nth_data (sys_objs, i);
          no_services = g_list_length (sys_obj->services);

          for (int j = 0; j < no_services; j++)
            {
                temp_dest = *(cups_dest_t*)(g_list_nth_data (sys_obj->services, j));

                if (g_hash_table_contains(unique_entries, temp_dest.name))
                  continue;

                if (g_hash_table_contains (service_entries, temp_dest.name))
                {
                  add_interface_data (&new_dest[it++], &temp_dest);
                  // add_interface_data (&new_dest[it-1], (*dests)[i]);
                  g_hash_table_insert (unique_entries, temp_dest.name, GINT_TO_POINTER(it-1));
                  g_hash_table_remove (service_entries, temp_dest.name);
                }
                else
                {
                    new_dest[it++] = temp_dest;
                    g_hash_table_insert(unique_entries, temp_dest.name, GINT_TO_POINTER(it));
                }
            }
      }
    }

    for (int i = 0; i < num_of_dests; i++)
    {
      if (g_hash_table_contains (service_entries, (*dests)[i].name))
          new_dest[it++] = (*dests)[i];
    }

    *dests = new_dest;
    
    g_hash_table_destroy (service_entries);

    return it;
}
