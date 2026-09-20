#include "compositor_server_internal.hpp"

#include <linux-dmabuf-zwayland-server.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <linux/memfd.h>
#include <new>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace protocol = zwayland::generated;

namespace zwwm::detail {
namespace {

struct Params {
  DmabufState* state = nullptr;
  std::vector<renderer::DmabufPlane> planes;
  bool used = false;
};

void destroy_pool(Pool* pool) {
  if (pool->mapping != nullptr) (void)munmap(pool->mapping, pool->size);
  delete pool;
}

bool valid_pool_size(int fd, std::size_t size) {
  struct stat status {};
  return size > 0 && fstat(fd, &status) == 0 && status.st_size >= 0 &&
         static_cast<std::uint64_t>(status.st_size) >= size;
}

void buffer_destroyed(zwayland::server::Resource* resource) {
  auto* buffer = resource->data<Buffer>();
  if (buffer != nullptr) {
    buffer->resource = nullptr;
    unref_buffer(buffer);
  }
}

void pool_destroyed(zwayland::server::Resource* resource) {
  auto* pool = resource->data<Pool>();
  if (pool != nullptr) {
    pool->destroyed = true;
    if (pool->buffers == 0) destroy_pool(pool);
  }
}

void pool_resize(zwayland::server::Client*, zwayland::server::Resource* resource, std::int32_t size) {
  auto* pool = resource->data<Pool>();
  if (size < 0 || static_cast<std::size_t>(size) < pool->size) {
    resource->post_error(protocol::WL_SHM_ERROR_INVALID_FD, "wl_shm pool cannot shrink");
    return;
  }
  if (static_cast<std::size_t>(size) == pool->size) return;
  if (!valid_pool_size(pool->fd.get(), static_cast<std::size_t>(size))) {
    resource->post_error(protocol::WL_SHM_ERROR_INVALID_FD, "wl_shm backing file is smaller than the pool");
    return;
  }
  void* mapping = mmap(nullptr, static_cast<std::size_t>(size), PROT_READ, MAP_SHARED, pool->fd.get(), 0);
  if (mapping == MAP_FAILED) {
    resource->post_error(protocol::WL_SHM_ERROR_INVALID_FD, "could not grow wl_shm pool");
    return;
  }
  (void)munmap(pool->mapping, pool->size);
  pool->mapping = mapping;
  pool->size = static_cast<std::size_t>(size);
}

struct WlBufferKShmBufferHandler {
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource) { resource->destroy(); })(&client, &resource);
  }
};

void create_buffer(zwayland::server::Client* client, zwayland::server::Resource* resource, std::uint32_t id, std::int32_t offset,
                   std::int32_t width, std::int32_t height, std::int32_t stride, std::uint32_t format) {
  auto* pool = resource->data<Pool>();
  const bool valid = offset >= 0 && width > 0 && height > 0 &&
                     width <= std::numeric_limits<std::int32_t>::max() / 4 && stride >= width * 4 &&
                     (format == protocol::WL_SHM_FORMAT_ARGB8888 || format == protocol::WL_SHM_FORMAT_XRGB8888);
  const auto end = valid ? static_cast<std::uint64_t>(offset) + static_cast<std::uint64_t>(stride) * height
                         : std::numeric_limits<std::uint64_t>::max();
  if (!valid || end > pool->size) {
    resource->post_error(protocol::WL_SHM_ERROR_INVALID_STRIDE, "unsupported wl_shm buffer");
    return;
  }
  auto* buffer_resource = client->create_resource(&protocol::wl_buffer_interface, id, 1);
  if (buffer_resource == nullptr) {
    client->post_no_memory();
    return;
  }
  auto* buffer = new (std::nothrow) Buffer;
  if (buffer != nullptr) {
    buffer->pool = pool;
    buffer->resource = buffer_resource;
    buffer->offset = offset;
    buffer->width = width;
    buffer->height = height;
    buffer->stride = stride;
    buffer->format = format;
  }
  if (buffer == nullptr) {
    buffer_resource->destroy();
    client->post_no_memory();
    return;
  }
  ++pool->buffers;
  buffer_resource->set_data(buffer); buffer_resource->set_handler(protocol::wl_buffer_handler(WlBufferKShmBufferHandler{})); buffer_resource->set_destroy_handler([](zwayland::server::Resource& destroyed) { (buffer_destroyed)(&destroyed); });
}

void create_pool(zwayland::server::Client* client, zwayland::server::Resource*, std::uint32_t id, int fd, std::int32_t size) {
  UniqueFd owned_fd(fd);
  if (!owned_fd || size <= 0 || !valid_pool_size(owned_fd.get(), static_cast<std::size_t>(size))) {
    client->post_error(0, "invalid wl_shm pool");
    return;
  }
  void* mapping = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, owned_fd.get(), 0);
  if (mapping == MAP_FAILED) {
    client->post_error(0, "could not map wl_shm pool");
    return;
  }
  auto* resource = client->create_resource(&protocol::wl_shm_pool_interface, id, 1);
  auto* pool = resource == nullptr ? nullptr :
      new (std::nothrow) Pool{mapping, static_cast<std::size_t>(size), std::move(owned_fd)};
  if (resource == nullptr || pool == nullptr) {
    if (resource != nullptr) resource->destroy();
    if (mapping != MAP_FAILED) (void)munmap(mapping, size);
    client->post_no_memory();
    return;
  }
  struct WlShmPoolImplementationHandler {
  void create_buffer(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t id, std::int32_t offset, std::int32_t width, std::int32_t height, std::int32_t stride, std::uint32_t format) {
    (::zwwm::detail::create_buffer)(&client, &resource, id, offset, width, height, stride, format);
  }
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* item) { item->destroy(); })(&client, &resource);
  }
  void resize(zwayland::server::Client& client, zwayland::server::Resource& resource, std::int32_t size) {
    (pool_resize)(&client, &resource, size);
  }
};
  resource->set_data(pool); resource->set_handler(protocol::wl_shm_pool_handler(WlShmPoolImplementationHandler{})); resource->set_destroy_handler([](zwayland::server::Resource& destroyed) { (pool_destroyed)(&destroyed); });
}

struct WlShmKShmHandler {
  void create_pool(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t id, int fd, std::int32_t size) {
    (::zwwm::detail::create_pool)(&client, &resource, id, fd, size);
  }
  void release(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource) { resource->destroy(); })(&client, &resource);
  }
};

void params_destroyed(zwayland::server::Resource* resource) {
  auto* params = resource->data<Params>();
  if (params != nullptr) {
    for (const auto& plane : params->planes)
      if (plane.fd >= 0) (void)close(plane.fd);
    delete params;
  }
}

bool advertised(const DmabufState& state, std::uint32_t format, std::uint64_t modifier) {
  return std::find(state.formats.begin(), state.formats.end(), std::pair{format, modifier}) != state.formats.end();
}

void params_add(zwayland::server::Client*, zwayland::server::Resource* resource, std::int32_t fd, std::uint32_t index,
                 std::uint32_t offset, std::uint32_t stride, std::uint32_t hi, std::uint32_t lo) {
  auto* params = resource->data<Params>();
  UniqueFd owned_fd(fd);
  if (fd < 0 || index >= 4) {
    resource->post_error(protocol::ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_IDX, "invalid dma-buf plane");
    return;
  }
  if (params->used || index < params->planes.size()) {
    resource->post_error(params->used ? protocol::ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED
                                                   : protocol::ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_SET,
                            "dma-buf params already used or plane set");
    return;
  }
  if (index != params->planes.size()) {
    resource->post_error(protocol::ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE,
                            "dma-buf planes must be consecutive");
    return;
  }
  params->planes.push_back({owned_fd.get(), stride, offset, (static_cast<std::uint64_t>(hi) << 32U) | lo});
  (void)owned_fd.release();
}

Buffer* import_params(zwayland::server::Resource* resource, std::int32_t width, std::int32_t height,
                      std::uint32_t format, std::uint32_t flags) {
  auto* params = resource->data<Params>();
  if (params->used) {
    resource->post_error(protocol::ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,
                           "dma-buf params already used");
    return nullptr;
  }
  params->used = true;
  if (width <= 0 || height <= 0) {
    resource->post_error(protocol::ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_DIMENSIONS,
                           "invalid dma-buf dimensions");
    return nullptr;
  }
  if (flags != 0 || params->planes.empty() ||
      !advertised(*params->state, format, params->planes.front().modifier)) {
    resource->post_error(protocol::ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT,
                           "unsupported dma-buf format, modifier, or flags");
    return nullptr;
  }
  renderer::DmabufAttributes attributes{static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height),
                                        format, params->planes};
  if (!attributes.valid()) return nullptr;
  auto* buffer = new (std::nothrow) Buffer;
  if (buffer == nullptr) return nullptr;
  buffer->width = width;
  buffer->height = height;
  buffer->format = format;
  buffer->dmabuf.emplace(std::move(attributes));
  for (auto& plane : params->planes) plane.fd = -1;
  return buffer;
}

struct WlBufferKDmabufBufferHandler {
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource) { resource->destroy(); })(&client, &resource);
  }
};

void params_create(zwayland::server::Client* client, zwayland::server::Resource* resource, std::int32_t width, std::int32_t height,
                   std::uint32_t format, std::uint32_t flags) {
  Buffer* buffer = import_params(resource, width, height, format, flags);
  if (buffer == nullptr) {
    protocol::zwp_linux_buffer_params_v1_send_failed(*resource);
    return;
  }
  auto* buffer_resource = client->create_resource(&protocol::wl_buffer_interface, 0, 1);
  if (buffer_resource == nullptr) {
    unref_buffer(buffer);
    client->post_no_memory();
    return;
  }
  buffer->resource = buffer_resource;
  buffer_resource->set_data(buffer); buffer_resource->set_handler(protocol::wl_buffer_handler(WlBufferKDmabufBufferHandler{})); buffer_resource->set_destroy_handler([](zwayland::server::Resource& destroyed) { (buffer_destroyed)(&destroyed); });
  protocol::zwp_linux_buffer_params_v1_send_created(*resource, buffer_resource);
}

void params_create_immed(zwayland::server::Client* client, zwayland::server::Resource* resource, std::uint32_t id,
                         std::int32_t width, std::int32_t height, std::uint32_t format,
                         std::uint32_t flags) {
  Buffer* buffer = import_params(resource, width, height, format, flags);
  if (buffer == nullptr) {
    resource->post_error(protocol::ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_WL_BUFFER,
                           "could not import dma-buf");
    return;
  }
  auto* buffer_resource = client->create_resource(&protocol::wl_buffer_interface, id, 1);
  if (buffer_resource == nullptr) {
    unref_buffer(buffer);
    client->post_no_memory();
    return;
  }
  buffer->resource = buffer_resource;
  buffer_resource->set_data(buffer); buffer_resource->set_handler(protocol::wl_buffer_handler(WlBufferKDmabufBufferHandler{})); buffer_resource->set_destroy_handler([](zwayland::server::Resource& destroyed) { (buffer_destroyed)(&destroyed); });
}

struct ZwpLinuxBufferParamsV1KParamsHandler {
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource) { resource->destroy(); })(&client, &resource);
  }
  void add(zwayland::server::Client& client, zwayland::server::Resource& resource, int fd, std::uint32_t plane_idx, std::uint32_t offset, std::uint32_t stride, std::uint32_t modifier_hi, std::uint32_t modifier_lo) {
    (params_add)(&client, &resource, fd, plane_idx, offset, stride, modifier_hi, modifier_lo);
  }
  void create(zwayland::server::Client& client, zwayland::server::Resource& resource, std::int32_t width, std::int32_t height, std::uint32_t format, std::uint32_t flags) {
    (params_create)(&client, &resource, width, height, format, flags);
  }
  void create_immed(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t buffer_id, std::int32_t width, std::int32_t height, std::uint32_t format, std::uint32_t flags) {
    (params_create_immed)(&client, &resource, buffer_id, width, height, format, flags);
  }
  void set_sampling_device(zwayland::server::Client& client, zwayland::server::Resource& resource, std::span<const std::byte> device) {
    (void)client; (void)device; resource.post_error(1, "request is not implemented");
  }
};

void dmabuf_params(zwayland::server::Client* client, zwayland::server::Resource* resource, std::uint32_t id) {
  auto* state = resource->data<DmabufState>();
  auto* params_resource = client->create_resource(&protocol::zwp_linux_buffer_params_v1_interface, id, 3);
  auto* params = params_resource == nullptr ? nullptr : new (std::nothrow) Params;
  if (params != nullptr) params->state = state;
  if (params_resource == nullptr || params == nullptr) {
    if (params_resource != nullptr) params_resource->destroy();
    client->post_no_memory();
    return;
  }
  params_resource->set_data(params); params_resource->set_handler(protocol::zwp_linux_buffer_params_v1_handler(ZwpLinuxBufferParamsV1KParamsHandler{})); params_resource->set_destroy_handler([](zwayland::server::Resource& destroyed) { (params_destroyed)(&destroyed); });
}

struct FormatTableEntry {
  std::uint32_t format;
  std::uint32_t padding;
  std::uint64_t modifier;
};
static_assert(sizeof(FormatTableEntry) == 16);

UniqueFd create_format_table(const DmabufState& state) {
  const int fd = static_cast<int>(syscall(SYS_memfd_create, "zwwm-dmabuf-formats",
                                           MFD_CLOEXEC | MFD_ALLOW_SEALING));
  if (fd < 0) return {};
  UniqueFd result(fd);
  for (const auto& [format, modifier] : state.formats) {
    const FormatTableEntry entry{format, 0, modifier};
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&entry);
    std::size_t written = 0;
    while (written < sizeof(entry)) {
      const ssize_t count = write(result.get(), bytes + written, sizeof(entry) - written);
      if (count > 0) {
        written += static_cast<std::size_t>(count);
        continue;
      }
      if (count < 0 && errno == EINTR) continue;
      return {};
    }
  }
  if (fcntl(result.get(), F_ADD_SEALS, F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE) != 0)
    return {};
  return result;
}

struct ZwpLinuxDmabufFeedbackV1KFeedbackHandler {
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource) { resource->destroy(); })(&client, &resource);
  }
};

bool send_feedback(zwayland::server::Client* client, zwayland::server::Resource* feedback, const DmabufState& state) {
  const auto fd = create_format_table(state);
  if (!fd) {
    client->post_no_memory();
    return false;
  }
  const std::uint32_t size = static_cast<std::uint32_t>(state.formats.size() * sizeof(FormatTableEntry));
  const auto device = std::as_bytes(std::span(&*state.main_device, 1));
  std::vector<std::uint16_t> indices(state.formats.size());
  for (std::size_t index = 0; index < indices.size(); ++index)
    indices[index] = static_cast<std::uint16_t>(index);
  const auto tranche_formats = std::as_bytes(std::span(indices));
  protocol::zwp_linux_dmabuf_feedback_v1_send_format_table(*feedback, fd.get(), size);
  protocol::zwp_linux_dmabuf_feedback_v1_send_main_device(*feedback, device);
  protocol::zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(*feedback, device);
  protocol::zwp_linux_dmabuf_feedback_v1_send_tranche_flags(*feedback, 0);
  protocol::zwp_linux_dmabuf_feedback_v1_send_tranche_formats(*feedback, tranche_formats);
  protocol::zwp_linux_dmabuf_feedback_v1_send_tranche_done(*feedback);
  protocol::zwp_linux_dmabuf_feedback_v1_send_done(*feedback);
  return true;
}

void dmabuf_feedback(zwayland::server::Client* client, zwayland::server::Resource* manager, std::uint32_t id, zwayland::server::Resource* surface) {
  auto* state = manager->data<DmabufState>();
  auto* surface_state = surface == nullptr ? nullptr : surface->data<SurfaceState>();
  if (state == nullptr || !state->main_device.has_value() ||
      (surface != nullptr && (surface->client != client || surface_state == nullptr ||
                              surface_state->resource != surface))) {
    client->post_error(0, "invalid linux-dmabuf feedback request");
    return;
  }
  auto* feedback = client->create_resource(&protocol::zwp_linux_dmabuf_feedback_v1_interface, id, 1);
  if (feedback == nullptr) {
    client->post_no_memory();
    return;
  }
  feedback->set_handler(protocol::zwp_linux_dmabuf_feedback_v1_handler(ZwpLinuxDmabufFeedbackV1KFeedbackHandler{}));
  if (!send_feedback(client, feedback, *state)) feedback->destroy();
}

void dmabuf_default_feedback(zwayland::server::Client* client, zwayland::server::Resource* manager, std::uint32_t id) {
  dmabuf_feedback(client, manager, id, nullptr);
}

void dmabuf_surface_feedback(zwayland::server::Client* client, zwayland::server::Resource* manager, std::uint32_t id,
                             zwayland::server::Resource* surface) {
  dmabuf_feedback(client, manager, id, surface);
}

struct ZwpLinuxDmabufV1KDmabufHandler {
  void destroy(zwayland::server::Client& client, zwayland::server::Resource& resource) {
    ([](zwayland::server::Client*, zwayland::server::Resource* resource) { resource->destroy(); })(&client, &resource);
  }
  void create_params(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t params_id) {
    (dmabuf_params)(&client, &resource, params_id);
  }
  void get_default_feedback(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t id) {
    (dmabuf_default_feedback)(&client, &resource, id);
  }
  void get_surface_feedback(zwayland::server::Client& client, zwayland::server::Resource& resource, std::uint32_t id, zwayland::server::Resource* surface) {
    (dmabuf_surface_feedback)(&client, &resource, id, surface);
  }
};

}  // namespace

void send_buffer_release(Buffer* buffer) {
  if (buffer != nullptr && buffer->resource != nullptr) protocol::wl_buffer_send_release(*buffer->resource);
}

void unref_buffer(Buffer* buffer) {
  if (buffer == nullptr || --buffer->references != 0) return;
  Pool* pool = buffer->pool;
  if (pool != nullptr) {
    delete buffer;
    if (--pool->buffers == 0 && pool->destroyed) destroy_pool(pool);
  } else {
    if (buffer->dmabuf)
      for (const auto& plane : buffer->dmabuf->planes)
        if (plane.fd >= 0) (void)close(plane.fd);
    delete buffer;
  }
}

void bind_shm(zwayland::server::Client* client, void*, std::uint32_t version, std::uint32_t id) {
  auto* resource = client->create_resource(&protocol::wl_shm_interface, id, std::min(version, kShmVersion));
  if (resource == nullptr) {
    client->post_no_memory();
    return;
  }
  resource->set_handler(protocol::wl_shm_handler(WlShmKShmHandler{}));
  protocol::wl_shm_send_format(*resource, protocol::WL_SHM_FORMAT_ARGB8888);
  protocol::wl_shm_send_format(*resource, protocol::WL_SHM_FORMAT_XRGB8888);
}

void bind_dmabuf(zwayland::server::Client* client, void* data, std::uint32_t requested_version, std::uint32_t id) {
  auto* state = static_cast<DmabufState*>(data);
  const auto version = std::min(requested_version, state->main_device.has_value() ? 4U : 3U);
  auto* resource = client->create_resource(&protocol::zwp_linux_dmabuf_v1_interface, id, version);
  if (resource == nullptr) {
    client->post_no_memory();
    return;
  }
  resource->set_data(state); resource->set_handler(protocol::zwp_linux_dmabuf_v1_handler(ZwpLinuxDmabufV1KDmabufHandler{}));
  if (version >= 4) return;
  for (const auto& [format, modifier] : state->formats) {
    if (version >= 3)
      protocol::zwp_linux_dmabuf_v1_send_modifier(*resource, format, static_cast<std::uint32_t>(modifier >> 32U),
                                        static_cast<std::uint32_t>(modifier));
    else
      protocol::zwp_linux_dmabuf_v1_send_format(*resource, format);
  }
}

bool copy_shm_capture_buffer(zwayland::server::Resource* resource, const OutputCapture& capture,
                             std::uint32_t source_x, std::uint32_t source_y,
                             std::uint32_t width, std::uint32_t height) {
  if (resource == nullptr || capture.width == 0 || capture.height == 0 || width == 0 || height == 0 ||
      source_x > capture.width || source_y > capture.height || width > capture.width - source_x ||
      height > capture.height - source_y)
    return false;
  if (resource->interface != &protocol::wl_buffer_interface) return false;
  auto* buffer = resource->data<Buffer>();
  if (buffer == nullptr || buffer->resource != resource || buffer->pool == nullptr ||
      buffer->format != protocol::WL_SHM_FORMAT_XRGB8888 || buffer->width != static_cast<std::int32_t>(width) ||
      buffer->height != static_cast<std::int32_t>(height) ||
      buffer->stride != static_cast<std::int32_t>(width * 4U) || buffer->offset < 0 ||
      buffer->pool->mapping == nullptr ||
      capture.pixels.size() != static_cast<std::size_t>(capture.width) * capture.height)
    return false;
  const auto bytes = static_cast<std::size_t>(buffer->stride) * height;
  if (static_cast<std::size_t>(buffer->offset) > buffer->pool->size ||
      bytes > buffer->pool->size - static_cast<std::size_t>(buffer->offset))
    return false;
  auto* destination = static_cast<std::uint8_t*>(buffer->pool->mapping) + buffer->offset;
  for (std::uint32_t row = 0; row < height; ++row)
    std::memcpy(destination + static_cast<std::size_t>(row) * buffer->stride,
                capture.pixels.data() + static_cast<std::size_t>(source_y + row) * capture.width + source_x,
                static_cast<std::size_t>(width) * sizeof(std::uint32_t));
  return true;
}

}  // namespace zwwm::detail
