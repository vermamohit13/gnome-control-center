panels_list += cappletname
desktop = 'gnome-@0@-panel.desktop'.format(cappletname)

desktop_in = configure_file(
  input: desktop + '.in.in',
  output: desktop + '.in',
  configuration: desktop_conf
)

i18n.merge_file(
  type: 'desktop',
  input: desktop_in,
  output: desktop,
  po_dir: po_dir,
  install: true,
  install_dir: control_center_desktopdir
)

sources = files(
  'cc-printers-panel.c',
  'pp-cups.c',
  'pp-details-dialog.c',
  'pp-host.c',
  'pp-ipp-option-widget.c',
  'pp-job.c',
  'pp-job-row.c',
  'pp-jobs-dialog.c',
  'pp-maintenance-command.c',
  'pp-new-printer-dialog.c',
  'pp-new-printer.c',
  'pp-app-printer-dialog.c',
  'pp-options-dialog.c',
  'pp-ppd-option-widget.c',
  'pp-ppd-selection-dialog.c',
  'pp-print-device.c',
  'pp-printer-entry.c',
  'pp-printer.c',
  'pp-samba.c',
  'pp-utils.c',
  'pp-dns-row.c',
  'pp-dns-window.c'
)

resource_data = files(
  'new-printer-dialog.ui',
  'ppd-selection-dialog.ui',
  'pp-details-dialog.ui',
  'pp-job-row.ui',
  'pp-jobs-dialog.ui',
  'pp-options-dialog.ui',
  'printer-entry.ui',
  'printers.ui',
  'app-printer-dialog.ui',
  'pp-dns-row.ui',
  'pp-dns-window.ui'
)

sources += gnome.compile_resources(
  'cc-' + cappletname + '-resources',
  cappletname + '.gresource.xml',
  c_name: 'cc_' + cappletname,
  dependencies: resource_data,
  export: true
)

deps = common_deps + [
  cups_dep,
  m_dep,
  polkit_gobject_dep,
  dependency('smbclient'),
  curl_dep,
  avahi_glib_dep,
  avahi_client_dep
]

printers_panel_lib = static_library(
  cappletname,
  sources: sources,
  include_directories: [top_inc, common_inc, shell_inc],
  dependencies: deps,
  c_args: cflags + cups_cflags,
)
panels_libs += [ printers_panel_lib ]

subdir('icons')
