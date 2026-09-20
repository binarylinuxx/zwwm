#pragma once

#include <QImage>
#include <QList>
#include <QObject>
#include <QPoint>
#include <QRect>
#include <QSize>

#include <memory>

struct ToplevelSource {
  quint64 id = 0;
  QString title;
  QString appId;
  QRect geometry;
  quint32 state = 0;
  bool hasOutput = false;
};

struct MonitorSource {
  quint64 id = 0;
  QString name;
  QString description;
  QPoint position;
  QSize logicalSize;
};

class CaptureClient : public QObject {
  Q_OBJECT

 public:
  struct Impl;

  explicit CaptureClient(int capabilityFd, QObject* parent = nullptr);
  ~CaptureClient() override;

  bool start();
  bool request(quint64 requestId, bool overlayCursor = false);
  bool requestOutput(quint64 requestId, quint64 outputId, bool overlayCursor = false);
  bool requestRegion(quint64 requestId, const QRect& region, bool overlayCursor = false);
  bool requestToplevel(quint64 requestId, quint64 toplevelId, bool overlayCursor = false);
  void cancel(quint64 requestId);
  QList<ToplevelSource> toplevels() const;
  QList<MonitorSource> monitors() const;
  QSize outputLogicalSize() const;
  QSize outputLogicalSize(quint64 outputId) const;
  QPoint outputPosition(quint64 outputId) const;
  int outputTransform() const;
  quint32 outputRefreshMillihz() const;
  quint32 cursorModes() const;
  QString errorString() const;

 signals:
  void captured(quint64 requestId, const QImage& image, quint32 flags,
                quint64 seconds, quint32 nanoseconds);
  void failed(quint64 requestId);
  void toplevelsChanged();
  void presented(quint64 seconds, quint32 nanoseconds);
  void disconnected();

 private:
  std::unique_ptr<Impl> impl_;
};
