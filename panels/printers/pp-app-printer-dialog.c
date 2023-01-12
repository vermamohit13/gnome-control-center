#include "pp-app-printer-dialog.h"
#include <glib-object.h>
#include <ctype.h>
#include <curl/curl.h>
#include <stdio.h>
#define _(String) gettext(String)
enum
{
  PRINTER_APP_NAMES_COLUMN = 0
};

typedef struct
{
  gchar        *printer_app_name;
  gchar        *driver_name;
  gchar        *description;
  gchar        *device_id;
  gint          status;
} printer_app;

typedef struct
{
  printer_app   **printer_apps;
  gsize           num_of_apps;
}AppsList;

struct _PpAppPrinterDialog
{
  GtkDialog parent_instance;

  GtkButton             *app_printer_dialog_search;
  GtkButton             *app_printer_dialog_install;
  GtkButton             *app_printer_dialog_auto_add;
  GtkButton             *app_printer_dialog_web;
  GtkBox                *app_printer_main_box;
  GtkScrolledWindow     *app_printer_scrolled_window;
  GtkTreeView           *app_printer_list_treeview;
  gchar                 *app_printer_name;
  // GtkTreeSelection *treeview-selection;
  // GtkLabel    *app_printer_selected_app;
  PpPrintDevice         *device;
  int                   app_printer_ind;
  char                  *device_id;
  char                  *printer_app_name;
  AppsList              *list;
};

G_DEFINE_TYPE (PpAppPrinterDialog, pp_app_printer_dialog, GTK_TYPE_DIALOG)

static void
pp_app_printer_dialog_dispose (GObject *object)
{
  PpAppPrinterDialog *self = PP_APP_PRINTER_DIALOG (object);
  G_OBJECT_CLASS (pp_app_printer_dialog_parent_class)->dispose (object);
}

struct MemoryStruct 
{
  char *memory;
  size_t size;
};

static void *copy_word(char *source, size_t n) 
{
  source[n] = '\0';
  char *word = (char *) malloc(n + 1);
  strncpy(word, source, n + 1);
  return word;
}
static void tokenize(const char *line, char app_data[][1000]) 
{
  if (line == NULL) 
  {
    return ;
  }
  size_t line_length = strlen(line);
  char token[line_length];
  int k = 0;
  size_t n = 0, n_max = line_length;
  const int MODE_NORMAL = 0,
            MODE_SQUOTE = 1,
            MODE_DQUOTE = 2;
  int mode = MODE_NORMAL;
  for (unsigned int i = 0; i < line_length; i++) {
    char c = line[i];
    if (mode == MODE_NORMAL) {
      if (c == '\'') {
        mode = MODE_SQUOTE;
      } else if (c == '"') {
        mode = MODE_DQUOTE;
      } else if (isspace(c)) {
        if (n > 0) {
          if(k == 0)
           token[n-1] = '\0';
          void *word = copy_word(token, n);
          strcpy(app_data[k++], word);
          free(word);
          n = 0;
        }
      } else {
        token[n++] = c;
      }
    } else if (mode == MODE_SQUOTE) {
      if (c == '\'') {
        mode = MODE_NORMAL;
      } else {
        token[n++] = c;
      }
    } else if (mode == MODE_DQUOTE) {
      if (c == '"') {
        mode = MODE_NORMAL;
      } else {
        token[n++] = c;
      }
    }
    if (n + 1 >= n_max) 
      break;
  }

  if (n > 0) {
    void *word = copy_word(token, n);
     if(k == 0)
           token[n-1] = '\0';
    strcpy(app_data[k++],word);
    free(word);
    n = 0;
  }
  return;
}

static void parse_apps (PpAppPrinterDialog *self, char *data)
{
   char *line = strtok (data, "\n");
   char app_data [5][1000];
   self->list->num_of_apps = 0;

   while (line != NULL)
   {
      tokenize (line, app_data);
      if (self->list->printer_apps[self->list->num_of_apps] == NULL)
        {
          self->list->printer_apps[self->list->num_of_apps] = g_new0 (printer_app, 1);
        }      
      self->list->printer_apps[self->list->num_of_apps]->printer_app_name = g_strdup (app_data[0]);
      self->list->printer_apps[self->list->num_of_apps]->driver_name = g_strdup (app_data[1]);
      self->list->printer_apps[self->list->num_of_apps]->description = g_strdup (app_data[2]);
      self->list->printer_apps[self->list->num_of_apps]->device_id = g_strdup (app_data[3]);
      self->list->num_of_apps++;
     line = strtok (NULL,"\n");
   }
   return;
} 

static size_t
read_chunks(void *contents, size_t size, size_t nmemb, void *userp)
{
  size_t realsize = size * nmemb;
  struct MemoryStruct *mem = (struct MemoryStruct *)userp;
 
  char *ptr = realloc(mem->memory, mem->size + realsize + 1);
  if(!ptr) {
    /* out of memory! */
    g_warning("Not enough memory (realloc returned NULL)\n");
    return 0;
  }
 
  mem->memory = ptr;
  memcpy (&(mem->memory[mem->size]), contents, realsize);
  mem->size += realsize;
  mem->memory[mem->size] = 0;
 
  return realsize;
}
static void
fill_apps_list (PpAppPrinterDialog *self)
{
  GtkTreeSelection *selection;
  g_autoptr(GtkListStore) store = NULL;
  GtkTreePath      *path;
  GtkTreeView      *treeview;
  GtkTreeIter       iter;
  GtkTreeIter      *preselect_iter = NULL;
  gint              i,
                    start;

  treeview = self->app_printer_list_treeview;
  start = 0;
  if (self->list)
    {
      store = gtk_list_store_new (1, G_TYPE_STRING, G_TYPE_STRING);
    
      if (self->list->num_of_apps > 1)
      {
          gtk_list_store_append (store, &iter);
          gtk_list_store_set (store, &iter,
                              PRINTER_APP_NAMES_COLUMN, g_strdup_printf("%s (Recommended)",self->list->printer_apps[0]->printer_app_name),
                              -1);
              preselect_iter = gtk_tree_iter_copy (&iter);
              start++;
      }
      
      for (i = start ; i < self->list->num_of_apps; i++)
        {
          gtk_list_store_append (store, &iter);
          gtk_list_store_set (store, &iter,
                              PRINTER_APP_NAMES_COLUMN, self->list->printer_apps[i]->printer_app_name,
                             -1);
          if (i == 0)
            {
              preselect_iter = gtk_tree_iter_copy (&iter);
            }
        }

      gtk_tree_view_set_model (treeview, GTK_TREE_MODEL (store));

      if (preselect_iter &&
          (selection = gtk_tree_view_get_selection (treeview)) != NULL)
        {
          gtk_tree_selection_select_iter (selection, preselect_iter);
          path = gtk_tree_model_get_path (GTK_TREE_MODEL (store), preselect_iter);
          gtk_tree_view_scroll_to_cell (treeview, path, NULL, TRUE, 0.5, 0.0);
          gtk_tree_path_free (path);
          gtk_tree_iter_free (preselect_iter);
        }
    }
}

static void
apps_selection_changed_cb (PpAppPrinterDialog *self)
{
  GtkTreeView  *treeview;
  GtkTreeModel *model;
  GtkTreeIter   iter;
  GtkButton    *app_printer_dialog_auto_add;
  GtkButton    *app_printer_dialog_web;
  gchar        *printer_app_name = NULL;

  treeview = self->app_printer_list_treeview;
  if (gtk_tree_selection_get_selected (gtk_tree_view_get_selection (treeview), &model, &iter))
    {
      gtk_tree_model_get (model, &iter,
                          PRINTER_APP_NAMES_COLUMN, &printer_app_name,
                          -1);
    }

  app_printer_dialog_auto_add = self->app_printer_dialog_auto_add;
  app_printer_dialog_web = self->app_printer_dialog_web;

  if (printer_app_name)
    {
      for(int i = 0; i < self->list->num_of_apps ; i++)
      {
        if (printer_app_name == self->list->printer_apps[i]->printer_app_name && self->list->printer_apps[i]->status)
          { 
            gtk_widget_set_sensitive (GTK_WIDGET (app_printer_dialog_auto_add), TRUE);
            gtk_widget_set_sensitive (GTK_WIDGET (app_printer_dialog_web), TRUE);
          }
      }
      g_free (printer_app_name);
    }
  else
    {
      gtk_widget_set_sensitive (GTK_WIDGET (app_printer_dialog_auto_add), FALSE);
      gtk_widget_set_sensitive (GTK_WIDGET (app_printer_dialog_web), FALSE);
    }
}

static void 
poll_printer_app_cb (PpAppPrinterDialog *self)
{
  char          buf[4196];
  FILE          *fp;
  
  for(int i = 0; i < self->list->num_of_apps; i++)
  {
    gchar *cmd = g_strdup_printf ("/snap/bin/%s drivers -o device-id=%s 2>/dev/null", self->list->printer_apps[i]->printer_app_name, self->device_id);
     if ((fp = popen (cmd,"r")) == NULL)
     {
       char *message = g_strdup_printf ("Couldn't poll printer %s", self->list->printer_apps[i]->printer_app_name);
       g_warning (message);
       g_free (message);
     }

    fgets (buf, sizeof (buf), fp);
    g_message ("%s\n",buf);
    if ( buf == '\0')
     {
       self->list->printer_apps[i]->status = 0;
     }
     else 
     {
       self->list->printer_apps[i]->status = 1; 
     }
  }
  
}
static void
populate_app_dialog (PpAppPrinterDialog *self)
{
  GtkTreeViewColumn *column,
                    *prev_column;  
  GtkCellRenderer   *renderer;
  GtkTreeView       *apps_treeview;
  GtkWidget         *header;

  apps_treeview = self->app_printer_list_treeview;

  renderer = gtk_cell_renderer_text_new ();
  gtk_cell_renderer_set_padding (renderer, 10, 0);

  /* Translators: Name of column showing printer app */
  column = gtk_tree_view_column_new_with_attributes (_("Apps"), renderer,
                                                     "text", PRINTER_APP_NAMES_COLUMN, NULL);
  gtk_tree_view_column_set_expand (column, TRUE);
  header = gtk_label_new (gtk_tree_view_column_get_title (column));
  gtk_widget_set_margin_start (header, 10);
  gtk_tree_view_column_set_widget (column, header);
  gtk_widget_show (header);
  
  prev_column = gtk_tree_view_get_column (apps_treeview, 0);
  if (prev_column)
   {
    gtk_tree_view_remove_column (apps_treeview, prev_column);
   }

  gtk_tree_view_append_column (apps_treeview, column);

  g_signal_connect_object (gtk_tree_view_get_selection (apps_treeview),
                           "changed", G_CALLBACK (apps_selection_changed_cb), self, G_CONNECT_SWAPPED);
  // poll_printer_app_cb(self);
  fill_apps_list (self);
}

static void
search_app_printer_cb (PpAppPrinterDialog *self)
{
   CURL *curl;
   CURLcode res;
   struct MemoryStruct buf;
   buf.size = 0;
   buf.memory = malloc(1);
   if(curl_global_init(CURL_GLOBAL_DEFAULT))
   {
     g_error("curl_global_init() returned non-zero value.\n");
     return;
   }
   gchar URL = g_strdup_printf ("https://openprinting.org/query.php?papps=true&device-id=%s",pp_print_device_get_device_id(self->device));
   curl = curl_easy_init();
   if(curl) {
    //  curl_easy_setopt(curl, CURLOPT_URL, URL);
     curl_easy_setopt (curl, CURLOPT_URL, "https://openprinting.org/query.php?papps=true&device-id=MFG:HP;MDL:LaserJet%204050");
     curl_easy_setopt (curl, CURLOPT_WRITEFUNCTION, read_chunks);
     curl_easy_setopt (curl, CURLOPT_WRITEDATA, (void *)&buf);
     curl_easy_setopt (curl, CURLOPT_VERBOSE, 1L);
     curl_easy_setopt (curl, CURLOPT_TIMEOUT, 10L);
     curl_easy_setopt (curl, CURLOPT_FAILONERROR, 1L);     
     res = curl_easy_perform(curl);
      /* always cleanup */

     if(res == CURLE_OK)
        parse_apps (self, buf.memory);
     else if ( res == CURLE_OPERATION_TIMEDOUT)
        g_warning ("Curl Timed out\n");      
      else
        g_warning(curl_easy_strerror(res));
   }
   else {
        g_warning("Error while using curl_easy_init().\n");
   }
     curl_easy_cleanup(curl);
     populate_app_dialog(self);  
}

static void
auto_add_app_printer_cb (PpAppPrinterDialog *self)
{

}

static void
web_app_printer_cb (PpAppPrinterDialog *self)
{
    //  gchar *URL;      // Need to make a API call to cups-pk-helper for the url of printer Application

}

static void
install_app_printer_cb (PpAppPrinterDialog *self)
{

}


static void
app_printer_dialog_response_cb (PpAppPrinterDialog *self,
                                  gint       response_id)
{
  GtkTreeSelection *selection;
  GtkTreeModel     *model;
  GtkTreeView      *models_treeview;
  GtkTreeIter       iter;
  if (response_id == GTK_RESPONSE_OK)
    {
      models_treeview = self->app_printer_list_treeview;

      if (models_treeview)
        {
          selection = gtk_tree_view_get_selection (models_treeview);

          if (selection)
            {
              if (gtk_tree_selection_get_selected (selection, &model, &iter))
                {
                  gtk_tree_model_get (model, &iter,
                                      PRINTER_APP_NAMES_COLUMN, &self->printer_app_name,
                                      // PRINTER_APP_DISPLAY_NAMES_COLUMN, &self->printer_app_name,
                                      -1);
                 g_message("%s\n",self->printer_app_name);
                }
            }
        }
    }

  // self->user_callback (GTK_DIALOG (self), response_id, self->user_data); callback comes here
}

static void
pp_app_printer_dialog_init (PpAppPrinterDialog *self)
{
      gtk_widget_init_template (GTK_WIDGET (self));
}

void 
pp_app_printer_dialog_class_init (PpAppPrinterDialogClass *klass)
{

  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  gtk_widget_class_set_template_from_resource (widget_class, "/org/gnome/control-center/printers/app-printer-dialog.ui");

  // gtk_widget_class_bind_template_child (widget_class, PpAppPrinterDialog, app_printer_install);
  gtk_widget_class_bind_template_child (widget_class, PpAppPrinterDialog, app_printer_dialog_search);
  gtk_widget_class_bind_template_child (widget_class, PpAppPrinterDialog, app_printer_dialog_install);
  gtk_widget_class_bind_template_child (widget_class, PpAppPrinterDialog, app_printer_dialog_auto_add);
  gtk_widget_class_bind_template_child (widget_class, PpAppPrinterDialog, app_printer_dialog_web);
  gtk_widget_class_bind_template_child (widget_class, PpAppPrinterDialog, app_printer_main_box);
  gtk_widget_class_bind_template_child (widget_class, PpAppPrinterDialog, app_printer_scrolled_window);
  gtk_widget_class_bind_template_child (widget_class, PpAppPrinterDialog, app_printer_list_treeview);
  
  object_class -> dispose = pp_app_printer_dialog_dispose;
}

PpAppPrinterDialog *
pp_app_printer_dialog_new (PpPrintDevice *device)
{
    PpAppPrinterDialog *self;

    self = g_object_new (pp_app_printer_dialog_get_type(), NULL);
    self->list = g_new0(AppsList,1);
    self->list->printer_apps = g_new0(printer_app *,100);
    self->list->num_of_apps = 0;
    self->device = device;
    g_signal_connect_object (self, "response", G_CALLBACK (app_printer_dialog_response_cb), self, G_CONNECT_SWAPPED);
    g_signal_connect_object (self->app_printer_dialog_search, "clicked" , G_CALLBACK (search_app_printer_cb),self,G_CONNECT_SWAPPED);
    g_signal_connect_object (self->app_printer_dialog_install, "clicked" , G_CALLBACK (install_app_printer_cb),self,G_CONNECT_SWAPPED);
    g_signal_connect_object (self->app_printer_dialog_web, "clicked" , G_CALLBACK (web_app_printer_cb),self,G_CONNECT_SWAPPED);
    g_signal_connect_object (self->app_printer_dialog_auto_add, "clicked" , G_CALLBACK (auto_add_app_printer_cb),self,G_CONNECT_SWAPPED);    
    return self;
}