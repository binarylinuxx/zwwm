#include "file_chooser_portal.hpp"

#include "screenshot_portal.hpp"

#include <QByteArray>
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusServiceWatcher>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QUrl>

namespace {

void configureDialog(QFileDialog* dialog, const QString& title, const QVariantMap& options,
                     bool save, bool saveMultiple) {
  dialog->setOption(QFileDialog::DontUseNativeDialog, true);
  dialog->setWindowTitle(title.isEmpty() ? (save ? QStringLiteral("Save file") : QStringLiteral("Open file")) : title);
  dialog->setFileMode(save ? QFileDialog::AnyFile
                           : options.value(QStringLiteral("directory"), false).toBool()
                                 ? QFileDialog::Directory
                                 : options.value(QStringLiteral("multiple"), false).toBool()
                                       ? QFileDialog::ExistingFiles : QFileDialog::ExistingFile);
  dialog->setAcceptMode(save ? QFileDialog::AcceptSave : QFileDialog::AcceptOpen);
  if (saveMultiple) dialog->setFileMode(QFileDialog::Directory);
  const QByteArray currentFolder = options.value(QStringLiteral("current_folder")).toByteArray();
  if (!currentFolder.isEmpty()) dialog->setDirectory(QFile::decodeName(currentFolder.constData()));
  const QByteArray currentFile = options.value(QStringLiteral("current_file")).toByteArray();
  if (!currentFile.isEmpty()) dialog->selectFile(QFile::decodeName(currentFile.constData()));
  const QString currentName = options.value(QStringLiteral("current_name")).toString();
  if (!currentName.isEmpty()) dialog->selectFile(currentName);
  const QString acceptLabel = options.value(QStringLiteral("accept_label")).toString();
  if (!acceptLabel.isEmpty()) dialog->setLabelText(QFileDialog::Accept, acceptLabel);
}

}  // namespace

int runFileChooserProbe(const QString& mode) {
  QVariantMap options;
  const bool save = mode == QStringLiteral("save") || mode == QStringLiteral("save-files");
  const bool saveMultiple = mode == QStringLiteral("save-files");
  if (mode == QStringLiteral("multiple")) options.insert(QStringLiteral("multiple"), true);
  if (mode == QStringLiteral("directory")) options.insert(QStringLiteral("directory"), true);
  if (save) options.insert(QStringLiteral("current_name"), QStringLiteral("example.txt"));
  QFileDialog dialog;
  configureDialog(&dialog, save ? QStringLiteral("Save attachment") : QStringLiteral("Choose a file"),
                  options, save, saveMultiple);
  return dialog.exec() == QDialog::Accepted ? 0 : 1;
}

FileChooserAdaptor::FileChooserAdaptor(ScreenshotPortal* host, FileChooserPortal* portal)
    : QDBusAbstractAdaptor(host), portal_(portal) {}

void FileChooserAdaptor::begin(const QDBusObjectPath& handle, const QString& appId,
                               const QString& parentWindow, const QString& title,
                               const QVariantMap& options, bool save, bool saveMultiple) {
  auto* host = static_cast<ScreenshotPortal*>(parent());
  const QDBusMessage call = host->message();
  host->setDelayedReply(true);
  if (!host->authorizedCaller(call.service())) {
    host->connection().send(call.createErrorReply(QDBusError::AccessDenied,
                                                   QStringLiteral("caller is not xdg-desktop-portal")));
    return;
  }
  (void)appId;
  (void)parentWindow;
  portal_->show(call, handle, title, options, save, saveMultiple);
}

void FileChooserAdaptor::OpenFile(const QDBusObjectPath& handle, const QString& appId,
                                  const QString& parentWindow, const QString& title,
                                  const QVariantMap& options) {
  begin(handle, appId, parentWindow, title, options, false, false);
}

void FileChooserAdaptor::SaveFile(const QDBusObjectPath& handle, const QString& appId,
                                  const QString& parentWindow, const QString& title,
                                  const QVariantMap& options) {
  begin(handle, appId, parentWindow, title, options, true, false);
}

void FileChooserAdaptor::SaveFiles(const QDBusObjectPath& handle, const QString& appId,
                                   const QString& parentWindow, const QString& title,
                                   const QVariantMap& options) {
  begin(handle, appId, parentWindow, title, options, true, true);
}

FileChooserPortal::FileChooserPortal(ScreenshotPortal* host, QObject* parent)
    : QObject(parent), host_(host) {
  new FileChooserAdaptor(host, this);
}

void FileChooserPortal::show(const QDBusMessage& call, const QDBusObjectPath& handle,
                             const QString& title, const QVariantMap& options,
                             bool save, bool saveMultiple) {
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

  auto* dialog = new QFileDialog;
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  configureDialog(dialog, title, options, save, saveMultiple);

  pending->dialog = dialog;
  connect(pending->request, &RequestObject::closeRequested, this, [this,id] { complete(id, 1); });
  connect(pending->watcher, &QDBusServiceWatcher::serviceUnregistered, this, [this,id] { complete(id, 2); });
  connect(dialog, &QFileDialog::accepted, this, [this,id] { complete(id, 0); });
  connect(dialog, &QFileDialog::rejected, this, [this,id] { complete(id, 1); });
  pending_.emplace(id, std::move(pending));
  dialog->show();
}

void FileChooserPortal::complete(quint64 id, uint response) {
  const auto found = pending_.find(id);
  if (found == pending_.end()) return;
  auto pending = std::move(found->second);
  pending_.erase(found);
  QVariantMap results;
  if (response == 0 && pending->dialog != nullptr) {
    QStringList uris;
    for (const QString& file : pending->dialog->selectedFiles()) uris.push_back(QUrl::fromLocalFile(file).toString());
    results.insert(QStringLiteral("uris"), uris);
  }
  QDBusConnection::sessionBus().unregisterObject(pending->path);
  if (pending->dialog != nullptr) pending->dialog->close();
  pending->request->deleteLater();
  pending->watcher->deleteLater();
  QDBusConnection::sessionBus().send(pending->call.createReply({response, results}));
}
