#pragma once

#include <QDBusAbstractAdaptor>
#include <QDBusVariant>

class ScreenshotPortal;

class SettingsAdaptor : public QDBusAbstractAdaptor {
  Q_OBJECT
  Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.Settings")
  Q_CLASSINFO("D-Bus Introspection", "  <interface name=\"org.freedesktop.impl.portal.Settings\">\n"
      "    <method name=\"ReadAll\"><arg direction=\"in\" type=\"as\"/><arg direction=\"out\" type=\"a{sa{sv}}\"/></method>\n"
      "    <method name=\"Read\"><arg direction=\"in\" type=\"s\"/><arg direction=\"in\" type=\"s\"/><arg direction=\"out\" type=\"v\"/></method>\n"
      "    <signal name=\"SettingChanged\"><arg type=\"s\"/><arg type=\"s\"/><arg type=\"v\"/></signal>\n"
      "    <property access=\"read\" type=\"u\" name=\"version\"/>\n"
      "  </interface>\n")
  Q_PROPERTY(uint version READ version CONSTANT)

 public:
  explicit SettingsAdaptor(ScreenshotPortal* host);
  uint version() const { return 1; }

 public slots:
  void ReadAll(const QStringList& namespaces);
  void Read(const QString& nameSpace, const QString& key);

 signals:
  void SettingChanged(const QString& nameSpace, const QString& key, const QDBusVariant& value);

 private:
  void forwardCall(const QString& member, const QVariantList& arguments);

 private slots:
  void forwardSettingChanged(const QString& nameSpace, const QString& key,
                             const QDBusVariant& value);
};
