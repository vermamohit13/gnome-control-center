#pragma once

#include <gtk/gtk.h>
#include <cups/cups.h>
#include "pp-dns-row.h"
G_BEGIN_DECLS

#define PP_DNS_WINDOW_TYPE (pp_dns_window_get_type())
G_DECLARE_FINAL_TYPE (PpDnsWindow, pp_dns_window, PP, DNS_WINDOW, GtkWindow)

PpDnsWindow* pp_dns_window_new();

G_END_DECLS