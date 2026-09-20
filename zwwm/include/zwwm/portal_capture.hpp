#pragma once


#include <zwayland/server/display.hpp>
#include <functional>
#include <memory>
#include <string>

#include "zwwm/output_capture.hpp"
#include "zwwm/compositor_server.hpp"
namespace zwwm {

class PortalCapture {
 public:
  using Capture = std::function<bool(OutputCapture&, bool, std::uint64_t)>;
  using Copy = std::function<bool(zwayland::server::Resource*, const OutputCapture&, std::uint32_t, std::uint32_t,
                                   std::uint32_t, std::uint32_t)>;
  using MapRegion = std::function<bool(const OutputCapture&, std::int32_t, std::int32_t, std::int32_t,
                                       std::int32_t, std::uint32_t*, std::uint32_t*, std::uint32_t*,
                                       std::uint32_t*)>;
  using Toplevels = std::function<std::vector<ToplevelInfo>()>;
  using Output = std::function<zwayland::server::Resource*(zwayland::server::Client*, OutputId)>;
  using ResolveOutput = std::function<std::optional<OutputInfo>(zwayland::server::Resource*)>;
  using PortalClient = std::function<void(zwayland::server::Client*)>;

  PortalCapture(zwayland::server::Display* display, zwayland::server::EventLoop* event_loop, Capture capture, Copy copy,
                 MapRegion map_region, Toplevels toplevels, Output output,
                 ResolveOutput resolve_output, bool embedded_cursor, PortalClient portal_client = {});
  ~PortalCapture();
  PortalCapture(const PortalCapture&) = delete;
  PortalCapture& operator=(const PortalCapture&) = delete;

  [[nodiscard]] bool spawn(const std::string& executable);
  [[nodiscard]] const std::string& last_error() const;
  void toplevels_changed();
  void frame_presented();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace zwwm
