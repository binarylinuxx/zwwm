#include "access_portal.hpp"

#include "screenshot_portal.hpp"

#include <QDBusConnection>
#include <QDBusError>
#include <QDBusServiceWatcher>
#include <QDialog>
#include <QDialogButtonBox>
#include <QIcon>
#include <QLabel>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

AccessAdaptor::AccessAdaptor(QObject* host, AccessPortal* portal)
    : QDBusAbstractAdaptor(host), portal_(portal) {}

void AccessAdaptor::AccessDialog(const QDBusObjectPath& handle, const QString& appId,
                                 const QString& parentWindow, const QString& title,
                                 const QString& subtitle, const QString& body,
                                 const QVariantMap& options) {
  auto* host = static_cast<ScreenshotPortal*>(parent());
  const QDBusMessage call = host->message();
  host->setDelayedReply(true);
  if (!host->authorizedCaller(call.service())) {
    host->connection().send(call.createErrorReply(QDBusError::AccessDenied,
                                                   QStringLiteral("caller is not xdg-desktop-portal")));
    return;
  }
  (void)parentWindow;
  portal_->show(call, handle, appId, title, subtitle, body, options);
}

AccessPortal::AccessPortal(ScreenshotPortal* host, QObject* parent)
    : QObject(parent), host_(host) {
  new AccessAdaptor(host, this);
}

void AccessPortal::show(const QDBusMessage& call, const QDBusObjectPath& handle, const QString& appId,
                        const QString& title, const QString& subtitle, const QString& body,
                        const QVariantMap& options) {
  const QString path = handle.path();
  if (path.isEmpty() || path == QStringLiteral("/") ||
      QDBusConnection::sessionBus().objectRegisteredAt(path) != nullptr) {
    QDBusConnection::sessionBus().send(call.createReply({2U, QVariantMap{}}));
    return;
  }

  const quint64 id = nextId_++;
  auto pending = std::make_unique<Pending>();
  pending->call = call;
  pending->path = path;
  pending->request = new RequestObject(this);
  pending->watcher = new QDBusServiceWatcher(call.service(), QDBusConnection::sessionBus(),
      QDBusServiceWatcher::WatchForUnregistration, this);
  if (!QDBusConnection::sessionBus().registerObject(path, pending->request,
                                                     QDBusConnection::ExportAllSlots)) {
    pending->request->deleteLater();
    pending->watcher->deleteLater();
    QDBusConnection::sessionBus().send(call.createReply({2U, QVariantMap{}}));
    return;
  }

  auto* dialog = new QDialog;
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setWindowTitle(title.isEmpty() ? QStringLiteral("Permission request") : title);
  dialog->setModal(options.value(QStringLiteral("modal"), true).toBool());
  auto* layout = new QVBoxLayout(dialog);
  const QString heading = subtitle.isEmpty()
                              ? (appId.isEmpty() ? title : QStringLiteral("%1 requests access").arg(appId))
                              : subtitle;
  if (!heading.isEmpty()) {
    auto* label = new QLabel(heading, dialog);
    QFont font = label->font(); font.setBold(true); label->setFont(font);
    label->setWordWrap(true); layout->addWidget(label);
  }
  if (!body.isEmpty()) {
    auto* label = new QLabel(body, dialog); label->setWordWrap(true); layout->addWidget(label);
  }
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, dialog);
  buttons->button(QDialogButtonBox::Cancel)->setText(
      options.value(QStringLiteral("deny_label"), QStringLiteral("Deny")).toString());
  buttons->button(QDialogButtonBox::Ok)->setText(
      options.value(QStringLiteral("grant_label"), QStringLiteral("Allow")).toString());
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
  pending->dialog = dialog;
  connect(pending->request, &RequestObject::closeRequested, this, [this,id] { complete(id, 1); });
  connect(pending->watcher, &QDBusServiceWatcher::serviceUnregistered, this,
          [this,id] { complete(id, 2); });
  connect(dialog, &QDialog::accepted, this, [this,id] { complete(id, 0); });
  connect(dialog, &QDialog::rejected, this, [this,id] { complete(id, 1); });
  pending_.emplace(id, std::move(pending));
  dialog->show();
}

void AccessPortal::complete(quint64 id, uint response) {
  const auto found = pending_.find(id);
  if (found == pending_.end()) return;
  auto pending = std::move(found->second);
  pending_.erase(found);
  QDBusConnection::sessionBus().unregisterObject(pending->path);
  if (pending->dialog != nullptr) pending->dialog->close();
  pending->request->deleteLater();
  pending->watcher->deleteLater();
  QDBusConnection::sessionBus().send(pending->call.createReply({response, QVariantMap{}}));
}
