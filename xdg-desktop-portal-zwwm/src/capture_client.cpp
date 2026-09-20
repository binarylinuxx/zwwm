#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "capture_client.hpp"

#include <ext-zwwm-toplevels-unstable-v1-zwayland-client.h>
#include <zwwm-screencopy-view-unstable-v1-zwayland-client.h>
#include <zwayland/client/core.hpp>
#include <zwayland/wire/message.hpp>

#include <QSocketNotifier>
#include <QSize>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <linux/memfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace {

namespace protocol = zwayland::generated;

constexpr std::uint32_t kMagic = 0x5a574350U;
enum class BootstrapType : std::uint32_t { attach = 1, ready = 2 };
struct BootstrapMessage {
  std::uint32_t magic = kMagic;
  BootstrapType type = BootstrapType::attach;
};

bool sendBootstrap(int socket, const BootstrapMessage& message, int fd) {
  iovec io{.iov_base = const_cast<BootstrapMessage*>(&message), .iov_len = sizeof(message)};
  msghdr header{};
  header.msg_iov = &io;
  header.msg_iovlen = 1;
  alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
  header.msg_control = control;
  header.msg_controllen = sizeof(control);
  auto* cmsg = CMSG_FIRSTHDR(&header);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(int));
  std::memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));
  return sendmsg(socket, &header, MSG_NOSIGNAL) == static_cast<ssize_t>(sizeof(message));
}

bool receiveBootstrap(int socket, BootstrapMessage& message) {
  return recv(socket, &message, sizeof(message), 0) == static_cast<ssize_t>(sizeof(message)) &&
         message.magic == kMagic;
}

}  // namespace

struct CaptureClient::Impl {
  struct Frame {
    Impl* owner = nullptr;
    quint64 id = 0;
    zwayland::client::Proxy* frame = nullptr;
    zwayland::client::Proxy* buffer = nullptr;
    void* mapping = nullptr;
    std::size_t size = 0;
    std::uint32_t width = 0, height = 0, stride = 0, format = 0, flags = 0;
    bool copySent = false;
  };
  struct Toplevel {
    Impl* owner = nullptr;
    zwayland::client::Proxy* handle = nullptr;
    ToplevelSource source;
    bool closed = false;
  };
  struct Output {
    Impl* owner = nullptr;
    std::uint32_t registryName = 0;
    zwayland::client::Proxy* proxy = nullptr;
    QString make;
    QString model;
    QString name;
    QString description;
    int x = 0, y = 0, width = 0, height = 0, scale = 1,
        transform = static_cast<int>(protocol::WL_OUTPUT_TRANSFORM_NORMAL);
    std::uint32_t refresh = 0;
  };

  CaptureClient* q = nullptr;
  zwayland::wire::FileDescriptor capability;
  std::unique_ptr<zwayland::client::Display> display;
  zwayland::client::Proxy* registry = nullptr;
  zwayland::client::Proxy* shm = nullptr;
  zwayland::client::Proxy* screencopy = nullptr;
  zwayland::client::Proxy* toplevelManager = nullptr;
  std::uint32_t screencopyName = 0, toplevelName = 0;
  QSocketNotifier* waylandNotifier = nullptr;
  QSocketNotifier* waylandWriteNotifier = nullptr;
  QSocketNotifier* capabilityNotifier = nullptr;
  QString error;
  bool unavailable = false;
  quint64 nextToplevelId = 1;
  std::uint32_t cursorModes = protocol::ZWWM_SCREENCOPY_VIEW_MANAGER_V1_CURSOR_MODE_HIDDEN;
  std::unordered_map<quint64, std::unique_ptr<Frame>> frames;
  std::vector<std::unique_ptr<Output>> outputs;
  std::vector<std::unique_ptr<Toplevel>> toplevels;

  Output* output(quint64 id = 0) const {
    const auto found = std::find_if(outputs.begin(), outputs.end(), [id](const auto& item) {
      return id == 0 || item->registryName == id;
    });
    return found == outputs.end() ? nullptr : found->get();
  }

  static QSize logicalSize(const Output& output) {
    int width = output.width;
    int height = output.height;
    if (output.transform == static_cast<int>(protocol::WL_OUTPUT_TRANSFORM_90) ||
        output.transform == static_cast<int>(protocol::WL_OUTPUT_TRANSFORM_270))
      std::swap(width, height);
    return {std::max(1, width / std::max(1, output.scale)),
            std::max(1, height / std::max(1, output.scale))};
  }

  void cleanupFrame(Frame& frame) {
    if (frame.frame != nullptr)
      protocol::zwwm_screencopy_frame_v1_destroy(*display, frame.frame->id);
    if (frame.buffer != nullptr) protocol::wl_buffer_destroy(*display, frame.buffer->id);
    if (frame.mapping != nullptr) munmap(frame.mapping, frame.size);
    frame.frame = nullptr;
    frame.buffer = nullptr;
    frame.mapping = nullptr;
  }

  void finishFrame(Frame* frame, bool ready, std::uint64_t seconds = 0, std::uint32_t nanoseconds = 0) {
    if (frame == nullptr) return;
    const auto found = frames.find(frame->id);
    if (found == frames.end() || found->second.get() != frame) return;
    auto owned = std::move(found->second);
    frames.erase(found);
    QImage image;
    if (ready && owned->mapping != nullptr) {
      const QImage view(static_cast<const uchar*>(owned->mapping), static_cast<int>(owned->width),
                        static_cast<int>(owned->height), static_cast<qsizetype>(owned->stride),
                        QImage::Format_RGB32);
      image = view.copy();
      if ((owned->flags & protocol::ZWWM_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT) != 0)
        image = image.flipped(Qt::Vertical);
    }
    const quint64 id = owned->id;
    const quint32 flags = owned->flags;
    cleanupFrame(*owned);
    if (!ready || image.isNull()) emit q->failed(id);
    else emit q->captured(id, image, flags, seconds, nanoseconds);
  }

  bool addFrame(quint64 id, zwayland::client::Proxy* proxy) {
    if (id == 0 || proxy == nullptr || frames.contains(id)) {
      if (proxy != nullptr) protocol::zwwm_screencopy_frame_v1_destroy(*display, proxy->id);
      return false;
    }
    auto frame = std::make_unique<Frame>();
    frame->owner = this;
    frame->id = id;
    frame->frame = proxy;
    proxy->set_data(frame.get());
    frames.emplace(id, std::move(frame));
    flush();
    return true;
  }

  bool flush() {
    if (display == nullptr || !display->flush()) return false;
    if (waylandWriteNotifier != nullptr) waylandWriteNotifier->setEnabled(display->wants_write());
    return true;
  }

  void releaseOutput(Output& output) {
    if (output.proxy == nullptr || display == nullptr) return;
    if (output.proxy->version >= 3) protocol::wl_output_release(*display, output.proxy->id);
    else display->destroy_proxy(output.proxy->id);
    output.proxy = nullptr;
  }

  void disconnect() {
    if (unavailable) return;
    unavailable = true;
    if (waylandNotifier != nullptr) waylandNotifier->setEnabled(false);
    if (waylandWriteNotifier != nullptr) waylandWriteNotifier->setEnabled(false);
    if (capabilityNotifier != nullptr) capabilityNotifier->setEnabled(false);
    while (!frames.empty()) finishFrame(frames.begin()->second.get(), false);
    emit q->disconnected();
  }
};

namespace {

struct FrameHandler {
  void buffer(zwayland::client::Proxy& proxy, std::uint32_t format, std::uint32_t width,
              std::uint32_t height, std::uint32_t stride) const {
    auto* frame = proxy.data<CaptureClient::Impl::Frame>();
    if (frame == nullptr) return;
    if (frame->copySent || format != protocol::WL_SHM_FORMAT_XRGB8888 || width == 0 || height == 0 ||
        width > static_cast<std::uint32_t>(INT32_MAX / 4) || stride != width * 4U ||
        height > SIZE_MAX / stride || static_cast<std::size_t>(stride) * height > INT32_MAX) {
      frame->owner->finishFrame(frame, false);
      return;
    }
    frame->format = format; frame->width = width; frame->height = height; frame->stride = stride;
    frame->size = static_cast<std::size_t>(stride) * height;
    zwayland::wire::FileDescriptor fd(
        static_cast<int>(syscall(SYS_memfd_create, "zwwm-screencopy-client", MFD_CLOEXEC)));
    if (fd.get() < 0 || ftruncate(fd.get(), static_cast<off_t>(frame->size)) != 0) {
      frame->owner->finishFrame(frame, false);
      return;
    }
    frame->mapping = mmap(nullptr, frame->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd.get(), 0);
    if (frame->mapping == MAP_FAILED) {
      frame->mapping = nullptr;
      frame->owner->finishFrame(frame, false);
      return;
    }
    auto& display = *frame->owner->display;
    const std::uint32_t poolId = protocol::wl_shm_create_pool(
        display, frame->owner->shm->id, fd.get(), static_cast<int>(frame->size));
    const std::uint32_t bufferId = protocol::wl_shm_pool_create_buffer(
        display, poolId, 0, static_cast<int>(width), static_cast<int>(height),
        static_cast<int>(stride), format);
    frame->buffer = display.find_proxy(bufferId);
    protocol::wl_shm_pool_destroy(display, poolId);
    if (frame->buffer == nullptr) {
      frame->owner->finishFrame(frame, false);
      return;
    }
    frame->copySent = true;
    protocol::zwwm_screencopy_frame_v1_copy(display, frame->frame->id, frame->buffer);
    if (!frame->owner->flush()) frame->owner->disconnect();
  }
  void flags(zwayland::client::Proxy& proxy, std::uint32_t flags) const {
    if (auto* frame = proxy.data<CaptureClient::Impl::Frame>(); frame != nullptr) frame->flags = flags;
  }
  void ready(zwayland::client::Proxy& proxy, std::uint32_t hi, std::uint32_t lo,
             std::uint32_t nanoseconds) const {
    if (auto* frame = proxy.data<CaptureClient::Impl::Frame>(); frame != nullptr)
      frame->owner->finishFrame(frame, true, (static_cast<std::uint64_t>(hi) << 32U) | lo, nanoseconds);
  }
  void failed(zwayland::client::Proxy& proxy, std::uint32_t) const {
    if (auto* frame = proxy.data<CaptureClient::Impl::Frame>(); frame != nullptr)
      frame->owner->finishFrame(frame, false);
  }
};

struct ShmHandler { void format(zwayland::client::Proxy&, std::uint32_t) const {} };

struct OutputHandler {
  void geometry(zwayland::client::Proxy& proxy, std::int32_t x, std::int32_t y,
                std::int32_t, std::int32_t, std::int32_t, const std::string& make,
                const std::string& model, std::int32_t transform) const {
    auto* output = proxy.data<CaptureClient::Impl::Output>();
    if (output == nullptr) return;
    output->x = x; output->y = y; output->make = QString::fromStdString(make);
    output->model = QString::fromStdString(model); output->transform = transform;
  }
  void mode(zwayland::client::Proxy& proxy, std::uint32_t flags, std::int32_t width,
            std::int32_t height, std::int32_t refresh) const {
    if ((flags & protocol::WL_OUTPUT_MODE_CURRENT) == 0) return;
    auto* output = proxy.data<CaptureClient::Impl::Output>();
    if (output == nullptr) return;
    output->width = width; output->height = height;
    output->refresh = static_cast<std::uint32_t>(std::max(0, refresh));
  }
  void done(zwayland::client::Proxy&) const {}
  void scale(zwayland::client::Proxy& proxy, std::int32_t scale) const {
    if (auto* output = proxy.data<CaptureClient::Impl::Output>(); output != nullptr)
      output->scale = std::max(1, scale);
  }
  void name(zwayland::client::Proxy& proxy, const std::string& value) const {
    if (auto* output = proxy.data<CaptureClient::Impl::Output>(); output != nullptr)
      output->name = QString::fromStdString(value);
  }
  void description(zwayland::client::Proxy& proxy, const std::string& value) const {
    if (auto* output = proxy.data<CaptureClient::Impl::Output>(); output != nullptr)
      output->description = QString::fromStdString(value);
  }
};

struct ScreencopyHandler {
  CaptureClient::Impl* owner;
  void cursor_modes(zwayland::client::Proxy&, std::uint32_t modes) const { owner->cursorModes = modes; }
  void presented(zwayland::client::Proxy&, zwayland::client::Proxy*, std::uint32_t hi,
                 std::uint32_t lo, std::uint32_t nanoseconds) const {
    emit owner->q->presented((static_cast<std::uint64_t>(hi) << 32U) | lo, nanoseconds);
  }
};

struct ToplevelHandler {
  void title(zwayland::client::Proxy& proxy, const std::string& value) const {
    if (auto* item = proxy.data<CaptureClient::Impl::Toplevel>(); item != nullptr)
      item->source.title = QString::fromStdString(value);
  }
  void app_id(zwayland::client::Proxy& proxy, const std::string& value) const {
    if (auto* item = proxy.data<CaptureClient::Impl::Toplevel>(); item != nullptr)
      item->source.appId = QString::fromStdString(value);
  }
  void geometry(zwayland::client::Proxy& proxy, std::int32_t x, std::int32_t y,
                std::int32_t width, std::int32_t height) const {
    if (auto* item = proxy.data<CaptureClient::Impl::Toplevel>(); item != nullptr)
      item->source.geometry = QRect(x, y, width, height);
  }
  void state(zwayland::client::Proxy& proxy, std::uint32_t state) const {
    if (auto* item = proxy.data<CaptureClient::Impl::Toplevel>(); item != nullptr)
      item->source.state = state;
  }
  void output(zwayland::client::Proxy& proxy, zwayland::client::Proxy* output) const {
    if (auto* item = proxy.data<CaptureClient::Impl::Toplevel>(); item != nullptr)
      item->source.hasOutput = output != nullptr;
  }
  void closed(zwayland::client::Proxy& proxy) const {
    if (auto* item = proxy.data<CaptureClient::Impl::Toplevel>(); item != nullptr) item->closed = true;
  }
};

struct ToplevelManagerHandler {
  CaptureClient::Impl* owner;
  void toplevel(zwayland::client::Proxy&, zwayland::client::Proxy* proxy) const {
    auto source = std::make_unique<CaptureClient::Impl::Toplevel>();
    source->owner = owner; source->handle = proxy; source->source.id = owner->nextToplevelId++;
    proxy->set_data(source.get());
    owner->toplevels.push_back(std::move(source));
  }
  void done(zwayland::client::Proxy&) const {
    std::erase_if(owner->toplevels, [this](const auto& item) {
      if (!item->closed) return false;
      protocol::ext_zwwm_toplevel_handle_v1_destroy(*owner->display, item->handle->id);
      return true;
    });
    emit owner->q->toplevelsChanged();
  }
  void finished(zwayland::client::Proxy&) const {
    for (auto& item : owner->toplevels) item->closed = true;
    emit owner->q->toplevelsChanged();
  }
};

struct RegistryHandler {
  CaptureClient::Impl* owner;
  void global(zwayland::client::Proxy& registry, std::uint32_t name,
              std::string_view interface, std::uint32_t version) const {
    if (interface == protocol::wl_shm_interface.name) {
      owner->shm = zwayland::client::core::bind(
          *owner->display, registry, name, protocol::wl_shm_interface, 1);
    } else if (interface == protocol::wl_output_interface.name) {
      auto output = std::make_unique<CaptureClient::Impl::Output>();
      output->owner = owner; output->registryName = name;
      output->proxy = zwayland::client::core::bind(
          *owner->display, registry, name, protocol::wl_output_interface, std::min(version, 4U));
      output->proxy->set_data(output.get());
      owner->outputs.push_back(std::move(output));
    } else if (interface == protocol::zwwm_screencopy_view_manager_v1_interface.name) {
      owner->screencopyName = name;
      owner->screencopy = zwayland::client::core::bind(
          *owner->display, registry, name, protocol::zwwm_screencopy_view_manager_v1_interface, 1);
    } else if (interface == protocol::ext_zwwm_toplevels_v1_interface.name) {
      owner->toplevelName = name;
      owner->toplevelManager = zwayland::client::core::bind(
          *owner->display, registry, name, protocol::ext_zwwm_toplevels_v1_interface, 1);
    }
  }
  void global_remove(zwayland::client::Proxy&, std::uint32_t name) const {
    const auto output = std::find_if(owner->outputs.begin(), owner->outputs.end(),
        [name](const auto& item) { return item->registryName == name; });
    if (output != owner->outputs.end()) {
      owner->releaseOutput(**output);
      owner->outputs.erase(output);
      while (!owner->frames.empty()) owner->finishFrame(owner->frames.begin()->second.get(), false);
    } else if (name == owner->screencopyName || name == owner->toplevelName) {
      owner->disconnect();
    }
  }
};

}  // namespace

CaptureClient::CaptureClient(int capabilityFd, QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>()) {
  impl_->q = this;
  impl_->capability = zwayland::wire::FileDescriptor(capabilityFd);
}

CaptureClient::~CaptureClient() {
  delete impl_->waylandNotifier;
  delete impl_->waylandWriteNotifier;
  delete impl_->capabilityNotifier;
  while (!impl_->frames.empty()) impl_->finishFrame(impl_->frames.begin()->second.get(), false);
  if (impl_->display != nullptr) {
    for (auto& item : impl_->toplevels) if (item->handle != nullptr)
      protocol::ext_zwwm_toplevel_handle_v1_destroy(*impl_->display, item->handle->id);
    if (impl_->toplevelManager != nullptr)
      protocol::ext_zwwm_toplevels_v1_destroy(*impl_->display, impl_->toplevelManager->id);
    if (impl_->screencopy != nullptr)
      protocol::zwwm_screencopy_view_manager_v1_destroy(*impl_->display, impl_->screencopy->id);
    for (auto& output : impl_->outputs) impl_->releaseOutput(*output);
    if (impl_->shm != nullptr) impl_->display->destroy_proxy(impl_->shm->id);
    if (impl_->registry != nullptr) impl_->display->destroy_proxy(impl_->registry->id);
  }
}

bool CaptureClient::start() {
  int wayland[2];
  if (impl_->capability.get() < 0 || socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, wayland) != 0) {
    impl_->error = QStringLiteral("could not create private Wayland connection");
    return false;
  }
  if (!sendBootstrap(impl_->capability.get(), BootstrapMessage{.type = BootstrapType::attach}, wayland[0])) {
    close(wayland[0]); close(wayland[1]);
    impl_->error = QStringLiteral("compositor rejected capability attachment");
    return false;
  }
  close(wayland[0]);
  BootstrapMessage response;
  if (!receiveBootstrap(impl_->capability.get(), response) || response.type != BootstrapType::ready) {
    close(wayland[1]);
    impl_->error = QStringLiteral("compositor did not authorize the portal client");
    return false;
  }
  impl_->display = zwayland::client::Display::adopt(
      std::make_unique<zwayland::wire::Connection>(wayland[1]));
  zwayland::client::core::observe_registry(*impl_->display, RegistryHandler{impl_.get()});
  protocol::wl_shm_observe(*impl_->display, ShmHandler{});
  protocol::wl_output_observe(*impl_->display, OutputHandler{});
  protocol::zwwm_screencopy_view_manager_v1_observe(
      *impl_->display, ScreencopyHandler{impl_.get()});
  protocol::zwwm_screencopy_frame_v1_observe(*impl_->display, FrameHandler{});
  protocol::ext_zwwm_toplevels_v1_observe(
      *impl_->display, ToplevelManagerHandler{impl_.get()});
  protocol::ext_zwwm_toplevel_handle_v1_observe(*impl_->display, ToplevelHandler{});
  impl_->registry = zwayland::client::core::get_registry(*impl_->display);
  if (impl_->registry == nullptr || !impl_->display->roundtrip() || !impl_->display->roundtrip() ||
      impl_->shm == nullptr || impl_->outputs.empty() || impl_->screencopy == nullptr ||
      impl_->toplevelManager == nullptr) {
    impl_->error = QStringLiteral("required privileged Wayland globals are unavailable");
    return false;
  }
  impl_->waylandNotifier = new QSocketNotifier(impl_->display->fd(), QSocketNotifier::Read, this);
  connect(impl_->waylandNotifier, &QSocketNotifier::activated, this, [this] {
    if (!impl_->display->dispatch_pending() || !impl_->flush()) impl_->disconnect();
  });
  impl_->waylandWriteNotifier = new QSocketNotifier(
      impl_->display->fd(), QSocketNotifier::Write, this);
  impl_->waylandWriteNotifier->setEnabled(impl_->display->wants_write());
  connect(impl_->waylandWriteNotifier, &QSocketNotifier::activated, this, [this] {
    if (!impl_->flush()) impl_->disconnect();
  });
  impl_->capabilityNotifier = new QSocketNotifier(impl_->capability.get(), QSocketNotifier::Read, this);
  connect(impl_->capabilityNotifier, &QSocketNotifier::activated, this, [this] {
    char byte;
    if (recv(impl_->capability.get(), &byte, 1, MSG_DONTWAIT | MSG_PEEK) <= 0) impl_->disconnect();
    else impl_->disconnect();
  });
  return true;
}

bool CaptureClient::request(quint64 requestId, bool overlayCursor) {
  return requestOutput(requestId, 0, overlayCursor);
}

bool CaptureClient::requestOutput(quint64 requestId, quint64 outputId, bool overlayCursor) {
  auto* output = impl_->output(outputId);
  if (impl_->unavailable || impl_->screencopy == nullptr || output == nullptr) return false;
  const std::uint32_t id = protocol::zwwm_screencopy_view_manager_v1_capture_output(
      *impl_->display, impl_->screencopy->id, output->proxy, overlayCursor ? 1 : 0);
  return impl_->addFrame(requestId, impl_->display->find_proxy(id));
}

bool CaptureClient::requestRegion(quint64 requestId, const QRect& region, bool overlayCursor) {
  auto* output = impl_->output();
  if (impl_->unavailable || impl_->screencopy == nullptr || output == nullptr ||
      region.width() <= 0 || region.height() <= 0) return false;
  const std::uint32_t id = protocol::zwwm_screencopy_view_manager_v1_capture_output_region(
      *impl_->display, impl_->screencopy->id, output->proxy, overlayCursor ? 1 : 0,
      region.x(), region.y(), region.width(), region.height());
  return impl_->addFrame(requestId, impl_->display->find_proxy(id));
}

bool CaptureClient::requestToplevel(quint64 requestId, quint64 toplevelId, bool overlayCursor) {
  const auto found = std::find_if(impl_->toplevels.begin(), impl_->toplevels.end(),
      [toplevelId](const auto& item) { return !item->closed && item->source.id == toplevelId; });
  if (impl_->unavailable || found == impl_->toplevels.end()) return false;
  const std::uint32_t id = protocol::ext_zwwm_toplevel_handle_v1_capture(
      *impl_->display, (*found)->handle->id, overlayCursor ? 1 : 0);
  return impl_->addFrame(requestId, impl_->display->find_proxy(id));
}

void CaptureClient::cancel(quint64 requestId) {
  const auto found = impl_->frames.find(requestId);
  if (found == impl_->frames.end()) return;
  auto frame = std::move(found->second);
  impl_->frames.erase(found);
  impl_->cleanupFrame(*frame);
}

QList<ToplevelSource> CaptureClient::toplevels() const {
  QList<ToplevelSource> result;
  for (const auto& item : impl_->toplevels) if (!item->closed) result.push_back(item->source);
  return result;
}

QList<MonitorSource> CaptureClient::monitors() const {
  QList<MonitorSource> result;
  for (const auto& output : impl_->outputs) {
    QString name = output->name;
    if (name.isEmpty()) name = output->model;
    if (name.isEmpty()) name = QStringLiteral("Monitor %1").arg(result.size() + 1);
    result.push_back({output->registryName, name, output->description,
                      {output->x, output->y}, Impl::logicalSize(*output)});
  }
  return result;
}

QSize CaptureClient::outputLogicalSize() const {
  return outputLogicalSize(0);
}

QSize CaptureClient::outputLogicalSize(quint64 outputId) const {
  const auto* output = impl_->output(outputId);
  return output == nullptr ? QSize{} : Impl::logicalSize(*output);
}

QPoint CaptureClient::outputPosition(quint64 outputId) const {
  const auto* output = impl_->output(outputId);
  return output == nullptr ? QPoint{} : QPoint{output->x, output->y};
}

int CaptureClient::outputTransform() const { const auto* output=impl_->output(); return output==nullptr?static_cast<int>(protocol::WL_OUTPUT_TRANSFORM_NORMAL):output->transform; }
quint32 CaptureClient::outputRefreshMillihz() const { const auto* output=impl_->output(); return output==nullptr?0:output->refresh; }
quint32 CaptureClient::cursorModes() const { return impl_->cursorModes; }

QString CaptureClient::errorString() const { return impl_->error; }
