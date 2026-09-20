#pragma once

#include <QDBusAbstractAdaptor>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QObject>
#include <QPointer>
#include <QVariantMap>

#include <memory>
#include <unordered_map>

class QDialog;
class QDBusServiceWatcher;
class ScreenshotPortal;
class RequestObject;

class AccessPortal;

class AccessAdaptor : public QDBusAbstractAdaptor {
  Q_OBJECT
  Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.Access")
  Q_CLASSINFO("D-Bus Introspection", "  <interface name=\"org.freedesktop.impl.portal.Access\">\n"
      "    <method name=\"AccessDialog\"><arg direction=\"in\" type=\"o\"/><arg direction=\"in\" type=\"s\"/><arg direction=\"in\" type=\"s\"/><arg direction=\"in\" type=\"s\"/><arg direction=\"in\" type=\"s\"/><arg direction=\"in\" type=\"s\"/><arg direction=\"in\" type=\"a{sv}\"/><arg direction=\"out\" type=\"u\"/><arg direction=\"out\" type=\"a{sv}\"/></method>\n"
      "  </interface>\n")

 public:
  AccessAdaptor(QObject* host, AccessPortal* portal);

 public slots:
  void AccessDialog(const QDBusObjectPath& handle, const QString& appId, const QString& parentWindow,
                    const QString& title, const QString& subtitle, const QString& body,
                    const QVariantMap& options);

 private:
  AccessPortal* portal_ = nullptr;
};

class AccessPortal : public QObject {
  Q_OBJECT

 public:
  explicit AccessPortal(ScreenshotPortal* host, QObject* parent = nullptr);
  void show(const QDBusMessage& call, const QDBusObjectPath& handle, const QString& appId,
            const QString& title, const QString& subtitle, const QString& body,
            const QVariantMap& options);

 private:
  struct Pending {
    QDBusMessage call;
    QString path;
    RequestObject* request = nullptr;
    QDBusServiceWatcher* watcher = nullptr;
    QPointer<QDialog> dialog;
  };

  void complete(quint64 id, uint response);

  ScreenshotPortal* host_ = nullptr;
  std::unordered_map<quint64, std::unique_ptr<Pending>> pending_;
  quint64 nextId_ = 1;
};
