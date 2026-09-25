#include "capture_client.hpp"
#include "access_portal.hpp"
#include "file_chooser_portal.hpp"
#include "screenshot_portal.hpp"
#include "screencast_portal.hpp"
#include "settings_portal.hpp"

#include <QApplication>
#include <QDBusInterface>
#include <QDBusMetaType>
#include <QDBusReply>
#include <QDBusVariant>
#include <QMap>
#include <QTimer>

#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace {
bool publishActivationEnvironment() {
  qDBusRegisterMetaType<QMap<QString, QString>>();
  QMap<QString, QString> environment;
  QStringList assignments;
  for (const char* name : {"WAYLAND_DISPLAY", "DISPLAY", "XDG_CURRENT_DESKTOP",
                           "XDG_SESSION_DESKTOP", "XDG_SESSION_TYPE", "XDG_DATA_DIRS",
                           "NIX_XDG_DESKTOP_PORTAL_DIR"}) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') continue;
    const QString key = QString::fromLatin1(name);
    const QString text = QString::fromLocal8Bit(value);
    environment.insert(key, text);
    assignments.push_back(key + QLatin1Char('=') + text);
  }

  QDBusInterface bus(QStringLiteral("org.freedesktop.DBus"),
                     QStringLiteral("/org/freedesktop/DBus"),
                     QStringLiteral("org.freedesktop.DBus"),
                     QDBusConnection::sessionBus());
  const QDBusReply<void> busReply = bus.call(
      QStringLiteral("UpdateActivationEnvironment"), QVariant::fromValue(environment));
  if (!busReply.isValid()) return false;

  QDBusInterface systemd(QStringLiteral("org.freedesktop.systemd1"),
                         QStringLiteral("/org/freedesktop/systemd1"),
                         QStringLiteral("org.freedesktop.systemd1.Manager"),
                         QDBusConnection::sessionBus());
  const QDBusReply<void> systemdReply = systemd.call(QStringLiteral("SetEnvironment"),
                                                     assignments);
  if (!systemdReply.isValid()) return false;
  systemd.call(QStringLiteral("ResetFailedUnit"),
               QStringLiteral("xdg-desktop-portal-gtk.service"));
  return true;
}

void refreshPublicPortal() {
  QDBusInterface properties(QStringLiteral("org.freedesktop.portal.Desktop"),
                            QStringLiteral("/org/freedesktop/portal/desktop"),
                            QStringLiteral("org.freedesktop.DBus.Properties"),
                            QDBusConnection::sessionBus());
  const QDBusReply<QDBusVariant> sources = properties.call(
      QStringLiteral("Get"), QStringLiteral("org.freedesktop.portal.ScreenCast"),
      QStringLiteral("AvailableSourceTypes"));
  if (sources.isValid() && sources.value().variant().toUInt() == 3U) return;

  QDBusInterface systemd(QStringLiteral("org.freedesktop.systemd1"),
                         QStringLiteral("/org/freedesktop/systemd1"),
                         QStringLiteral("org.freedesktop.systemd1.Manager"),
                         QDBusConnection::sessionBus());
  systemd.asyncCall(QStringLiteral("RestartUnit"), QStringLiteral("xdg-desktop-portal.service"),
                    QStringLiteral("replace"));
}
}

int main(int argc, char** argv) {
  QString probeMode;
  if (argc == 2 && std::string_view(argv[1]).starts_with("--file-chooser-probe")) {
    const std::string_view argument(argv[1]);
    probeMode = argument.contains('=')
                    ? QString::fromUtf8(argument.substr(argument.find('=') + 1))
                    : QStringLiteral("open");
  }
  int capability = -1;
  for (int index = probeMode.isEmpty() ? 1 : argc; index + 1 < argc; index += 2) {
    if (std::string_view(argv[index]) == "--capability-fd") capability = std::atoi(argv[index + 1]);
    else return EXIT_FAILURE;
  }
  if (capability < 0 && probeMode.isEmpty()) return EXIT_FAILURE;

  QApplication application(argc, argv);
  application.setApplicationName(QStringLiteral("xdg-desktop-portal-zwwm"));
  application.setQuitOnLastWindowClosed(false);
  if (!probeMode.isEmpty()) return runFileChooserProbe(probeMode);
  if (!publishActivationEnvironment()) {
    std::fprintf(stderr, "xdg-desktop-portal-zwwm: could not publish the desktop activation environment\n");
    return EXIT_FAILURE;
  }

  CaptureClient capture(capability);
  if (!capture.start()) {
    std::fprintf(stderr, "xdg-desktop-portal-zwwm: %s\n", capture.errorString().toLocal8Bit().constData());
    return EXIT_FAILURE;
  }
  ScreenshotPortal portal(&capture);
  ScreenCastPortal screencast(&capture, &portal);
  AccessPortal access(&portal);
  FileChooserPortal fileChooser(&portal);
  SettingsAdaptor settings(&portal);
  if (!portal.start()) {
    std::fprintf(stderr, "xdg-desktop-portal-zwwm: %s\n", portal.errorString().toLocal8Bit().constData());
    return EXIT_FAILURE;
  }
  QTimer::singleShot(0, refreshPublicPortal);
  QObject::connect(&capture, &CaptureClient::disconnected, &application, &QCoreApplication::quit);
  return application.exec();
}
