#pragma once

#include <gtk/gtk.h>
#include "pp-utils.h"

G_BEGIN_DECLS

#define PP_TYPE_PRINTER_APP_SELECTION_DIALOG (pp_printer_app_selection_dialog_get_type ())
G_DECLARE_FINAL_TYPE (PpPrinterAppSelectionDialog, pp_printer_app_selection_dialog, PP, PRINTER_APP_SELECTION_DIALOG, GtkDialog)

PpPrinterAppSelectionDialog *pp_printer_app_selection_dialog_new   ();
gchar                *pp_printer_app_selection_dialog_get_ppd_name         (PpPrinterAppSelectionDialog      *dialog);
gchar                *pp_printer_app_selection_dialog_get_ppd_display_name (PpPrinterAppSelectionDialog      *dialog);
void                  pp_printer_app_selection_dialog_set_ppd_list         (PpPrinterAppSelectionDialog      *dialog,
                                                                            PPDList                   *list);

G_END_DECLS
