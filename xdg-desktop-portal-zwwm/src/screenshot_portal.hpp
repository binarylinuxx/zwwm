#pragma once

#include <QDBusAbstractAdaptor>
#include <QDBusContext>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QVariantMap>

#include <memory>
#include <unordered_map>

class CaptureClient;
class QDialog;
class QFileDialog;
class QDBusServiceWatcher;
class QWindow;
class QWidget;

class RequestObject : public QObject {
  Q_OBJECT
  Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.Request")

 public:
  explicit RequestObject(QObject* parent = nullptr) : QObject(parent) {}

 public slots:
  void Close() { emit closeRequested(); }

 signals:
  void closeRequested();
};

class ScreenshotPortal;

class ScreenshotAdaptor : public QDBusAbstractAdaptor {
  Q_OBJECT
  Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.Screenshot")
  Q_CLASSINFO("D-Bus Introspection", "  <interface name=\"org.freedesktop.impl.portal.Screenshot\">\n"
      "    <method name=\"Screenshot\">\n"
      "      <arg direction=\"in\" type=\"o\" name=\"handle\"/>\n"
      "      <arg direction=\"in\" type=\"s\" name=\"app_id\"/>\n"
      "      <arg direction=\"in\" type=\"s\" name=\"parent_window\"/>\n"
      "      <arg direction=\"in\" type=\"a{sv}\" name=\"options\"/>\n"
      "      <arg direction=\"out\" type=\"u\" name=\"response\"/>\n"
      "      <arg direction=\"out\" type=\"a{sv}\" name=\"results\"/>\n"
      "    </method>\n"
      "    <method name=\"PickColor\">\n"
      "      <arg direction=\"in\" type=\"o\" name=\"handle\"/>\n"
      "      <arg direction=\"in\" type=\"s\" name=\"app_id\"/>\n"
      "      <arg direction=\"in\" type=\"s\" name=\"parent_window\"/>\n"
      "      <arg direction=\"in\" type=\"a{sv}\" name=\"options\"/>\n"
      "      <arg direction=\"out\" type=\"u\" name=\"response\"/>\n"
      "      <arg direction=\"out\" type=\"a{sv}\" name=\"results\"/>\n"
      "    </method>\n"
      "    <property access=\"read\" type=\"u\" name=\"version\"/>\n"
      "  </interface>\n")
  Q_PROPERTY(uint version READ version CONSTANT)

 public:
  explicit ScreenshotAdaptor(ScreenshotPortal* parent);
  uint version() const { return 2; }

 public slots:
  void Screenshot(const QDBusObjectPath& handle, const QString& appId, const QString& parentWindow,
                  const QVariantMap& options);
  void PickColor(const QDBusObjectPath& handle, const QString& appId, const QString& parentWindow,
                 const QVariantMap& options);
};

class ScreenshotPortal : public QObject, public QDBusContext {
  Q_OBJECT

 public:
  explicit ScreenshotPortal(CaptureClient* capture, QObject* parent = nullptr);
  bool start();
  QString errorString() const;
  bool authorizedCaller(const QString& sender) const;
  void beginScreenshot(const QDBusMessage& call, const QDBusObjectPath& handle, const QString& appId,
                       const QString& parentWindow, const QVariantMap& options);
  void replyUnsupported(const QDBusMessage& call);

 private:
  struct Pending {
    enum class Stage { fullCapture, windowCapture, regionPreview, regionCapture, confirmation, saving };
    QDBusMessage call;
    QString handle;
    QString appId;
    QString parentWindow;
    bool interactive = false;
    bool modal = true;
    RequestObject* request = nullptr;
    QDBusServiceWatcher* callerWatcher = nullptr;
    QPointer<QDialog> confirmation;
    QPointer<QFileDialog> fileDialog;
    QPointer<QWindow> foreignParent;
    QImage image;
    Stage stage = Stage::fullCapture;
  };

  void captureReady(quint64 id, const QImage& image);
  void showSourceChooser(quint64 id);
  void showRegionSelector(quint64 id);
  void showConfirmation(quint64 id);
  void showFileDialog(quint64 id);
  void complete(quint64 id, uint response, const QVariantMap& results = {});
  void sendReply(const QDBusMessage& call, uint response, const QVariantMap& results = {}) const;
  void applyParent(QWidget* widget, Pending& pending);

  CaptureClient* capture_ = nullptr;
  std::unordered_map<quint64, std::unique_ptr<Pending>> pending_;
  quint64 nextId_ = 1;
  QString error_;
};
