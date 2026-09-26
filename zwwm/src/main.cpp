#include "zwwm/compositor_server.hpp"
#include "zwwm/manager_protocol.hpp"
#include "zwwm/nested_backend.hpp"
#include "zwwm/runtime_backend.hpp"
#include "zwwm/runtime_config.hpp"
#include "zwwm/portal_capture.hpp"

#include <zwayland/server/display.hpp>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <utility>
#include <string>
#include <string_view>

namespace {
bool apply_environment(const zwwm::RuntimeConfig& config) {
  for (const auto& [name, value] : config.environment) {
    if (setenv(name.c_str(), value.c_str(), 1) != 0) {
      std::fprintf(stderr, "zwwm: could not set environment variable %s: %d\n", name.c_str(), errno);
      return false;
    }
  }
  unsetenv("WAYLAND_DEBUG");
  return true;
}
}

int main(int argc, char** argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--version") {
    std::printf("zwwm %s\n", ZWWM_VERSION);
    return EXIT_SUCCESS;
  }
  // A clipboard recipient may close its pipe during a transfer. Let write()
  // report EPIPE so transfer cleanup runs instead of terminating the session.
  struct sigaction pipe_action {};
  pipe_action.sa_handler = SIG_IGN;
  sigemptyset(&pipe_action.sa_mask);
  if (sigaction(SIGPIPE, &pipe_action, nullptr) != 0) {
    std::fprintf(stderr, "zwwm: could not ignore SIGPIPE: %d\n", errno);
    return EXIT_FAILURE;
  }

  std::string portal_executable = ZWWM_PORTAL_EXECUTABLE;
  for (int index = 1; index < argc; ++index) {
    const std::string argument(argv[index]);
    if (argument == "--portal-helper" && index + 1 < argc) portal_executable = argv[++index];
    else if (argument == "--no-portal") portal_executable.clear();
    else if (argument == "--version") {
      std::printf("zwwm %s\n", ZWWM_VERSION);
      return EXIT_SUCCESS;
    }
    else {
      std::fprintf(stderr, "usage: %s [--portal-helper PATH | --no-portal] [--version]\n", argv[0]);
      return EXIT_FAILURE;
    }
  }
  zwwm::LoadedRuntimeConfig loaded;
  try {
    loaded = zwwm::load_runtime_config();
  } catch (const std::exception& exception) {
    std::fprintf(stderr, "zwwm: %s\n", exception.what());
    return EXIT_FAILURE;
  }
  if (loaded.error.empty() && !loaded.path.empty() && std::filesystem::exists(loaded.path)) {
    std::printf("zwwm: loaded configuration %s\n", loaded.path.c_str());
  } else if (!loaded.error.empty()) {
    std::fprintf(stderr, "zwwm: using built-in defaults until the configuration is fixed\n");
  }
  if (!apply_environment(*loaded.config)) return EXIT_FAILURE;

  std::unique_ptr<zwayland::server::Display> display_owner;
  try {
    display_owner = std::make_unique<zwayland::server::Display>();
  } catch (const std::exception& exception) {
    std::fprintf(stderr, "zwwm: could not create Wayland display: %s\n", exception.what());
    return EXIT_FAILURE;
  }
  auto* display = display_owner.get();
  const auto& socket = display->socket_name();
  auto* event_loop = &display->event_loop();
  zwwm::CompositorServer compositor_server(display, loaded.config, event_loop);
  const auto run_startup_commands = [&] {
    for (const auto& command : loaded.config->startup_commands) {
      std::string error;
      if (!compositor_server.dispatch_action("exec", command, &error))
        std::fprintf(stderr, "zwwm: autostart failed for '%s': %s\n", command.c_str(), error.c_str());
    }
  };
  struct StartupState {
    zwayland::server::EventLoop* loop;
    decltype(run_startup_commands)* start;
    bool presented = false;
    bool armed = false;
    bool scheduled = false;

    void schedule() {
      if (!presented || !armed || scheduled) return;
      scheduled = true;
      loop->add_idle([this] { (*start)(); });
    }
    void on_frame() {
      presented = true;
      schedule();
    }
  } startup{event_loop, &run_startup_commands};
  struct PresentationState {
    StartupState* startup;
    zwwm::PortalCapture* portal = nullptr;
  } presentation{&startup};
  compositor_server.set_presentation_observer(
      [](void* data) {
        auto* state = static_cast<PresentationState*>(data);
        state->startup->on_frame();
        if (state->portal != nullptr) state->portal->frame_presented();
      }, &presentation);

  zwwm::NestedBackend nested_backend(event_loop, loaded.config);
  struct ConfigTargets {
    zwwm::CompositorServer* compositor;
    zwwm::NestedBackend* nested;
    zwwm::RuntimeBackend* runtime = nullptr;
    void show_error(const std::string& message) {
      if (runtime != nullptr) runtime->show_error(message);
      else nested->show_error(message);
    }
    void clear_error() {
      if (runtime != nullptr) runtime->clear_error();
      else nested->clear_error();
    }
    std::string rebuild_switch_shaders() {
      std::string error;
      const bool rebuilt = runtime != nullptr ? runtime->rebuild_switch_shaders(&error)
                                              : nested->rebuild_switch_shaders(&error);
      if (!rebuilt) show_error(error);
      else clear_error();
      return rebuilt ? std::string{} : error;
    }
    std::string apply_config(std::shared_ptr<const zwwm::RuntimeConfig> config) {
      compositor->set_config(config);
      if (runtime != nullptr) runtime->set_config(config);
      else nested->set_config(std::move(config));
      return rebuild_switch_shaders();
    }
    std::string set_cursor(const std::string& theme, std::uint32_t size) {
      std::string error;
      const bool applied = runtime != nullptr ? runtime->set_cursor_theme(theme, size, &error)
                                              : nested->set_cursor_theme(theme, size, &error);
      return applied ? std::string{} : error;
    }
  } config_targets{&compositor_server, &nested_backend};
  std::unique_ptr<zwwm::ConfigReloader> config_reloader;
  if (!loaded.path.empty()) {
    config_reloader = std::make_unique<zwwm::ConfigReloader>(event_loop, loaded.path, loaded.config,
        [](void* data, std::shared_ptr<const zwwm::RuntimeConfig> config) {
          auto* targets = static_cast<ConfigTargets*>(data);
          if (!apply_environment(*config)) {
            targets->show_error("Could not apply the configuration environment");
            return;
          }
          const auto shader_error = targets->apply_config(std::move(config));
          if (shader_error.empty()) targets->clear_error();
        }, &config_targets,
        [](void* data, const std::string& message) {
          static_cast<ConfigTargets*>(data)->show_error(message);
        });
    if (!config_reloader->start()) std::fprintf(stderr, "zwwm: %s\n", config_reloader->last_error().c_str());
  }
  std::unique_ptr<zwwm::ManagerProtocol> manager_protocol;
  try {
    manager_protocol = std::make_unique<zwwm::ManagerProtocol>(display, &compositor_server,
        [&loaded, &config_targets]() -> std::string {
          if (loaded.path.empty()) return "no configuration file is active";
          auto next = zwwm::load_runtime_config_file(loaded.path);
          if (!next.ok()) {
            auto error = zwwm::format_runtime_config_error(loaded.path, next.diagnostics);
            config_targets.show_error(error);
            return error;
          }
          if (!apply_environment(*next.config)) {
            const std::string error = "Could not apply the configuration environment";
            config_targets.show_error(error);
            return error;
          }
          const auto shader_error = config_targets.apply_config(next.config);
          if (!shader_error.empty()) return shader_error;
          config_targets.clear_error();
          loaded.config = std::move(next.config);
          return {};
        }, [&config_targets]() { return config_targets.rebuild_switch_shaders(); },
        [&config_targets](const std::string& theme, std::uint32_t size) {
          return config_targets.set_cursor(theme, size);
        });
  } catch (const std::exception& exception) {
    std::fprintf(stderr, "zwwm: manager protocol initialization failed: %s\n", exception.what());
    return EXIT_FAILURE;
  }
  nested_backend.set_input_target(&compositor_server);
  nested_backend.set_dmabuf_target(&compositor_server);
  const char* parent_wayland_display = std::getenv("WAYLAND_DISPLAY");
  const bool has_parent_wayland_display = parent_wayland_display != nullptr && *parent_wayland_display != '\0';
  if (has_parent_wayland_display && nested_backend.start()) {
    if (setenv("WAYLAND_DISPLAY", socket.c_str(), 1) != 0) {
      std::fprintf(stderr, "zwwm: could not export WAYLAND_DISPLAY: %d\n", errno);
      nested_backend.stop();
      manager_protocol.reset();
      return EXIT_FAILURE;
    }
    nested_backend.set_presentation_observer(
        [](void* data) { static_cast<zwwm::CompositorServer*>(data)->notify_frame_presented(); },
        &compositor_server);
    compositor_server.set_cursor_shape_observer(
        [](void* data, const char* name) { static_cast<zwwm::NestedBackend*>(data)->set_cursor_shape(name); },
        &nested_backend);
    compositor_server.set_pointer_position_observer(
        [](void* data, std::int32_t x, std::int32_t y) { static_cast<zwwm::NestedBackend*>(data)->set_cursor_position(x, y); },
        &nested_backend);
    compositor_server.set_surface_commit_observer(
        [](void* data, const zwwm::ShmBufferView& buffer) {
          static_cast<zwwm::NestedBackend*>(data)->present(buffer);
        },
        &nested_backend);
    std::unique_ptr<zwwm::PortalCapture> portal_capture;
    if (!portal_executable.empty()) portal_capture = std::make_unique<zwwm::PortalCapture>(display, event_loop,
          [&nested_backend](zwwm::OutputCapture& output, bool cursor, std::uint64_t target) {
            return target == 0 ? nested_backend.capture_output(output, cursor) :
                                 nested_backend.capture_toplevel(target, output, cursor);
          },
          [&compositor_server](zwayland::server::Resource* buffer, const zwwm::OutputCapture& output, std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h) { return compositor_server.copy_capture_buffer(buffer, output, x, y, w, h); },
          [&compositor_server](const zwwm::OutputCapture& output, std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h, std::uint32_t* px, std::uint32_t* py, std::uint32_t* pw, std::uint32_t* ph) { return compositor_server.map_capture_region(output, x, y, w, h, px, py, pw, ph); },
          [&compositor_server] { return compositor_server.toplevels(); },
           [&compositor_server](zwayland::server::Client* client, zwwm::OutputId output) { return compositor_server.output_resource(client, output); },
            [&compositor_server](zwayland::server::Resource* resource) { return compositor_server.output_info(compositor_server.output_id(resource)); }, false,
            [&compositor_server](zwayland::server::Client* client) { compositor_server.set_portal_client(client); });
    if (portal_capture != nullptr) {
      compositor_server.set_toplevel_observer([](void* data) { static_cast<zwwm::PortalCapture*>(data)->toplevels_changed(); }, portal_capture.get());
      if (!portal_capture->spawn(portal_executable)) {
        std::fprintf(stderr, "zwwm: %s\n", portal_capture->last_error().c_str());
        compositor_server.set_toplevel_observer(nullptr, nullptr);
        portal_capture.reset();
      }
    }
    presentation.portal = portal_capture.get();
    if (!loaded.error.empty()) nested_backend.show_error(loaded.error);
    std::printf("zwwm: using nested Wayland backend\n");
    std::printf("zwwm: listening on WAYLAND_DISPLAY=%s\n", socket.c_str());
    std::fflush(stdout);
    startup.armed = true;
    startup.schedule();
    display->run();
    compositor_server.set_presentation_observer(nullptr, nullptr);
    compositor_server.set_toplevel_observer(nullptr, nullptr);
    portal_capture.reset();
    nested_backend.stop();
    compositor_server.set_dmabuf_feedback({}, std::nullopt);
    nested_backend.set_presentation_observer(nullptr, nullptr);
    compositor_server.set_surface_commit_observer(nullptr, nullptr);
    compositor_server.set_cursor_shape_observer(nullptr, nullptr);
    compositor_server.set_pointer_position_observer(nullptr, nullptr);
  } else {
    zwwm::RuntimeBackend drm_backend(event_loop, loaded.config);
    config_targets.runtime = &drm_backend;
    drm_backend.set_input_target(&compositor_server);
    if (!drm_backend.start()) {
      std::fprintf(stderr, "zwwm: backend initialization failed: %s\n", drm_backend.last_error().c_str());
      manager_protocol.reset();
      return EXIT_FAILURE;
    }
    if (!loaded.error.empty()) drm_backend.show_error(loaded.error);
    drm_backend.set_dmabuf_target(&compositor_server);
    compositor_server.set_cursor_shape_observer(
        [](void* data, const char* name) { static_cast<zwwm::RuntimeBackend*>(data)->set_cursor_shape(name); },
        &drm_backend);
    compositor_server.set_pointer_position_observer(
        [](void* data, std::int32_t x, std::int32_t y) { static_cast<zwwm::RuntimeBackend*>(data)->set_cursor_position(x, y); },
        &drm_backend);
    if (setenv("WAYLAND_DISPLAY", socket.c_str(), 1) != 0) {
      std::fprintf(stderr, "zwwm: could not export WAYLAND_DISPLAY: %d\n", errno);
      drm_backend.stop();
         manager_protocol.reset();
      return EXIT_FAILURE;
    }
    compositor_server.set_surface_commit_observer(
        [](void* data, const zwwm::ShmBufferView& buffer) {
          static_cast<zwwm::RuntimeBackend*>(data)->present(buffer);
        },
        &drm_backend);
    std::unique_ptr<zwwm::PortalCapture> portal_capture;
    if (!portal_executable.empty()) portal_capture = std::make_unique<zwwm::PortalCapture>(display, event_loop,
          [&drm_backend](zwwm::OutputCapture& output, bool cursor, std::uint64_t target) {
            return target == 0 ? drm_backend.capture_output(output, cursor) :
                                 drm_backend.capture_toplevel(target, output, cursor);
          },
          [&compositor_server](zwayland::server::Resource* buffer, const zwwm::OutputCapture& output, std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h) { return compositor_server.copy_capture_buffer(buffer, output, x, y, w, h); },
          [&compositor_server](const zwwm::OutputCapture& output, std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h, std::uint32_t* px, std::uint32_t* py, std::uint32_t* pw, std::uint32_t* ph) { return compositor_server.map_capture_region(output, x, y, w, h, px, py, pw, ph); },
          [&compositor_server] { return compositor_server.toplevels(); },
           [&compositor_server](zwayland::server::Client* client, zwwm::OutputId output) { return compositor_server.output_resource(client, output); },
           [&compositor_server](zwayland::server::Resource* resource) { return compositor_server.output_info(compositor_server.output_id(resource)); },
            drm_backend.supports_embedded_cursor(),
            [&compositor_server](zwayland::server::Client* client) { compositor_server.set_portal_client(client); });
    if (portal_capture != nullptr) {
      compositor_server.set_toplevel_observer([](void* data) { static_cast<zwwm::PortalCapture*>(data)->toplevels_changed(); }, portal_capture.get());
      if (!portal_capture->spawn(portal_executable)) {
        std::fprintf(stderr, "zwwm: %s\n", portal_capture->last_error().c_str());
        compositor_server.set_toplevel_observer(nullptr, nullptr);
        portal_capture.reset();
      }
    }
    presentation.portal = portal_capture.get();
    std::printf("zwwm: using direct DRM backend\n");
    std::printf("zwwm: listening on WAYLAND_DISPLAY=%s\n", socket.c_str());
    std::fflush(stdout);
    startup.armed = true;
    startup.schedule();
    display->run();
    compositor_server.set_presentation_observer(nullptr, nullptr);
    compositor_server.set_toplevel_observer(nullptr, nullptr);
    portal_capture.reset();
    compositor_server.set_surface_commit_observer(nullptr, nullptr);
    compositor_server.set_cursor_shape_observer(nullptr, nullptr);
    compositor_server.set_pointer_position_observer(nullptr, nullptr);
    compositor_server.set_dmabuf_feedback({}, std::nullopt);
    drm_backend.stop();
    config_targets.runtime = nullptr;
  }
  manager_protocol.reset();
  return EXIT_SUCCESS;
}
