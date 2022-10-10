#pragma once

#include <gtk/gtk.h>
#include "pp-print-device.h"

G_BEGIN_DECLS

#define PP_TYPE_APP_PRINTER_DIALOG (pp_app_printer_dialog_get_type ())
G_DECLARE_FINAL_TYPE (PpAppPrinterDialog, pp_app_printer_dialog, PP, APP_PRINTER_DIALOG, GtkDialog)

PpAppPrinterDialog *pp_app_printer_dialog_new               (PpPrintDevice *device);

G_END_DECLS