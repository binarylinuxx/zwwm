#pragma once

#include <QDBusAbstractAdaptor>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QObject>
#include <QPointer>
#include <QVariantMap>

#include <memory>
#include <unordered_map>

class QFileDialog;
class QDBusServiceWatcher;
class RequestObject;
class ScreenshotPortal;

int runFileChooserProbe(const QString& mode);

class FileChooserPortal;

class FileChooserAdaptor : public QDBusAbstractAdaptor {
  Q_OBJECT
  Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.FileChooser")
  Q_CLASSINFO("D-Bus Introspection", "  <interface name=\"org.freedesktop.impl.portal.FileChooser\">\n"
      "    <method name=\"OpenFile\"><arg direction=\"in\" type=\"o\"/><arg direction=\"in\" type=\"s\"/><arg direction=\"in\" type=\"s\"/><arg direction=\"in\" type=\"s\"/><arg direction=\"in\" type=\"a{sv}\"/><arg direction=\"out\" type=\"u\"/><arg direction=\"out\" type=\"a{sv}\"/></method>\n"
      "    <method name=\"SaveFile\"><arg direction=\"in\" type=\"o\"/><arg direction=\"in\" type=\"s\"/><arg direction=\"in\" type=\"s\"/><arg direction=\"in\" type=\"s\"/><arg direction=\"in\" type=\"a{sv}\"/><arg direction=\"out\" type=\"u\"/><arg direction=\"out\" type=\"a{sv}\"/></method>\n"
      "    <method name=\"SaveFiles\"><arg direction=\"in\" type=\"o\"/><arg direction=\"in\" type=\"s\"/><arg direction=\"in\" type=\"s\"/><arg direction=\"in\" type=\"s\"/><arg direction=\"in\" type=\"a{sv}\"/><arg direction=\"out\" type=\"u\"/><arg direction=\"out\" type=\"a{sv}\"/></method>\n"
      "    <property access=\"read\" type=\"u\" name=\"version\"/>\n"
      "  </interface>\n")
  Q_PROPERTY(uint version READ version CONSTANT)

 public:
  FileChooserAdaptor(ScreenshotPortal* host, FileChooserPortal* portal);
  uint version() const { return 4; }

 public slots:
  void OpenFile(const QDBusObjectPath& handle, const QString& appId, const QString& parentWindow,
                const QString& title, const QVariantMap& options);
  void SaveFile(const QDBusObjectPath& handle, const QString& appId, const QString& parentWindow,
                const QString& title, const QVariantMap& options);
  void SaveFiles(const QDBusObjectPath& handle, const QString& appId, const QString& parentWindow,
                 const QString& title, const QVariantMap& options);

 private:
  void begin(const QDBusObjectPath& handle, const QString& appId, const QString& parentWindow,
             const QString& title, const QVariantMap& options, bool save, bool saveMultiple);
  FileChooserPortal* portal_ = nullptr;
};

class FileChooserPortal : public QObject {
  Q_OBJECT

 public:
  explicit FileChooserPortal(ScreenshotPortal* host, QObject* parent = nullptr);
  void show(const QDBusMessage& call, const QDBusObjectPath& handle, const QString& title,
            const QVariantMap& options, bool save, bool saveMultiple);

 private:
  struct Pending {
    QDBusMessage call;
    QString path;
    RequestObject* request = nullptr;
    QDBusServiceWatcher* watcher = nullptr;
    QPointer<QFileDialog> dialog;
  };

  void complete(quint64 id, uint response);
  ScreenshotPortal* host_ = nullptr;
  std::unordered_map<quint64, std::unique_ptr<Pending>> pending_;
  quint64 nextId_ = 1;
};
