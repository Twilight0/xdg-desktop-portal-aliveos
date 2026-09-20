libexecdir_conf = configuration_data()
libexecdir_conf.set('libexecdir', prefix / libexecdir)

# Portal
install_data(
  [ 'aliveos.portal', 'aliveos-gnome-keyring.portal' ],
  install_dir: datadir / 'xdg-desktop-portal' / 'portals'
)

# Desktop portal configs
install_data(
  [ 'portals.conf', 'qtile-portals.conf' ],
  install_dir: datadir / 'xdg-desktop-portal'
)

# GSettings schema
install_data(
  'org.aliveos.portal.filechooser.gschema.xml',
  install_dir: datadir / 'glib-2.0' / 'schemas'
)

# dbus service
service_name = 'org.freedesktop.impl.portal.desktop.aliveos'
configure_file(
  input: service_name + '.service.in',
  output: service_name + '.service',
  configuration: libexecdir_conf,
  install_dir: dbus_service_dir,
)

filename = 'xdg-desktop-portal-aliveos'
# systemd unit
configure_file(
  input: filename + '.service.in',
  output: filename + '.service',
  configuration: libexecdir_conf,
  install_dir: systemduserunitdir,
)

# Desktop file
configure_file(
  input: filename + '.desktop.in',
  output: filename + '.desktop',
  configuration: libexecdir_conf,
  install: true,
  install_dir: datadir / 'applications',
)
