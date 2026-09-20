#pragma once

#include <QImage>
#include <QObject>
#include <QSize>

#include <memory>

class PipeWireStream : public QObject {
  Q_OBJECT

 public:
  explicit PipeWireStream(QSize size, QObject* parent = nullptr);
  ~PipeWireStream() override;

  bool start();
  bool submit(const QImage& image, quint64 monotonicSeconds, quint32 monotonicNanoseconds);
  quint32 nodeId() const;
  QString errorString() const;

 signals:
  void ready();
  void failed();
  void frameNeeded();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
