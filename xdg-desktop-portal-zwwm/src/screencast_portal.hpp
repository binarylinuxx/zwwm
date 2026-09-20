#pragma once

#include <QDBusAbstractAdaptor>
#include <QDBusArgument>
#include <QDBusContext>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QVariantMap>

#include <memory>
#include <functional>
#include <map>
#include <unordered_map>

#include "pipewire_stream.hpp"

class CaptureClient;
class RequestObject;
class ScreenshotPortal;
class QDBusServiceWatcher;
class QDialog;

struct IntPair { qint32 first = 0; qint32 second = 0; };
Q_DECLARE_METATYPE(IntPair)
QDBusArgument& operator<<(QDBusArgument& argument, const IntPair& value);
const QDBusArgument& operator>>(const QDBusArgument& argument, IntPair& value);

struct StreamEntry { quint32 nodeId = 0; QVariantMap properties; };
Q_DECLARE_METATYPE(StreamEntry)
using StreamList = QList<StreamEntry>;
Q_DECLARE_METATYPE(StreamList)
QDBusArgument& operator<<(QDBusArgument& argument, const StreamEntry& value);
const QDBusArgument& operator>>(const QDBusArgument& argument, StreamEntry& value);

class ScreenCastSessionObject : public QObject {
  Q_OBJECT
  Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.Session")
  Q_PROPERTY(uint version READ version CONSTANT)
 public:
  explicit ScreenCastSessionObject(QObject* parent = nullptr) : QObject(parent) {}
  uint version() const { return 1; }
  std::function<void()> close;
 public slots:
  void Close() { if (close) close(); }
 signals:
  void Closed();
};

class ScreenCastPortal;
class ScreenCastAdaptor : public QDBusAbstractAdaptor {
  Q_OBJECT
  Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.ScreenCast")
  Q_CLASSINFO("D-Bus Introspection", "  <interface name=\"org.freedesktop.impl.portal.ScreenCast\">\n"
      "    <method name=\"CreateSession\"><arg direction=\"in\" type=\"o\"/><arg direction=\"in\" type=\"o\"/><arg direction=\"in\" type=\"s\"/><arg direction=\"in\" type=\"a{sv}\"/><arg direction=\"out\" type=\"u\"/><arg direction=\"out\" type=\"a{sv}\"/></method>\n"
      "    <method name=\"SelectSources\"><arg direction=\"in\" type=\"o\"/><arg direction=\"in\" type=\"o\"/><arg direction=\"in\" type=\"s\"/><arg direction=\"in\" type=\"a{sv}\"/><arg direction=\"out\" type=\"u\"/><arg direction=\"out\" type=\"a{sv}\"/></method>\n"
      "    <method name=\"Start\"><arg direction=\"in\" type=\"o\"/><arg direction=\"in\" type=\"o\"/><arg direction=\"in\" type=\"s\"/><arg direction=\"in\" type=\"s\"/><arg direction=\"in\" type=\"a{sv}\"/><arg direction=\"out\" type=\"u\"/><arg direction=\"out\" type=\"a{sv}\"/></method>\n"
      "    <property access=\"read\" type=\"u\" name=\"AvailableSourceTypes\"/><property access=\"read\" type=\"u\" name=\"AvailableCursorModes\"/><property access=\"read\" type=\"u\" name=\"version\"/>\n"
      "  </interface>\n")
  Q_PROPERTY(uint AvailableSourceTypes READ availableSourceTypes CONSTANT)
  Q_PROPERTY(uint AvailableCursorModes READ availableCursorModes CONSTANT)
  Q_PROPERTY(uint version READ version CONSTANT)
 public:
  ScreenCastAdaptor(QObject* dbusObject, ScreenCastPortal* portal);
  uint availableSourceTypes() const { return 3; }
  uint availableCursorModes() const;
  uint version() const { return 5; }
 public slots:
  void CreateSession(const QDBusObjectPath&, const QDBusObjectPath&, const QString&, const QVariantMap&);
  void SelectSources(const QDBusObjectPath&, const QDBusObjectPath&, const QString&, const QVariantMap&);
  void Start(const QDBusObjectPath&, const QDBusObjectPath&, const QString&, const QString&, const QVariantMap&);
 private:
  ScreenCastPortal* portal_ = nullptr;
};

class ScreenCastPortal : public QObject {
  Q_OBJECT
 public:
  ScreenCastPortal(CaptureClient* capture, ScreenshotPortal* dbusHost, QObject* parent = nullptr);
  ~ScreenCastPortal() override;
  bool authorized(const QString& sender) const;
  quint32 availableCursorModes() const;
  void createSession(const QDBusMessage&, const QDBusObjectPath&, const QDBusObjectPath&, const QString&, const QVariantMap&);
  void selectSources(const QDBusMessage&, const QDBusObjectPath&, const QDBusObjectPath&, const QString&, const QVariantMap&);
  void start(const QDBusMessage&, const QDBusObjectPath&, const QDBusObjectPath&, const QString&, const QString&, const QVariantMap&);

 private:
  enum class SessionStage { created, selected, starting, active };
  struct Session {
    QString path, owner, appId;
    ScreenCastSessionObject* object = nullptr;
    QDBusServiceWatcher* watcher = nullptr;
    SessionStage stage = SessionStage::created;
    quint32 types = 1;
    quint32 cursorMode = 1;
    quint32 persistMode = 0;
    quint64 outputId = 0;
    quint64 toplevelId = 0;
    QRect sourceGeometry;
    QImage firstFrame;
    QSize frameSize;
    quint64 firstSeconds = 0;
    quint32 firstNanoseconds = 0;
    quint64 captureId = 0;
    quint64 startRequest = 0;
    std::unique_ptr<PipeWireStream> stream;
    bool pipewireReady = false;
    bool presentationPending = false;
  };
  struct Request {
    QDBusMessage call;
    QString path, sessionPath;
    RequestObject* object = nullptr;
    QDBusServiceWatcher* watcher = nullptr;
    QPointer<QDialog> dialog;
  };

  quint64 addRequest(const QDBusMessage&, const QDBusObjectPath&, const QString& sessionPath);
  void completeRequest(quint64, quint32 response, const QVariantMap& results = {});
  void closeSession(const QString& path, bool notify);
  bool requestFrame(Session& session);
  void maybeCapture(Session& session);
  void captured(quint64 id, const QImage&, quint64 seconds, quint32 nanoseconds);
  void captureFailed(quint64 id);
  void beginPipeWire(Session& session, const QImage&, quint64 seconds, quint32 nanoseconds);
  void streamReady(const QString& sessionPath);

  CaptureClient* capture_ = nullptr;
  ScreenshotPortal* dbusHost_ = nullptr;
  std::map<QString, std::unique_ptr<Session>> sessions_;
  std::unordered_map<quint64, std::unique_ptr<Request>> requests_;
  quint64 nextRequest_ = 1;
  quint64 nextCapture_ = (quint64{1} << 63);
};
