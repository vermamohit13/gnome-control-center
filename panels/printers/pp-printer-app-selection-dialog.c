#include "config.h"

#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>

#include "pp-printer-app-selection-dialog.h"

enum
{
  PPD_NAMES_COLUMN = 0,
  PPD_DISPLAY_NAMES_COLUMN
};

enum
{
  PPD_MANUFACTURERS_NAMES_COLUMN = 0,
  PPD_MANUFACTURERS_DISPLAY_NAMES_COLUMN
};

struct _PpPrinterAppSelectionDialog {
  GtkDialog parent_instance;

  GtkButton   *ppd_selection_select_button;
  GtkSpinner  *ppd_spinner;
  GtkLabel    *progress_label;
  GtkTreeView *ppd_selection_manufacturers_treeview;
  GtkTreeView *ppd_selection_models_treeview;

  UserResponseCallback user_callback;
  gpointer             user_data;

  gchar           *ppd_name;
  gchar           *ppd_display_name;
  gchar           *manufacturer;

  PPDList *list;
};

G_DEFINE_TYPE (PpPrinterAppSelectionDialog, pp_printer_app_selection_dialog, GTK_TYPE_DIALOG)

static void
manufacturer_selection_changed_cb (PpPrinterAppSelectionDialog *self)
{
  GtkTreeView  *treeview;
  g_autoptr(GtkListStore) store = NULL;
  GtkTreeModel *model;
  GtkTreeIter   iter;
  GtkTreeView  *models_treeview;
  gchar        *manufacturer_name = NULL;
  gint          i, index;

  treeview = self->ppd_selection_manufacturers_treeview;
  if (gtk_tree_selection_get_selected (gtk_tree_view_get_selection (treeview), &model, &iter))
    {
      gtk_tree_model_get (model, &iter,
                          PPD_MANUFACTURERS_NAMES_COLUMN, &manufacturer_name,
                          -1);
    }

  if (manufacturer_name)
    {
      index = -1;
      for (i = 0; i < self->list->num_of_manufacturers; i++)
        {
          if (g_strcmp0 (manufacturer_name,
                         self->list->manufacturers[i]->manufacturer_name) == 0)
            {
              index = i;
              break;
            }
        }

      if (index >= 0)
        {
          models_treeview = self->ppd_selection_models_treeview;

          store = gtk_list_store_new (2, G_TYPE_STRING, G_TYPE_STRING);

          for (i = 0; i < self->list->manufacturers[index]->num_of_ppds; i++)
            {
              gtk_list_store_append (store, &iter);
              gtk_list_store_set (store, &iter,
                                  PPD_NAMES_COLUMN, self->list->manufacturers[index]->ppds[i]->ppd_name,
                                  PPD_DISPLAY_NAMES_COLUMN, self->list->manufacturers[index]->ppds[i]->ppd_display_name,
                                  -1);
            }

          gtk_tree_view_set_model (models_treeview, GTK_TREE_MODEL (store));
          gtk_tree_view_columns_autosize (models_treeview);
        }

      g_free (manufacturer_name);
    }
}

static void
model_selection_changed_cb (PpPrinterAppSelectionDialog *self)
{
  GtkTreeView  *treeview;
  GtkTreeModel *model;
  GtkTreeIter   iter;
  GtkButton    *ppd_select_button;
  gchar        *model_name = NULL;

  treeview = self->ppd_selection_models_treeview;
  if (gtk_tree_selection_get_selected (gtk_tree_view_get_selection (treeview), &model, &iter))
    {
      gtk_tree_model_get (model, &iter,
                          PPD_NAMES_COLUMN, &model_name,
                          -1);
    }

  ppd_select_button = self->ppd_selection_select_button;

  if (model_name)
    {
      gtk_widget_set_sensitive (GTK_WIDGET (ppd_select_button), TRUE);
      g_free (model_name);
    }
  else
    {
      gtk_widget_set_sensitive (GTK_WIDGET (ppd_select_button), FALSE);
    }
}

static void
fill_ppds_list (PpPrinterAppSelectionDialog *self)
{
  GtkTreeSelection *selection;
  g_autoptr(GtkListStore) store = NULL;
  GtkTreePath      *path;
  GtkTreeView      *treeview;
  GtkTreeIter       iter;
  GtkTreeIter      *preselect_iter = NULL;
  gint              i;

  gtk_widget_hide (GTK_WIDGET (self->ppd_spinner));
  gtk_spinner_stop (self->ppd_spinner);

  gtk_widget_hide (GTK_WIDGET (self->progress_label));

  treeview = self->ppd_selection_manufacturers_treeview;

  if (self->list)
    {
      store = gtk_list_store_new (2, G_TYPE_STRING, G_TYPE_STRING);

      for (i = 0; i < self->list->num_of_manufacturers; i++)
        {
          gtk_list_store_append (store, &iter);
          gtk_list_store_set (store, &iter,
                              PPD_MANUFACTURERS_NAMES_COLUMN, self->list->manufacturers[i]->manufacturer_name,
                              PPD_MANUFACTURERS_DISPLAY_NAMES_COLUMN, self->list->manufacturers[i]->manufacturer_display_name,
                              -1);

          if (g_strcmp0 (self->manufacturer,
                         self->list->manufacturers[i]->manufacturer_display_name) == 0)
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
populate_dialog (PpPrinterAppSelectionDialog *self)
{
  GtkTreeViewColumn *column;
  GtkCellRenderer   *renderer;
  GtkTreeView       *manufacturers_treeview;
  GtkTreeView       *models_treeview;
  GtkWidget         *header;

  manufacturers_treeview = self->ppd_selection_manufacturers_treeview;

  renderer = gtk_cell_renderer_text_new ();
  gtk_cell_renderer_set_padding (renderer, 10, 0);

  /* Translators: Name of column showing printer manufacturers */
  column = gtk_tree_view_column_new_with_attributes (_("Manufacturer"), renderer,
                                                     "text", PPD_MANUFACTURERS_DISPLAY_NAMES_COLUMN, NULL);
  gtk_tree_view_column_set_expand (column, TRUE);
  header = gtk_label_new (gtk_tree_view_column_get_title (column));
  gtk_widget_set_margin_start (header, 10);
  gtk_tree_view_column_set_widget (column, header);
  gtk_widget_show (header);
  gtk_tree_view_append_column (manufacturers_treeview, column);


  models_treeview = self->ppd_selection_models_treeview;

  renderer = gtk_cell_renderer_text_new ();
  gtk_cell_renderer_set_padding (renderer, 10, 0);

  /* Translators: Name of column showing printer drivers */
  column = gtk_tree_view_column_new_with_attributes (_("Driver"), renderer,
                                                     "text", PPD_DISPLAY_NAMES_COLUMN,
                                                     NULL);
  gtk_tree_view_column_set_expand (column, TRUE);
  header = gtk_label_new (gtk_tree_view_column_get_title (column));
  gtk_widget_set_margin_start (header, 10);
  gtk_tree_view_column_set_widget (column, header);
  gtk_widget_show (header);
  gtk_tree_view_append_column (models_treeview, column);


  g_signal_connect_object (gtk_tree_view_get_selection (models_treeview),
                           "changed", G_CALLBACK (model_selection_changed_cb), self, G_CONNECT_SWAPPED);

  g_signal_connect_object (gtk_tree_view_get_selection (manufacturers_treeview),
                           "changed", G_CALLBACK (manufacturer_selection_changed_cb), self, G_CONNECT_SWAPPED);

  if (!self->list)
    {
      gtk_widget_show (GTK_WIDGET (self->ppd_spinner));
      gtk_spinner_start (self->ppd_spinner);

      gtk_widget_show (GTK_WIDGET (self->progress_label));
    }
  else
    {
      fill_ppds_list (self);
    }
}

static void
ppd_selection_dialog_response_cb (PpPrinterAppSelectionDialog *self,
                                  gint       response_id)
{
  GtkTreeSelection *selection;
  GtkTreeModel     *model;
  GtkTreeView      *models_treeview;
  GtkTreeIter       iter;

  if (response_id == GTK_RESPONSE_OK)
    {
      models_treeview = self->ppd_selection_models_treeview;

      if (models_treeview)
        {
          selection = gtk_tree_view_get_selection (models_treeview);

          if (selection)
            {
              if (gtk_tree_selection_get_selected (selection, &model, &iter))
                {
                  gtk_tree_model_get (model, &iter,
                                      PPD_NAMES_COLUMN, &self->ppd_name,
                                      PPD_DISPLAY_NAMES_COLUMN, &self->ppd_display_name,
                                      -1);
                }
            }
        }
    }

  self->user_callback (GTK_DIALOG (self), response_id, self->user_data);
}

gchar * 
get_installed_printer_apps ()
{
  
}

PpPrinterAppSelectionDialog *
pp_printer_app_selection_dialog_new (PpPrintDevice *device)
{
  PpPrinterAppSelectionDialog *self;

  self = g_object_new (pp_printer_app_selection_dialog_get_type (), NULL);

  // self->user_callback = user_callback;
  // self->user_data = user_data;

  // self->list = ppd_list_copy (ppd_list);

  self->manufacturer = get_installed_printer_app ();

  /* connect signal */
  g_signal_connect_object (self, "response", G_CALLBACK (ppd_selection_dialog_response_cb), self, G_CONNECT_SWAPPED);

  gtk_spinner_start (self->ppd_spinner);

  populate_dialog (self);

  return self;
}

static void
pp_printer_app_selection_dialog_dispose (GObject *object)
{
  PpPrinterAppSelectionDialog *self = PP_PPD_SELECTION_DIALOG (object);

  g_clear_pointer (&self->ppd_name, g_free);
  g_clear_pointer (&self->ppd_display_name, g_free);
  g_clear_pointer (&self->manufacturer, g_free);

  G_OBJECT_CLASS (pp_printer_app_selection_dialog_parent_class)->dispose (object);
}

void
pp_printer_app_selection_dialog_class_init (PpPrinterAppSelectionDialogClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  GObjectClass   *object_class = G_OBJECT_CLASS (klass);

  gtk_widget_class_set_template_from_resource (widget_class, "/org/gnome/control-center/printers/ppd-printer-app-selection-dialog.ui");
  gtk_widget_class_bind_template_child (widget_class, PpPrinterAppSelectionDialog, ppd_selection_select_button);
  gtk_widget_class_bind_template_child (widget_class, PpPrinterAppSelectionDialog, ppd_spinner);
  gtk_widget_class_bind_template_child (widget_class, PpPrinterAppSelectionDialog, progress_label);
  gtk_widget_class_bind_template_child (widget_class, PpPrinterAppSelectionDialog, ppd_selection_manufacturers_treeview);
  gtk_widget_class_bind_template_child (widget_class, PpPrinterAppSelectionDialog, ppd_selection_models_treeview);

  object_class->dispose = pp_printer_app_selection_dialog_dispose;
}

void
pp_printer_app_selection_dialog_init (PpPrinterAppSelectionDialog *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));
}