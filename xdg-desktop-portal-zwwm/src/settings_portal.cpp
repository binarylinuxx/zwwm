#include "settings_portal.hpp"

#include "screenshot_portal.hpp"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>

namespace {
constexpr auto kGtkService = "org.freedesktop.impl.portal.desktop.gtk";
constexpr auto kPortalPath = "/org/freedesktop/portal/desktop";
constexpr auto kSettingsInterface = "org.freedesktop.impl.portal.Settings";
}

SettingsAdaptor::SettingsAdaptor(ScreenshotPortal* host) : QDBusAbstractAdaptor(host) {
  QDBusConnection::sessionBus().connect(
      QString::fromLatin1(kGtkService), QString::fromLatin1(kPortalPath),
      QString::fromLatin1(kSettingsInterface), QStringLiteral("SettingChanged"), this,
      SLOT(forwardSettingChanged(QString,QString,QDBusVariant)));
}

void SettingsAdaptor::forwardCall(const QString& member, const QVariantList& arguments) {
  auto* host = static_cast<ScreenshotPortal*>(parent());
  const QDBusMessage call = host->message();
  host->setDelayedReply(true);

  QDBusMessage forwarded = QDBusMessage::createMethodCall(
      QString::fromLatin1(kGtkService), QString::fromLatin1(kPortalPath),
      QString::fromLatin1(kSettingsInterface), member);
  forwarded.setArguments(arguments);
  auto* watcher = new QDBusPendingCallWatcher(
      QDBusConnection::sessionBus().asyncCall(forwarded), this);
  connect(watcher, &QDBusPendingCallWatcher::finished, this,
          [call](QDBusPendingCallWatcher* finished) {
            const QDBusMessage reply = finished->reply();
            if (reply.type() == QDBusMessage::ErrorMessage) {
              QDBusConnection::sessionBus().send(
                  call.createErrorReply(reply.errorName(), reply.errorMessage()));
            } else {
              QDBusConnection::sessionBus().send(call.createReply(reply.arguments()));
            }
            finished->deleteLater();
          });
}

void SettingsAdaptor::ReadAll(const QStringList& namespaces) {
  forwardCall(QStringLiteral("ReadAll"), {namespaces});
}

void SettingsAdaptor::Read(const QString& nameSpace, const QString& key) {
  forwardCall(QStringLiteral("Read"), {nameSpace, key});
}

void SettingsAdaptor::forwardSettingChanged(const QString& nameSpace, const QString& key,
                                            const QDBusVariant& value) {
  emit SettingChanged(nameSpace, key, value);
}
