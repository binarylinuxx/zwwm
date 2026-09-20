#include "screencast_portal.hpp"

#include "capture_client.hpp"
#include "pipewire_stream.hpp"
#include "screenshot_portal.hpp"

#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMetaType>
#include <QDBusServiceWatcher>
#include <QDialog>
#include <QDialogButtonBox>
#include <QCheckBox>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <vector>

namespace {
constexpr quint32 kMonitor = 1;
constexpr quint32 kWindow = 2;
constexpr quint32 kHiddenCursor = 1;
constexpr quint32 kEmbeddedCursor = 2;
}

QDBusArgument& operator<<(QDBusArgument& argument, const IntPair& value) { argument.beginStructure(); argument << value.first << value.second; argument.endStructure(); return argument; }
const QDBusArgument& operator>>(const QDBusArgument& argument, IntPair& value) { argument.beginStructure(); argument >> value.first >> value.second; argument.endStructure(); return argument; }
QDBusArgument& operator<<(QDBusArgument& argument, const StreamEntry& value) { argument.beginStructure(); argument << value.nodeId << value.properties; argument.endStructure(); return argument; }
const QDBusArgument& operator>>(const QDBusArgument& argument, StreamEntry& value) { argument.beginStructure(); argument >> value.nodeId >> value.properties; argument.endStructure(); return argument; }

ScreenCastAdaptor::ScreenCastAdaptor(QObject* dbusObject, ScreenCastPortal* portal)
    : QDBusAbstractAdaptor(dbusObject), portal_(portal) {}
uint ScreenCastAdaptor::availableCursorModes() const { return portal_->availableCursorModes(); }

void ScreenCastAdaptor::CreateSession(const QDBusObjectPath& request, const QDBusObjectPath& session,
                                      const QString& app, const QVariantMap& options) {
  auto* host = static_cast<ScreenshotPortal*>(parent());
  const auto call = host->message(); host->setDelayedReply(true);
  if (!portal_->authorized(call.service())) { host->connection().send(call.createErrorReply(QDBusError::AccessDenied, "caller is not xdg-desktop-portal")); return; }
  portal_->createSession(call, request, session, app, options);
}
void ScreenCastAdaptor::SelectSources(const QDBusObjectPath& request, const QDBusObjectPath& session,
                                      const QString& app, const QVariantMap& options) {
  auto* host = static_cast<ScreenshotPortal*>(parent());
  const auto call = host->message(); host->setDelayedReply(true);
  if (!portal_->authorized(call.service())) { host->connection().send(call.createErrorReply(QDBusError::AccessDenied, "caller is not xdg-desktop-portal")); return; }
  portal_->selectSources(call, request, session, app, options);
}
void ScreenCastAdaptor::Start(const QDBusObjectPath& request, const QDBusObjectPath& session,
                              const QString& app, const QString& parentWindow, const QVariantMap& options) {
  auto* host = static_cast<ScreenshotPortal*>(parent());
  const auto call = host->message(); host->setDelayedReply(true);
  if (!portal_->authorized(call.service())) { host->connection().send(call.createErrorReply(QDBusError::AccessDenied, "caller is not xdg-desktop-portal")); return; }
  portal_->start(call, request, session, app, parentWindow, options);
}

ScreenCastPortal::ScreenCastPortal(CaptureClient* capture, ScreenshotPortal* host, QObject* parent)
    : QObject(parent), capture_(capture), dbusHost_(host) {
  qDBusRegisterMetaType<IntPair>(); qDBusRegisterMetaType<StreamEntry>(); qDBusRegisterMetaType<StreamList>();
  new ScreenCastAdaptor(host, this);
  connect(capture_, &CaptureClient::captured, this, [this](quint64 id, const QImage& image, quint32, quint64 sec, quint32 nsec) { captured(id, image, sec, nsec); });
  connect(capture_, &CaptureClient::failed, this, &ScreenCastPortal::captureFailed);
  connect(capture_, &CaptureClient::presented, this, [this](quint64, quint32) {
    for (auto& [path, session] : sessions_) {
      (void)path;
      if (session->stage == SessionStage::active) {
        session->presentationPending = true;
        session->pipewireReady = true;
        maybeCapture(*session);
      }
    }
  });
  connect(capture_, &CaptureClient::disconnected, this, [this] { while (!sessions_.empty()) closeSession(sessions_.begin()->first, true); });
}

ScreenCastPortal::~ScreenCastPortal() {
  while (!sessions_.empty()) closeSession(sessions_.begin()->first, true);
  while (!requests_.empty()) completeRequest(requests_.begin()->first, 2);
}

bool ScreenCastPortal::authorized(const QString& sender) const { return dbusHost_->authorizedCaller(sender); }
quint32 ScreenCastPortal::availableCursorModes() const { return capture_->cursorModes() & 3U; }

quint64 ScreenCastPortal::addRequest(const QDBusMessage& call, const QDBusObjectPath& handle, const QString& sessionPath) {
  const QString path = handle.path();
  if (path.isEmpty() || path == "/" || QDBusConnection::sessionBus().objectRegisteredAt(path) != nullptr) return 0;
  auto request = std::make_unique<Request>(); request->call=call; request->path=path; request->sessionPath=sessionPath;
  request->object = new RequestObject(this);
  request->watcher = new QDBusServiceWatcher(call.service(), QDBusConnection::sessionBus(), QDBusServiceWatcher::WatchForUnregistration, this);
  if (!QDBusConnection::sessionBus().registerObject(path, request->object, QDBusConnection::ExportAllSlots)) { request->object->deleteLater(); request->watcher->deleteLater(); return 0; }
  const quint64 id=nextRequest_++;
  connect(request->object, &RequestObject::closeRequested, this, [this,id] { const auto it=requests_.find(id); if (it!=requests_.end() && !it->second->sessionPath.isEmpty()) closeSession(it->second->sessionPath, false); completeRequest(id,1); });
  connect(request->watcher, &QDBusServiceWatcher::serviceUnregistered, this, [this,id] { const auto it=requests_.find(id); if (it!=requests_.end() && !it->second->sessionPath.isEmpty()) closeSession(it->second->sessionPath,true); completeRequest(id,2); });
  requests_.emplace(id,std::move(request)); return id;
}

void ScreenCastPortal::completeRequest(quint64 id, quint32 response, const QVariantMap& results) {
  const auto found=requests_.find(id); if (found==requests_.end()) return; auto request=std::move(found->second); requests_.erase(found);
  QDBusConnection::sessionBus().unregisterObject(request->path); if(request->dialog) request->dialog->reject(); request->object->deleteLater(); request->watcher->deleteLater();
  QDBusConnection::sessionBus().send(request->call.createReply({QVariant::fromValue(response),results}));
}

void ScreenCastPortal::createSession(const QDBusMessage& call, const QDBusObjectPath& handle,
                                     const QDBusObjectPath& sessionHandle, const QString& app, const QVariantMap&) {
  const QString path=sessionHandle.path(); const quint64 request=addRequest(call,handle,path);
  if(request==0 || path.isEmpty() || path=="/" || sessions_.contains(path) || QDBusConnection::sessionBus().objectRegisteredAt(path)!=nullptr) { if(request) completeRequest(request,2); else QDBusConnection::sessionBus().send(call.createReply({2u,QVariantMap{}})); return; }
  auto session=std::make_unique<Session>(); session->path=path; session->owner=call.service(); session->appId=app; session->object=new ScreenCastSessionObject(this);
  session->watcher=new QDBusServiceWatcher(call.service(),QDBusConnection::sessionBus(),QDBusServiceWatcher::WatchForUnregistration,this);
  if(!QDBusConnection::sessionBus().registerObject(path,session->object,QDBusConnection::ExportAllSlots|QDBusConnection::ExportAllSignals|QDBusConnection::ExportAllProperties)){session->object->deleteLater();session->watcher->deleteLater();completeRequest(request,2);return;}
  session->object->close=[this,path]{closeSession(path,false);};
  connect(session->watcher,&QDBusServiceWatcher::serviceUnregistered,this,[this,path]{closeSession(path,true);});
  sessions_.emplace(path,std::move(session));
  QTimer::singleShot(0,this,[this,request,path]{completeRequest(request,0,{{"session_id",path.section('/',-1)}});});
}

void ScreenCastPortal::selectSources(const QDBusMessage& call,const QDBusObjectPath& handle,const QDBusObjectPath& sessionHandle,const QString& app,const QVariantMap& options){
  const QString path=sessionHandle.path(); const quint64 request=addRequest(call,handle,path); if(!request){QDBusConnection::sessionBus().send(call.createReply({2u,QVariantMap{}}));return;}
  const auto found=sessions_.find(path); const quint32 types=options.value("types",kMonitor).toUInt()&(kMonitor|kWindow); const quint32 requestedCursor=options.value("cursor_mode",kHiddenCursor).toUInt(); const quint32 persist=options.value("persist_mode",0).toUInt(); const bool valid=found!=sessions_.end()&&found->second->owner==call.service()&&found->second->appId==app&&found->second->stage==SessionStage::created&&types!=0&&persist<=2U;
  if(!valid){completeRequest(request,2);return;} found->second->types=types; found->second->cursorMode=(availableCursorModes()&requestedCursor)!=0?requestedCursor:kHiddenCursor; found->second->persistMode=persist; found->second->stage=SessionStage::selected; QTimer::singleShot(0,this,[this,request]{completeRequest(request,0);});
}

void ScreenCastPortal::start(const QDBusMessage& call,const QDBusObjectPath& handle,const QDBusObjectPath& sessionHandle,const QString& app,const QString& parent,const QVariantMap&){
  const QString path=sessionHandle.path(); const quint64 request=addRequest(call,handle,path); if(!request){QDBusConnection::sessionBus().send(call.createReply({2u,QVariantMap{}}));return;}
  const auto found=sessions_.find(path); if(found==sessions_.end()||found->second->owner!=call.service()||found->second->appId!=app||found->second->stage!=SessionStage::selected){completeRequest(request,2);return;}
  (void)parent;
  found->second->stage=SessionStage::starting; found->second->startRequest=request;
  auto* dialog = new QDialog;
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setWindowTitle(QStringLiteral("Share your screen"));
  dialog->setModal(true);
  dialog->setMinimumWidth(480);
  auto* layout = new QVBoxLayout(dialog);
  auto* title = new QLabel(app.isEmpty() ? QStringLiteral("An application wants to share your screen")
                                         : QStringLiteral("%1 wants to share your screen").arg(app), dialog);
  QFont titleFont = title->font(); titleFont.setBold(true); title->setFont(titleFont);
  layout->addWidget(title);
  auto* detail = new QLabel(QStringLiteral("Choose a monitor or an application window to share."), dialog);
  detail->setWordWrap(true); layout->addWidget(detail);
  auto* sources = new QListWidget(dialog);
  sources->setSelectionMode(QAbstractItemView::SingleSelection);
  sources->setAlternatingRowColors(true);
  const auto populate = [this, sources, types=found->second->types] {
    const quint32 previousType = sources->currentItem() == nullptr ? 0 : sources->currentItem()->data(Qt::UserRole).toUInt();
    const quint64 previousId = sources->currentItem() == nullptr ? 0 : sources->currentItem()->data(Qt::UserRole + 1).toULongLong();
    sources->clear();
    int selected = -1;
    if ((types & kMonitor) != 0) for (const auto& monitor : capture_->monitors()) {
      const QString size = QStringLiteral("%1 x %2").arg(monitor.logicalSize.width()).arg(monitor.logicalSize.height());
      auto* item = new QListWidgetItem(QIcon::fromTheme(QStringLiteral("video-display")), monitor.name + QStringLiteral("\n") + size, sources);
      item->setData(Qt::UserRole, kMonitor); item->setData(Qt::UserRole + 1, QVariant::fromValue<qulonglong>(monitor.id));
      item->setToolTip(monitor.description);
      if (previousType == kMonitor && previousId == monitor.id) selected = sources->count() - 1;
    }
    if ((types & kWindow) != 0) for (const auto& window : capture_->toplevels()) {
      const QString title = window.title.isEmpty() ? QStringLiteral("Untitled window") : window.title;
      const QString application = window.appId.isEmpty() ? QStringLiteral("unknown application") : window.appId;
      auto* item = new QListWidgetItem(QIcon::fromTheme(QStringLiteral("window")), title + QStringLiteral("\n") + application, sources);
      item->setData(Qt::UserRole, kWindow); item->setData(Qt::UserRole + 1, QVariant::fromValue<qulonglong>(window.id));
      if (previousType == kWindow && previousId == window.id) selected = sources->count() - 1;
    }
    if (selected >= 0) sources->setCurrentRow(selected); else if (sources->count() != 0) sources->setCurrentRow(0);
  };
  populate();
  connect(capture_, &CaptureClient::toplevelsChanged, dialog, populate);
  sources->setMinimumHeight(std::min(300, std::max(96, sources->sizeHintForRow(0) * std::min(8, sources->count()) + 8)));
  layout->addWidget(sources);
  auto* cursor = new QCheckBox(QStringLiteral("Include mouse cursor"), dialog);
  cursor->setChecked(found->second->cursorMode == kEmbeddedCursor);
  cursor->setEnabled((availableCursorModes() & kEmbeddedCursor) != 0);
  layout->addWidget(cursor);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, dialog);
  buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Share"));
  buttons->button(QDialogButtonBox::Ok)->setDefault(true);
  buttons->button(QDialogButtonBox::Ok)->setEnabled(sources->currentItem() != nullptr);
  layout->addWidget(buttons);
  connect(sources, &QListWidget::currentItemChanged, buttons->button(QDialogButtonBox::Ok),
          [share=buttons->button(QDialogButtonBox::Ok)](QListWidgetItem* current) {
            share->setEnabled(current != nullptr);
          });
  connect(sources, &QListWidget::itemDoubleClicked, dialog, [dialog](QListWidgetItem*) {
    dialog->accept();
  });
  connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
  requests_.at(request)->dialog = dialog;
  connect(dialog, &QDialog::accepted, this, [this,path,sources,cursor] {
    const auto session = sessions_.find(path);
    if (session == sessions_.end() || sources->currentItem() == nullptr) { closeSession(path,false); return; }
    session->second->types = sources->currentItem()->data(Qt::UserRole).toUInt();
    const quint64 sourceId = sources->currentItem()->data(Qt::UserRole + 1).toULongLong();
    session->second->cursorMode = cursor->isChecked() ? kEmbeddedCursor : kHiddenCursor;
    if (session->second->types == kMonitor) session->second->outputId = sourceId;
    else {
      session->second->toplevelId = sourceId;
      const auto windows = capture_->toplevels();
      const auto selected = std::find_if(windows.begin(), windows.end(), [sourceId](const auto& window) { return window.id == sourceId; });
      if (selected == windows.end()) { closeSession(path,false); return; }
      session->second->sourceGeometry = selected->geometry;
    }
    // The picker is a surface in the captured scene. Wait for Qt to unmap it
    // and for the compositor's close animation to finish before the first frame.
    QTimer::singleShot(350, this, [this, path] {
      const auto current = sessions_.find(path);
      if (current == sessions_.end() || current->second->stage != SessionStage::starting) return;
      if (!requestFrame(*current->second)) closeSession(path,true);
    });
  });
  connect(dialog, &QDialog::rejected, this, [this,path] { closeSession(path,false); });
  dialog->show();
}

bool ScreenCastPortal::requestFrame(Session& session){session.captureId=nextCapture_++;return session.types==kWindow?capture_->requestToplevel(session.captureId,session.toplevelId,session.cursorMode==kEmbeddedCursor):capture_->requestOutput(session.captureId,session.outputId,session.cursorMode==kEmbeddedCursor);}

void ScreenCastPortal::maybeCapture(Session& session){if(session.stage!=SessionStage::active||!session.pipewireReady||!session.presentationPending||session.captureId!=0)return;session.presentationPending=false;if(!requestFrame(session))closeSession(session.path,true);}

void ScreenCastPortal::captured(quint64 id,const QImage& image,quint64 seconds,quint32 nanoseconds){for(auto& [path,ptr]:sessions_){auto& s=*ptr;if(s.captureId!=id)continue;s.captureId=0;if(s.stage==SessionStage::starting){beginPipeWire(s,image,seconds,nanoseconds);return;}if(s.stage==SessionStage::active){if(image.size()!=s.frameSize){closeSession(path,true);return;}s.stream->submit(image,seconds,nanoseconds);s.pipewireReady=false;maybeCapture(s);return;}}}
void ScreenCastPortal::captureFailed(quint64 id){for(auto& [path,s]:sessions_)if(s->captureId==id){s->captureId=0;closeSession(path,true);return;}}

void ScreenCastPortal::beginPipeWire(Session& session,const QImage& image,quint64 seconds,quint32 nanoseconds){session.firstFrame=image;session.frameSize=image.size();session.firstSeconds=seconds;session.firstNanoseconds=nanoseconds;session.stream=std::make_unique<PipeWireStream>(image.size());const QString path=session.path;connect(session.stream.get(),&PipeWireStream::ready,this,[this,path]{streamReady(path);});connect(session.stream.get(),&PipeWireStream::failed,this,[this,path]{closeSession(path,true);});connect(session.stream.get(),&PipeWireStream::frameNeeded,this,[this,path]{const auto it=sessions_.find(path);if(it==sessions_.end())return;auto& s=*it->second;s.pipewireReady=true;s.presentationPending=true;if(!s.firstFrame.isNull()){if(s.stream->submit(s.firstFrame,s.firstSeconds,s.firstNanoseconds))s.firstFrame={};s.pipewireReady=false;s.presentationPending=false;}maybeCapture(s);});if(!session.stream->start()){closeSession(path,true);return;}QTimer::singleShot(5000,this,[this,path]{const auto it=sessions_.find(path);if(it!=sessions_.end()&&it->second->stage==SessionStage::starting)closeSession(path,true);});}

void ScreenCastPortal::streamReady(const QString& path){const auto found=sessions_.find(path);if(found==sessions_.end())return;auto& s=*found->second;if(s.stage!=SessionStage::starting||s.stream->nodeId()==UINT32_MAX)return;QVariantMap properties;properties["source_type"]=s.types;const QSize logical=s.types==kWindow?s.frameSize:capture_->outputLogicalSize(s.outputId);const QPoint position=s.types==kWindow?s.sourceGeometry.topLeft():capture_->outputPosition(s.outputId);properties["size"]=QVariant::fromValue(IntPair{logical.width(),logical.height()});properties["position"]=QVariant::fromValue(IntPair{position.x(),position.y()});StreamList streams{{s.stream->nodeId(),properties}};const quint64 request=s.startRequest;s.stage=SessionStage::active;QVariantMap results{{"streams",QVariant::fromValue(streams)}};if(s.persistMode!=0)results["persist_mode"]=0U;completeRequest(request,0,results);}

void ScreenCastPortal::closeSession(const QString& path,bool notify){auto found=sessions_.find(path);if(found==sessions_.end())return;auto session=std::move(found->second);sessions_.erase(found);if(session->captureId)capture_->cancel(session->captureId);std::vector<quint64> pending;for(const auto& [id,r]:requests_)if(r->sessionPath==path)pending.push_back(id);for(quint64 id:pending)completeRequest(id,notify?2:1);if(notify)emit session->object->Closed();QDBusConnection::sessionBus().unregisterObject(path);session->object->deleteLater();session->watcher->deleteLater();}
