#include "screenshot_portal.hpp"

#include "capture_client.hpp"
#include "image_writer.hpp"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusError>
#include <QDBusReply>
#include <QDBusServiceWatcher>
#include <QDialog>
#include <QDialogButtonBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QRadioButton>
#include <QRubberBand>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>

#include <zwayland/client/core.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>

namespace {

namespace protocol = zwayland::generated;

constexpr auto kService = "org.freedesktop.impl.portal.desktop.zwwm";
constexpr auto kObjectPath = "/org/freedesktop/portal/desktop";
constexpr auto kFrontendService = "org.freedesktop.portal.Desktop";

class RegionPreview final : public QWidget {
 public:
  explicit RegionPreview(const QImage& image, QWidget* parent = nullptr) : QWidget(parent), image_(image) {
    setMinimumSize(480, 300);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  }

  std::function<void(const QRect&)> selected;

  QRect selection() const { return previewSelection(); }
  QRect previewRect() const { return fittedRect(); }

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this);
    painter.fillRect(rect(), QColor(25, 28, 34));
    painter.drawImage(fittedRect(), image_);
  }
  void mousePressEvent(QMouseEvent* event) override {
    if (event->button() != Qt::LeftButton || !fittedRect().contains(event->position().toPoint())) return;
    const std::optional<QPoint> point = clamp(event->position().toPoint());
    if (!point.has_value()) return;
    origin_ = *point;
    current_ = *point;
    if (band_ == nullptr) band_ = new QRubberBand(QRubberBand::Rectangle, this);
    updateBand(); band_->show();
  }
  void mouseMoveEvent(QMouseEvent* event) override {
    if (!origin_.has_value()) return;
    const std::optional<QPoint> point = clamp(event->position().toPoint());
    if (!point.has_value()) return;
    current_ = *point; updateBand();
  }
  void mouseReleaseEvent(QMouseEvent* event) override {
    if (!origin_.has_value() || event->button() != Qt::LeftButton) return;
    const std::optional<QPoint> point = clamp(event->position().toPoint());
    if (!point.has_value()) return;
    current_ = *point;
    const QRect selection = previewSelection();
    origin_.reset();
    if (band_ != nullptr) band_->hide();
    if (selection.width() > 0 && selection.height() > 0 && selected) selected(selection);
  }

 private:
  QRect fittedRect() const {
    if (image_.isNull()) return {};
    QSize size = image_.size();
    size.scale(contentsRect().size(), Qt::KeepAspectRatio);
    return {contentsRect().center() - QPoint(size.width() / 2, size.height() / 2), size};
  }
  std::optional<QPoint> clamp(QPoint point) const {
    const QRect area = fittedRect();
    if (area.isEmpty()) return std::nullopt;
    return QPoint{std::clamp(point.x(), area.left(), area.x() + area.width()),
                  std::clamp(point.y(), area.top(), area.y() + area.height())};
  }
  QRect previewSelection() const {
    const int left = std::min(origin_->x(), current_.x());
    const int top = std::min(origin_->y(), current_.y());
    return {left, top, std::abs(current_.x() - origin_->x()), std::abs(current_.y() - origin_->y())};
  }
  void updateBand() { if (band_ != nullptr) band_->setGeometry(previewSelection()); }

  QImage image_;
  std::optional<QPoint> origin_;
  QPoint current_;
  QRubberBand* band_ = nullptr;
};

QRect logicalRegion(const QRect& selection, const QRect& preview, const QSize& logical, int transform) {
  if (selection.isEmpty() || preview.isEmpty() || logical.isEmpty()) return {};
  const double x0 = static_cast<double>(selection.x() - preview.x()) / preview.width();
  const double y0 = static_cast<double>(selection.y() - preview.y()) / preview.height();
  const double x1 = static_cast<double>(selection.x() - preview.x() + selection.width()) / preview.width();
  const double y1 = static_cast<double>(selection.y() - preview.y() + selection.height()) / preview.height();
  double left = x0, top = y0, right = x1, bottom = y1;
  if (transform == protocol::WL_OUTPUT_TRANSFORM_90) { left = y0; right = y1; top = 1.0 - x1; bottom = 1.0 - x0; }
  else if (transform == protocol::WL_OUTPUT_TRANSFORM_180) { left = 1.0 - x1; right = 1.0 - x0; top = 1.0 - y1; bottom = 1.0 - y0; }
  else if (transform == protocol::WL_OUTPUT_TRANSFORM_270) { left = 1.0 - y1; right = 1.0 - y0; top = x0; bottom = x1; }
  const int x = std::clamp(static_cast<int>(std::floor(left * logical.width())), 0, logical.width());
  const int y = std::clamp(static_cast<int>(std::floor(top * logical.height())), 0, logical.height());
  const int r = std::clamp(static_cast<int>(std::ceil(right * logical.width())), x, logical.width());
  const int b = std::clamp(static_cast<int>(std::ceil(bottom * logical.height())), y, logical.height());
  return {x, y, r - x, b - y};
}

}  // namespace

ScreenshotAdaptor::ScreenshotAdaptor(ScreenshotPortal* parent) : QDBusAbstractAdaptor(parent) {}

void ScreenshotAdaptor::Screenshot(const QDBusObjectPath& handle, const QString& appId,
                                   const QString& parentWindow, const QVariantMap& options) {
  auto* portal = static_cast<ScreenshotPortal*>(parent());
  const QDBusMessage call = portal->message();
  portal->setDelayedReply(true);
  if (!portal->authorizedCaller(call.service())) {
    portal->connection().send(call.createErrorReply(QDBusError::AccessDenied,
                                                    QStringLiteral("caller is not xdg-desktop-portal")));
    return;
  }
  portal->beginScreenshot(call, handle, appId, parentWindow, options);
}

void ScreenshotAdaptor::PickColor(const QDBusObjectPath&, const QString&, const QString&,
                                  const QVariantMap&) {
  auto* portal = static_cast<ScreenshotPortal*>(parent());
  const QDBusMessage call = portal->message();
  portal->setDelayedReply(true);
  if (!portal->authorizedCaller(call.service())) {
    portal->connection().send(call.createErrorReply(QDBusError::AccessDenied,
                                                    QStringLiteral("caller is not xdg-desktop-portal")));
    return;
  }
  portal->replyUnsupported(call);
}

ScreenshotPortal::ScreenshotPortal(CaptureClient* capture, QObject* parent)
    : QObject(parent), capture_(capture) {
  new ScreenshotAdaptor(this);
  connect(capture_, &CaptureClient::captured, this, &ScreenshotPortal::captureReady);
  connect(capture_, &CaptureClient::failed, this, [this](quint64 id) { complete(id, 2); });
  connect(capture_, &CaptureClient::disconnected, this, [this] {
    while (!pending_.empty()) complete(pending_.begin()->first, 2);
  });
}

bool ScreenshotPortal::start() {
  auto bus = QDBusConnection::sessionBus();
  if (!bus.isConnected()) {
    error_ = bus.lastError().message();
    return false;
  }
  if (!bus.registerService(QString::fromLatin1(kService))) {
    error_ = bus.lastError().message();
    return false;
  }
  if (!bus.registerObject(QString::fromLatin1(kObjectPath), this, QDBusConnection::ExportAdaptors)) {
    error_ = bus.lastError().message();
    bus.unregisterService(QString::fromLatin1(kService));
    return false;
  }
  return true;
}

QString ScreenshotPortal::errorString() const { return error_; }

bool ScreenshotPortal::authorizedCaller(const QString& sender) const {
  auto* interface = QDBusConnection::sessionBus().interface();
  if (interface == nullptr) return false;
  const QDBusReply<QString> owner = interface->serviceOwner(QString::fromLatin1(kFrontendService));
  return owner.isValid() && !owner.value().isEmpty() && owner.value() == sender;
}

void ScreenshotPortal::beginScreenshot(const QDBusMessage& call, const QDBusObjectPath& handle,
                                       const QString& appId, const QString& parentWindow,
                                       const QVariantMap& options) {
  const QString path = handle.path();
  if (path.isEmpty() || path == QStringLiteral("/") ||
      QDBusConnection::sessionBus().objectRegisteredAt(path) != nullptr) {
    sendReply(call, 2);
    return;
  }
  auto pending = std::make_unique<Pending>();
  pending->call = call;
  pending->handle = path;
  pending->appId = appId;
  pending->parentWindow = parentWindow;
  pending->interactive = options.value(QStringLiteral("interactive"), false).toBool();
  pending->modal = options.value(QStringLiteral("modal"), true).toBool();
  pending->request = new RequestObject(this);
  pending->callerWatcher = new QDBusServiceWatcher(call.service(), QDBusConnection::sessionBus(),
      QDBusServiceWatcher::WatchForUnregistration, this);
  if (!QDBusConnection::sessionBus().registerObject(path, pending->request,
                                                     QDBusConnection::ExportAllSlots)) {
    pending->request->deleteLater();
    sendReply(call, 2);
    return;
  }
  const quint64 id = nextId_++;
  connect(pending->request, &RequestObject::closeRequested, this, [this, id] { complete(id, 1); });
  connect(pending->callerWatcher, &QDBusServiceWatcher::serviceUnregistered, this,
          [this, id] { complete(id, 2); });
  pending_.emplace(id, std::move(pending));
  if (options.value(QStringLiteral("interactive"), false).toBool()) showSourceChooser(id);
  else if (!capture_->request(id)) complete(id, 2);
}

void ScreenshotPortal::replyUnsupported(const QDBusMessage& call) { sendReply(call, 2); }

void ScreenshotPortal::captureReady(quint64 id, const QImage& image) {
  const auto found = pending_.find(id);
  if (found == pending_.end()) return;
  found->second->image = image;
  switch (found->second->stage) {
    case Pending::Stage::regionPreview: showRegionSelector(id); return;
    case Pending::Stage::fullCapture:
    case Pending::Stage::windowCapture:
    case Pending::Stage::regionCapture: break;
    case Pending::Stage::confirmation:
    case Pending::Stage::saving: complete(id, 2); return;
  }
  if (found->second->interactive) { showConfirmation(id); return; }
  QString error;
  const QString path = writeTemporaryScreenshot(image, &error);
  if (path.isEmpty()) complete(id, 2);
  else complete(id, 0, {{QStringLiteral("uri"), QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded)}});
}

void ScreenshotPortal::showSourceChooser(quint64 id) {
  const auto found = pending_.find(id);
  if (found == pending_.end()) return;
  auto& pending = *found->second;
  auto* dialog = new QDialog;
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setWindowTitle(QStringLiteral("Choose screenshot source"));
  dialog->setWindowModality(pending.modal ? Qt::ApplicationModal : Qt::NonModal);
  auto* layout = new QVBoxLayout(dialog);
  const QString application = pending.appId.isEmpty() ? QStringLiteral("An application") : pending.appId;
  layout->addWidget(new QLabel(QStringLiteral("%1 wants to save a screenshot.").arg(application), dialog));
  auto* full = new QRadioButton(QStringLiteral("Full Output"), dialog);
  auto* window = new QRadioButton(QStringLiteral("Window"), dialog);
  auto* region = new QRadioButton(QStringLiteral("Region"), dialog);
  full->setChecked(true);
  layout->addWidget(full); layout->addWidget(window);
  auto* windows = new QComboBox(dialog);
  const auto populateWindows = [this, windows, window, full] {
    const quint64 current = windows->currentData().toULongLong();
    windows->clear();
    int selected = -1;
    for (const auto& item : capture_->toplevels()) {
      const QString title = item.title.isEmpty() ? QStringLiteral("Untitled window") : item.title;
      const QString app = item.appId.isEmpty() ? QStringLiteral("unknown app") : item.appId;
      windows->addItem(QStringLiteral("%1 — %2").arg(title, app), QVariant::fromValue(item.id));
      if (item.id == current) selected = windows->count() - 1;
    }
    if (selected >= 0) windows->setCurrentIndex(selected);
    window->setEnabled(windows->count() != 0);
    if (!window->isEnabled() && window->isChecked()) full->setChecked(true);
  };
  for (const auto& item : capture_->toplevels()) {
    const QString title = item.title.isEmpty() ? QStringLiteral("Untitled window") : item.title;
    const QString app = item.appId.isEmpty() ? QStringLiteral("unknown app") : item.appId;
    windows->addItem(QStringLiteral("%1 — %2").arg(title, app), QVariant::fromValue(item.id));
  }
  windows->setEnabled(false);
  window->setEnabled(windows->count() != 0);
  connect(window, &QRadioButton::toggled, windows, &QComboBox::setEnabled);
  connect(capture_, &CaptureClient::toplevelsChanged, dialog, populateWindows);
  layout->addWidget(windows); layout->addWidget(region);
  layout->addWidget(new QLabel(QStringLiteral("Region selection is made on an aspect-correct output preview."), dialog));
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
  connect(dialog, &QDialog::accepted, this, [this, id, full, window, windows] {
    const auto active = pending_.find(id);
    if (active == pending_.end()) return;
    auto& request = *active->second;
    if (full->isChecked()) {
      request.stage = Pending::Stage::fullCapture;
      if (!capture_->request(id)) complete(id, 2);
    } else if (window->isChecked()) {
      request.stage = Pending::Stage::windowCapture;
      if (!capture_->requestToplevel(id, windows->currentData().toULongLong())) complete(id, 2);
    } else {
      request.stage = Pending::Stage::regionPreview;
      if (!capture_->request(id)) complete(id, 2);
    }
  });
  connect(dialog, &QDialog::rejected, this, [this, id] { complete(id, 1); });
  pending.confirmation = dialog;
  applyParent(dialog, pending);
  dialog->open();
}

void ScreenshotPortal::showRegionSelector(quint64 id) {
  const auto found = pending_.find(id);
  if (found == pending_.end()) return;
  auto& pending = *found->second;
  auto* dialog = new QDialog;
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setWindowTitle(QStringLiteral("Select screenshot region"));
  dialog->setWindowModality(pending.modal ? Qt::ApplicationModal : Qt::NonModal);
  auto* layout = new QVBoxLayout(dialog);
  layout->addWidget(new QLabel(QStringLiteral("Drag a rectangle over the output preview."), dialog));
  auto* preview = new RegionPreview(pending.image, dialog);
  layout->addWidget(preview, 1);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, dialog);
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
  preview->selected = [this, id, dialog, preview](const QRect& selection) {
    const auto active = pending_.find(id);
    if (active == pending_.end()) return;
    const QRect region = logicalRegion(selection, preview->previewRect(), capture_->outputLogicalSize(), capture_->outputTransform());
    if (region.isEmpty()) return;
    active->second->stage = Pending::Stage::regionCapture;
    dialog->accept();
    if (!capture_->requestRegion(id, region)) complete(id, 2);
  };
  connect(dialog, &QDialog::rejected, this, [this, id] { complete(id, 1); });
  pending.confirmation = dialog;
  applyParent(dialog, pending);
  dialog->resize(760, 600);
  dialog->open();
}

void ScreenshotPortal::showConfirmation(quint64 id) {
  const auto found = pending_.find(id);
  if (found == pending_.end()) return;
  auto& pending = *found->second;
  pending.stage = Pending::Stage::confirmation;
  auto* dialog = new QDialog;
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setWindowTitle(QStringLiteral("Screenshot"));
  dialog->setWindowModality(pending.modal ? Qt::ApplicationModal : Qt::NonModal);
  auto* layout = new QVBoxLayout(dialog);
  const QString application = pending.appId.isEmpty() ? QStringLiteral("An application") : pending.appId;
  layout->addWidget(new QLabel(QStringLiteral("%1 requested a screenshot of the current output.").arg(application), dialog));
  auto* preview = new QLabel(dialog);
  preview->setAlignment(Qt::AlignCenter);
  preview->setPixmap(QPixmap::fromImage(pending.image).scaled(720, 450, Qt::KeepAspectRatio,
                                                               Qt::SmoothTransformation));
  layout->addWidget(preview);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, dialog);
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
  connect(dialog, &QDialog::accepted, this, [this, id] { showFileDialog(id); });
  connect(dialog, &QDialog::rejected, this, [this, id] { complete(id, 1); });
  pending.confirmation = dialog;
  applyParent(dialog, pending);
  dialog->open();
}

void ScreenshotPortal::showFileDialog(quint64 id) {
  const auto found = pending_.find(id);
  if (found == pending_.end()) return;
  auto& pending = *found->second;
  pending.stage = Pending::Stage::saving;
  auto* dialog = new QFileDialog;
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setAcceptMode(QFileDialog::AcceptSave);
  dialog->setFileMode(QFileDialog::AnyFile);
  dialog->setDefaultSuffix(QStringLiteral("png"));
  dialog->setNameFilters(screenshotNameFilters());
  dialog->selectNameFilter(screenshotNameFilters().front());
  dialog->setWindowTitle(QStringLiteral("Save Screenshot"));
  dialog->setWindowModality(pending.modal ? Qt::ApplicationModal : Qt::NonModal);
  QString directory = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
  if (directory.isEmpty()) directory = QDir::homePath();
  dialog->selectFile(QDir(directory).filePath(QStringLiteral("Screenshot.png")));
  connect(dialog, &QFileDialog::accepted, this, [this, id, dialog] {
    const auto found = pending_.find(id);
    if (found == pending_.end()) return;
    const QStringList files = dialog->selectedFiles();
    if (files.size() != 1) {
      complete(id, 1);
      return;
    }
    QString error;
    if (!writeScreenshot(files.front(), found->second->image, &error)) complete(id, 2);
    else complete(id, 0, {{QStringLiteral("uri"),
                            QUrl::fromLocalFile(files.front()).toString(QUrl::FullyEncoded)}});
  });
  connect(dialog, &QFileDialog::rejected, this, [this, id] { complete(id, 1); });
  pending.fileDialog = dialog;
  applyParent(dialog, pending);
  dialog->open();
}

void ScreenshotPortal::complete(quint64 id, uint response, const QVariantMap& results) {
  auto found = pending_.find(id);
  if (found == pending_.end()) return;
  auto pending = std::move(found->second);
  pending_.erase(found);
  capture_->cancel(id);
  QDBusConnection::sessionBus().unregisterObject(pending->handle);
  if (pending->confirmation != nullptr) pending->confirmation->reject();
  if (pending->fileDialog != nullptr) pending->fileDialog->reject();
  if (pending->request != nullptr) pending->request->deleteLater();
  if (pending->callerWatcher != nullptr) pending->callerWatcher->deleteLater();
  if (pending->foreignParent != nullptr) pending->foreignParent->deleteLater();
  sendReply(pending->call, response, results);
}

void ScreenshotPortal::sendReply(const QDBusMessage& call, uint response, const QVariantMap& results) const {
  QDBusConnection::sessionBus().send(call.createReply({QVariant::fromValue(response), results}));
}

void ScreenshotPortal::applyParent(QWidget* widget, Pending& pending) {
  if (!pending.parentWindow.startsWith(QStringLiteral("x11:"))) return;
  bool valid = false;
  const qulonglong id = pending.parentWindow.sliced(4).toULongLong(&valid, 16);
  if (!valid || id == 0) return;
  widget->winId();
  QWindow* foreign = QWindow::fromWinId(static_cast<WId>(id));
  if (foreign == nullptr || widget->windowHandle() == nullptr) {
    delete foreign;
    return;
  }
  widget->windowHandle()->setTransientParent(foreign);
  pending.foreignParent = foreign;
}
