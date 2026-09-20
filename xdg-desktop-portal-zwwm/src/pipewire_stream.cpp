#include "pipewire_stream.hpp"

#include <drm_fourcc.h>
#include <gbm.h>
#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/param/buffers.h>
#include <spa/param/format-utils.h>
#include <spa/param/video/format-utils.h>
#include <spa/utils/result.h>
#include <xf86drm.h>
#include <zwayland/wire/message.hpp>

#include <QMetaObject>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <fcntl.h>
#include <linux/memfd.h>
#include <limits>
#include <mutex>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

void addIdProperty(spa_pod_builder& builder, std::uint32_t key, std::uint32_t value) {
  spa_pod_builder_prop(&builder, key, 0);
  spa_pod_builder_id(&builder, value);
}

void addIntProperty(spa_pod_builder& builder, std::uint32_t key, std::int32_t value) {
  spa_pod_builder_prop(&builder, key, 0);
  spa_pod_builder_int(&builder, value);
}

void addLongProperty(spa_pod_builder& builder, std::uint32_t key, std::int64_t value) {
  spa_pod_builder_prop(&builder, key, SPA_POD_PROP_FLAG_MANDATORY);
  spa_pod_builder_long(&builder, value);
}

void addIntChoiceProperty(spa_pod_builder& builder, std::uint32_t key, std::uint32_t choiceType,
                          std::span<const std::int32_t> values) {
  spa_pod_builder_prop(&builder, key, 0);
  spa_pod_frame choice{};
  spa_pod_builder_push_choice(&builder, &choice, choiceType, 0);
  for (const auto value : values) spa_pod_builder_int(&builder, value);
  spa_pod_builder_pop(&builder, &choice);
}

const spa_pod* buildBufferParams(spa_pod_builder& builder, std::int32_t bytes, std::int32_t stride,
                                  bool dmaBuf) {
  spa_pod_frame object{};
  spa_pod_builder_push_object(&builder, &object, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers);
  const std::array<std::int32_t, 3> bufferCounts{4, 2, 8};
  addIntChoiceProperty(builder, SPA_PARAM_BUFFERS_buffers, SPA_CHOICE_Range, bufferCounts);
  addIntProperty(builder, SPA_PARAM_BUFFERS_blocks, 1);
  addIntProperty(builder, SPA_PARAM_BUFFERS_size, bytes);
  addIntProperty(builder, SPA_PARAM_BUFFERS_stride, stride);
  addIntProperty(builder, SPA_PARAM_BUFFERS_align, 16);
  const std::array<std::int32_t, 1> dataTypes{1 << (dmaBuf ? SPA_DATA_DmaBuf : SPA_DATA_MemFd)};
  addIntChoiceProperty(builder, SPA_PARAM_BUFFERS_dataType, SPA_CHOICE_Flags, dataTypes);
  return static_cast<const spa_pod*>(spa_pod_builder_pop(&builder, &object));
}

const spa_pod* buildMetaParams(spa_pod_builder& builder) {
  spa_pod_frame object{};
  spa_pod_builder_push_object(&builder, &object, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta);
  addIdProperty(builder, SPA_PARAM_META_type, SPA_META_Header);
  addIntProperty(builder, SPA_PARAM_META_size, static_cast<std::int32_t>(sizeof(spa_meta_header)));
  return static_cast<const spa_pod*>(spa_pod_builder_pop(&builder, &object));
}

const spa_pod* buildFormatParams(spa_pod_builder& builder, const spa_rectangle& frameSize,
                                 bool dmaBuf) {
  spa_pod_frame object{};
  spa_pod_builder_push_object(&builder, &object, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
  addIdProperty(builder, SPA_FORMAT_mediaType, SPA_MEDIA_TYPE_video);
  addIdProperty(builder, SPA_FORMAT_mediaSubtype, SPA_MEDIA_SUBTYPE_raw);
  addIdProperty(builder, SPA_FORMAT_VIDEO_format, SPA_VIDEO_FORMAT_BGRx);
  if (dmaBuf) addLongProperty(builder, SPA_FORMAT_VIDEO_modifier, DRM_FORMAT_MOD_LINEAR);
  spa_pod_builder_prop(&builder, SPA_FORMAT_VIDEO_size, 0);
  spa_pod_builder_rectangle(&builder, frameSize.width, frameSize.height);
  spa_pod_builder_prop(&builder, SPA_FORMAT_VIDEO_framerate, 0);
  spa_pod_frame choice{};
  spa_pod_builder_push_choice(&builder, &choice, SPA_CHOICE_Range, 0);
  constexpr std::array<spa_fraction, 3> rates{{{60, 1}, {1, 1}, {60, 1}}};
  for (const auto& rate : rates) spa_pod_builder_fraction(&builder, rate.num, rate.denom);
  spa_pod_builder_pop(&builder, &choice);
  return static_cast<const spa_pod*>(spa_pod_builder_pop(&builder, &object));
}

}  // namespace

struct PipeWireStream::Impl {
  struct Allocation {
    zwayland::wire::FileDescriptor fd;
    void* mapping = nullptr;
    std::size_t size = 0;
    gbm_bo* bo = nullptr;
    std::uint32_t stride = 0;

    Allocation() = default;
    Allocation(const Allocation&) = delete;
    Allocation& operator=(const Allocation&) = delete;
    Allocation(Allocation&& other) noexcept
        : fd(std::move(other.fd)), mapping(std::exchange(other.mapping, nullptr)),
          size(std::exchange(other.size, 0)), bo(std::exchange(other.bo, nullptr)),
          stride(std::exchange(other.stride, 0)) {}
    Allocation& operator=(Allocation&& other) noexcept {
      if (this == &other) return *this;
      release();
      fd = std::move(other.fd);
      mapping = std::exchange(other.mapping, nullptr);
      size = std::exchange(other.size, 0);
      bo = std::exchange(other.bo, nullptr);
      stride = std::exchange(other.stride, 0);
      return *this;
    }
    ~Allocation() { release(); }

    void release() {
      if (mapping != nullptr) munmap(mapping, size);
      if (bo != nullptr) gbm_bo_destroy(bo);
      mapping = nullptr;
      bo = nullptr;
      size = 0;
      stride = 0;
      fd = zwayland::wire::FileDescriptor{};
    }
  };
  PipeWireStream* q = nullptr;
  QSize size;
  pw_thread_loop* loop = nullptr;
  pw_context* context = nullptr;
  pw_core* core = nullptr;
  pw_stream* stream = nullptr;
  std::vector<std::pair<zwayland::wire::FileDescriptor, gbm_device*>> gbmDevices;
  spa_hook listener{};
  std::atomic<quint32> node{PW_ID_ANY};
  std::atomic<quint64> sequence{0};
  std::atomic<bool> dmaBufFormat{false};
  QString error;
  std::unordered_map<pw_buffer*, Allocation> allocations;

  bool initializeDmaBuf() {
    std::array<drmDevicePtr, 64> devices{};
    const int count = drmGetDevices2(0, devices.data(), static_cast<int>(devices.size()));
    if (count < 0) return false;
    for (int index = 0; index < count; ++index) {
      const auto* device = devices[static_cast<std::size_t>(index)];
      if (device == nullptr || (device->available_nodes & (1 << DRM_NODE_RENDER)) == 0) continue;
      zwayland::wire::FileDescriptor fd(open(device->nodes[DRM_NODE_RENDER], O_RDWR | O_CLOEXEC));
      if (fd.get() < 0) continue;
      gbm_device* gbm = gbm_create_device(fd.get());
      if (gbm == nullptr) {
        continue;
      }
      gbmDevices.emplace_back(std::move(fd), gbm);
    }
    drmFreeDevices(devices.data(), count);
    return !gbmDevices.empty();
  }

  bool allocateDmaBuf(pw_buffer* wrapper, spa_data& plane) {
    for (const auto& [deviceFd, gbm] : gbmDevices) {
      (void)deviceFd;
      constexpr std::uint64_t modifier = DRM_FORMAT_MOD_LINEAR;
      gbm_bo* bo = gbm_bo_create_with_modifiers2(
          gbm, static_cast<std::uint32_t>(size.width()), static_cast<std::uint32_t>(size.height()),
          GBM_FORMAT_XRGB8888, &modifier, 1, GBM_BO_USE_RENDERING);
      if (bo == nullptr) continue;
      if (gbm_bo_get_modifier(bo) != modifier) {
        gbm_bo_destroy(bo);
        continue;
      }
      Allocation allocation;
      allocation.fd = zwayland::wire::FileDescriptor(gbm_bo_get_fd(bo));
      allocation.bo = bo;
      const std::uint32_t stride = gbm_bo_get_stride(bo);
      const std::size_t bytes = static_cast<std::size_t>(stride) * size.height();
      if (allocation.fd.get() < 0 || stride < static_cast<std::uint32_t>(size.width() * 4) ||
          bytes > std::numeric_limits<std::uint32_t>::max()) {
        continue;
      }
      std::uint32_t mapStride = 0;
      void* mapData = nullptr;
      void* mapping = gbm_bo_map(bo, 0, 0, static_cast<std::uint32_t>(size.width()),
                                 static_cast<std::uint32_t>(size.height()), GBM_BO_TRANSFER_WRITE,
                                 &mapStride, &mapData);
      if (mapping == nullptr || mapping == MAP_FAILED || mapStride < size.width() * 4U) {
        if (mapping != nullptr && mapping != MAP_FAILED) gbm_bo_unmap(bo, mapData);
        continue;
      }
      gbm_bo_unmap(bo, mapData);
      allocation.size = bytes;
      allocation.stride = stride;
      const auto [stored, inserted] = allocations.try_emplace(wrapper, std::move(allocation));
      if (!inserted) return false;
      plane.type = SPA_DATA_DmaBuf;
      plane.flags = SPA_DATA_FLAG_READABLE | SPA_DATA_FLAG_WRITABLE | SPA_DATA_FLAG_MAPPABLE;
      plane.fd = stored->second.fd.get();
      plane.mapoffset = 0;
      plane.maxsize = static_cast<std::uint32_t>(bytes);
      plane.data = nullptr;
      if (plane.chunk != nullptr) {
        plane.chunk->offset = 0;
        plane.chunk->size = 0;
        plane.chunk->stride = static_cast<std::int32_t>(stride);
        plane.chunk->flags = SPA_CHUNK_FLAG_NONE;
      }
      return true;
    }
    return false;
  }

  bool allocateMemFd(pw_buffer* wrapper, spa_data& plane) {
    const std::size_t bytes = static_cast<std::size_t>(size.width()) * size.height() * 4U;
    Allocation allocation;
    allocation.fd = zwayland::wire::FileDescriptor(
        static_cast<int>(syscall(SYS_memfd_create, "zwwm-pipewire-frame", MFD_CLOEXEC)));
    if (allocation.fd.get() < 0 || ftruncate(allocation.fd.get(), static_cast<off_t>(bytes)) != 0)
      return false;
    allocation.mapping = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, allocation.fd.get(), 0);
    if (allocation.mapping == MAP_FAILED) { allocation.mapping = nullptr; return false; }
    allocation.size = bytes;
    allocation.stride = static_cast<std::uint32_t>(size.width() * 4);
    const auto [stored, inserted] = allocations.try_emplace(wrapper, std::move(allocation));
    if (!inserted) return false;
    plane.type = SPA_DATA_MemFd;
    plane.flags = SPA_DATA_FLAG_READABLE | SPA_DATA_FLAG_WRITABLE | SPA_DATA_FLAG_MAPPABLE;
    plane.fd = stored->second.fd.get();
    plane.mapoffset = 0;
    plane.maxsize = static_cast<std::uint32_t>(bytes);
    plane.data = stored->second.mapping;
    const std::uint32_t stride = static_cast<std::uint32_t>(size.width() * 4);
    if (plane.chunk != nullptr) {
      plane.chunk->offset = 0;
      plane.chunk->size = 0;
      plane.chunk->stride = static_cast<std::int32_t>(stride);
      plane.chunk->flags = SPA_CHUNK_FLAG_NONE;
    }
    return true;
  }

  static void stateChanged(void* data, pw_stream_state, pw_stream_state state, const char* message) {
    auto* self = static_cast<Impl*>(data);
    if (state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_STREAMING) {
      const quint32 id = pw_stream_get_node_id(self->stream);
      const bool first = id != PW_ID_ANY && self->node.exchange(id) == PW_ID_ANY;
      if (first)
        QMetaObject::invokeMethod(self->q, "ready", Qt::QueuedConnection);
      if (first || state == PW_STREAM_STATE_STREAMING)
        QMetaObject::invokeMethod(self->q, "frameNeeded", Qt::QueuedConnection);
    } else if (state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED) {
      if (message != nullptr) self->error = QString::fromUtf8(message);
      QMetaObject::invokeMethod(self->q, "failed", Qt::QueuedConnection);
    }
  }
  static void paramChanged(void* data, std::uint32_t id, const spa_pod* param) {
    auto* self = static_cast<Impl*>(data);
    if (id != SPA_PARAM_Format || param == nullptr) return;
    const bool dmaBuf = spa_pod_find_prop(param, nullptr, SPA_FORMAT_VIDEO_modifier) != nullptr;
    self->dmaBufFormat.store(dmaBuf);
    std::uint8_t storage[512];
    spa_pod_builder builder{};
    spa_pod_builder_init(&builder, storage, sizeof(storage));
    const int stride = self->size.width() * 4;
    const int bytes = stride * self->size.height();
    const spa_pod* params[2];
    params[0] = buildBufferParams(builder, bytes, stride, dmaBuf);
    params[1] = buildMetaParams(builder);
    pw_stream_update_params(self->stream, params, 2);
  }
  static void process(void* data) {
    auto* self = static_cast<Impl*>(data);
    QMetaObject::invokeMethod(self->q, "frameNeeded", Qt::QueuedConnection);
  }
  static void addBuffer(void* data, pw_buffer* wrapper) {
    auto* self = static_cast<Impl*>(data);
    if (wrapper == nullptr || wrapper->buffer == nullptr || wrapper->buffer->n_datas == 0) return;
    spa_data& plane = wrapper->buffer->datas[0];
    if (self->dmaBufFormat.load()) {
      if (self->gbmDevices.empty() || !self->allocateDmaBuf(wrapper, plane))
        QMetaObject::invokeMethod(self->q, "failed", Qt::QueuedConnection);
      return;
    }
    if (!self->allocateMemFd(wrapper, plane))
      QMetaObject::invokeMethod(self->q, "failed", Qt::QueuedConnection);
  }
  static void removeBuffer(void* data, pw_buffer* wrapper) {
    auto* self = static_cast<Impl*>(data);
    const auto found = self->allocations.find(wrapper);
    if (found == self->allocations.end()) return;
    if (wrapper != nullptr && wrapper->buffer != nullptr && wrapper->buffer->n_datas != 0) {
      wrapper->buffer->datas[0].fd = -1;
      wrapper->buffer->datas[0].data = nullptr;
    }
    self->allocations.erase(found);
  }
};

PipeWireStream::PipeWireStream(QSize size, QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>()) {
  impl_->q = this;
  impl_->size = size;
}

PipeWireStream::~PipeWireStream() {
  if (impl_->loop != nullptr) pw_thread_loop_stop(impl_->loop);
  if (impl_->stream != nullptr) pw_stream_destroy(impl_->stream);
  if (impl_->core != nullptr) pw_core_disconnect(impl_->core);
  if (impl_->context != nullptr) pw_context_destroy(impl_->context);
  if (impl_->loop != nullptr) pw_thread_loop_destroy(impl_->loop);
  for (const auto& [fd, gbm] : impl_->gbmDevices) {
    (void)fd;
    gbm_device_destroy(gbm);
  }
  impl_->gbmDevices.clear();
}

bool PipeWireStream::start() {
  if (!impl_->size.isValid() || impl_->size.width() > INT32_MAX / 4) return false;
  static std::once_flag initialized;
  std::call_once(initialized, [] { pw_init(nullptr, nullptr); });
  (void)impl_->initializeDmaBuf();
  impl_->loop = pw_thread_loop_new("zwwm-screencast", nullptr);
  impl_->context = impl_->loop == nullptr ? nullptr : pw_context_new(pw_thread_loop_get_loop(impl_->loop), nullptr, 0);
  impl_->core = impl_->context == nullptr ? nullptr : pw_context_connect(impl_->context, nullptr, 0);
  if (impl_->core == nullptr) {
    impl_->error = QStringLiteral("could not connect to the PipeWire daemon");
    return false;
  }
  pw_properties* properties = pw_properties_new(
      PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Screen",
      PW_KEY_NODE_NAME, "zwwm-screencast", PW_KEY_NODE_DESCRIPTION, "zwwm screen cast", nullptr);
  impl_->stream = pw_stream_new(impl_->core, "zwwm screen cast", properties);
  if (impl_->stream == nullptr) return false;
  static const pw_stream_events events = [] {
    pw_stream_events value{};
    value.version = PW_VERSION_STREAM_EVENTS;
    value.state_changed = Impl::stateChanged;
    value.param_changed = Impl::paramChanged;
    value.add_buffer = Impl::addBuffer;
    value.remove_buffer = Impl::removeBuffer;
    value.process = Impl::process;
    return value;
  }();
  pw_stream_add_listener(impl_->stream, &impl_->listener, &events, impl_.get());

  std::uint8_t storage[1024];
  spa_pod_builder builder{};
  spa_pod_builder_init(&builder, storage, sizeof(storage));
  const spa_rectangle frameSize{static_cast<std::uint32_t>(impl_->size.width()),
                                static_cast<std::uint32_t>(impl_->size.height())};
  const spa_pod* params[2];
  std::uint32_t paramCount = 0;
  if (!impl_->gbmDevices.empty()) params[paramCount++] = buildFormatParams(builder, frameSize, true);
  params[paramCount++] = buildFormatParams(builder, frameSize, false);

  if (pw_thread_loop_start(impl_->loop) < 0) return false;
  pw_thread_loop_lock(impl_->loop);
  const int result = pw_stream_connect(impl_->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
       static_cast<pw_stream_flags>(PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_ALLOC_BUFFERS), params, paramCount);
  pw_thread_loop_unlock(impl_->loop);
  if (result < 0) {
    impl_->error = QString::fromUtf8(spa_strerror(result));
    return false;
  }
  return true;
}

bool PipeWireStream::submit(const QImage& source, quint64 seconds, quint32 nanoseconds) {
  if (impl_->stream == nullptr || impl_->loop == nullptr || source.size() != impl_->size) return false;
  const QImage image = source.convertToFormat(QImage::Format_RGB32);
  pw_thread_loop_lock(impl_->loop);
  pw_buffer* wrapper = pw_stream_dequeue_buffer(impl_->stream);
  if (wrapper == nullptr || wrapper->buffer == nullptr || wrapper->buffer->n_datas == 0) {
    pw_thread_loop_unlock(impl_->loop);
    return false;
  }
  spa_buffer* buffer = wrapper->buffer;
  spa_data& data = buffer->datas[0];
  const auto allocation = impl_->allocations.find(wrapper);
  if (allocation == impl_->allocations.end() || data.chunk == nullptr) {
    pw_stream_queue_buffer(impl_->stream, wrapper);
    pw_thread_loop_unlock(impl_->loop);
    return false;
  }
  const std::uint32_t stride = allocation->second.stride;
  const std::uint32_t bytes = stride * static_cast<std::uint32_t>(impl_->size.height());
  void* mapping = allocation->second.mapping;
  void* mapData = nullptr;
  std::uint32_t mapStride = 0;
  if (allocation->second.bo != nullptr) {
    mapping = gbm_bo_map(allocation->second.bo, 0, 0,
                         static_cast<std::uint32_t>(impl_->size.width()),
                         static_cast<std::uint32_t>(impl_->size.height()), GBM_BO_TRANSFER_WRITE,
                         &mapStride, &mapData);
  }
  if (mapping == nullptr || mapping == MAP_FAILED || allocation->second.size < bytes ||
      data.maxsize < bytes) {
    if (allocation->second.bo != nullptr && mapping != nullptr && mapping != MAP_FAILED)
      gbm_bo_unmap(allocation->second.bo, mapData);
    pw_stream_queue_buffer(impl_->stream, wrapper);
    pw_thread_loop_unlock(impl_->loop);
    return false;
  }
  const std::uint32_t destinationStride = allocation->second.bo != nullptr ? mapStride : stride;
  for (int row = 0; row < image.height(); ++row)
    std::memcpy(static_cast<std::uint8_t*>(mapping) +
                    static_cast<std::size_t>(row) * destinationStride,
                image.constScanLine(row), static_cast<std::size_t>(image.width()) * 4U);
  if (allocation->second.bo != nullptr) gbm_bo_unmap(allocation->second.bo, mapData);
  data.chunk->offset = 0;
  data.chunk->size = bytes;
  data.chunk->stride = static_cast<std::int32_t>(stride);
  data.chunk->flags = SPA_CHUNK_FLAG_NONE;
  if (auto* header = static_cast<spa_meta_header*>(spa_buffer_find_meta_data(buffer, SPA_META_Header,
                                                                             sizeof(spa_meta_header)));
      header != nullptr) {
    header->flags = 0;
    header->seq = static_cast<std::uint32_t>(impl_->sequence.fetch_add(1));
    header->pts = static_cast<std::int64_t>(seconds * 1000000000ULL + nanoseconds);
    header->dts_offset = 0;
  }
  pw_stream_queue_buffer(impl_->stream, wrapper);
  pw_thread_loop_unlock(impl_->loop);
  return true;
}

quint32 PipeWireStream::nodeId() const { return impl_->node.load(); }
QString PipeWireStream::errorString() const { return impl_->error; }
