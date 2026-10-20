#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "connection.hpp"
#include <zwwm-client-connection-v1-zwayland-client.h>
#include <zwayland/wire/connection.hpp>
#include <zwayland/wire/message.hpp>

#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

extern char** environ;

namespace zterm {
namespace p = zwayland::generated;
using Fd = zwayland::wire::FileDescriptor;
using Parser = zwayland::wire::MessageParser;

namespace {

struct Peer {
  explicit Peer(int fd) : channel(fd) {}
  zwayland::wire::Connection channel;
  bool done = false;
  std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
};

std::vector<std::string> strings(std::span<const std::byte> bytes, size_t limit) {
  std::vector<std::string> result;
  while (!bytes.empty()) {
    const auto* data = reinterpret_cast<const char*>(bytes.data());
    const auto* end = static_cast<const char*>(std::memchr(data, 0, bytes.size()));
    if (!end || result.size() == limit) throw std::runtime_error("invalid daemon launch string array");
    result.emplace_back(data, end - data); bytes = bytes.subspan(end - data + 1);
  }
  return result;
}

struct Spawn {
  posix_spawn_file_actions_t files{};
  posix_spawnattr_t attributes{};
  Spawn() {
    if (posix_spawn_file_actions_init(&files) != 0) throw std::runtime_error("cannot initialize client launch");
    if (posix_spawnattr_init(&attributes) != 0) {
      posix_spawn_file_actions_destroy(&files); throw std::runtime_error("cannot initialize client attributes");
    }
  }
  ~Spawn() { posix_spawn_file_actions_destroy(&files); posix_spawnattr_destroy(&attributes); }
};

pid_t launch(ClientConnection& broker, Parser& parser) {
  // Open request: wire object 1/opcode 0, cwd string, NUL-separated argv array,
  // and an optional NUL-separated environment array. Reply: pid uint, error string.
  const std::string directory(parser.read_string());
  auto arguments = strings(parser.read_array(), 256);
  auto environment = parser.at_end() ? std::vector<std::string>{} : strings(parser.read_array(), 1024);
  if (!parser.at_end() || directory.empty() || directory.front() != '/' || directory.find('\0') != std::string::npos)
    throw std::runtime_error("daemon launch requires an absolute working directory");
  if (!arguments.empty() && arguments.front() == "daemon") throw std::runtime_error("nested daemon launch is not supported");
  auto socket = broker.create_wayland();
  Spawn spawn;
  int error = posix_spawn_file_actions_adddup2(&spawn.files, socket.get(), 3);
  if (!error) error = posix_spawn_file_actions_addchdir_np(&spawn.files, directory.c_str());
  sigset_t empty, defaults; sigemptyset(&empty); sigemptyset(&defaults);
  for (int signal : {SIGINT,SIGTERM,SIGHUP,SIGCHLD,SIGPIPE}) sigaddset(&defaults, signal);
  if (!error) error = posix_spawnattr_setsigmask(&spawn.attributes, &empty);
  if (!error) error = posix_spawnattr_setsigdefault(&spawn.attributes, &defaults);
  if (!error) error = posix_spawnattr_setflags(&spawn.attributes, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSID);
  if (error) throw std::runtime_error(std::strerror(error));
  std::unordered_map<std::string, std::string> values;
  const auto insert = [&](const std::string& entry) {
    const auto separator = entry.find('=');
    if (separator == std::string::npos || separator == 0) throw std::runtime_error("invalid launch environment");
    values[entry.substr(0, separator)] = entry.substr(separator + 1);
  };
  if (environment.empty()) for (char** entry = environ; *entry; ++entry) insert(*entry);
  else for (const auto& entry : environment) insert(entry);
  values["WAYLAND_SOCKET"] = "3";
  for (const char* name : {"WAYLAND_DISPLAY", "XDG_RUNTIME_DIR"})
    if (const auto* value = std::getenv(name)) values[name] = value;
  environment.clear();
  for (const auto& [name, value] : values) environment.push_back(name + "=" + value);
  std::vector<char*> argv{const_cast<char*>("zterm")}, env;
  for (auto& argument : arguments) argv.push_back(argument.data());
  argv.push_back(nullptr);
  for (auto& entry : environment) env.push_back(entry.data());
  env.push_back(nullptr);
  pid_t child = -1;
  error = posix_spawn(&child, "/proc/self/exe", &spawn.files, &spawn.attributes, argv.data(), env.data());
  if (error) throw std::runtime_error(std::strerror(error));
  return child;
}

void request(Peer& peer, ClientConnection& broker, const zwayland::wire::Message& message) {
  pid_t child = 0; std::string error;
  try {
    if (message.object_id != 1 || message.opcode != 0 || !message.fds.empty()) throw std::runtime_error("invalid zterm daemon request");
    Parser parser(message.body, message.fds); child = launch(broker, parser);
  } catch (const std::exception& exception) { error = exception.what(); }
  for (const auto fd : message.fds) close(fd);
  peer.channel.consume(message.size, message.fds.size());
  zwayland::wire::MessageBuilder reply(1); reply.set_opcode(0);
  reply.append_uint(child); reply.append_string(error);
  peer.channel.queue(reply.finish(), reply.take_fds()); peer.done = true;
}

}  // namespace

int run_daemon() {
  (void)signal(SIGPIPE, SIG_IGN);
  sigset_t mask, previous; sigemptyset(&mask);
  for (int signal : {SIGINT,SIGTERM,SIGHUP,SIGCHLD}) sigaddset(&mask, signal);
  if (sigprocmask(SIG_BLOCK, &mask, &previous) != 0) throw std::runtime_error("cannot initialize daemon signals");
  struct Restore { sigset_t mask; ~Restore() { (void)sigprocmask(SIG_SETMASK, &mask, nullptr); } } restore{previous};
  Fd signals(signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK));
  if (signals.get() < 0) throw std::runtime_error("cannot initialize daemon signal descriptor");
  ClientConnection broker(true);
  std::list<Peer> peers;
  bool ready = false;
  std::string error;
  const auto registration = p::zwwm_client_connection_manager_v1_register_daemon(broker.display(), broker.manager()->id, "zterm");
  broker.display().find_proxy(registration)->set_observer([&](zwayland::client::Proxy&, std::uint32_t opcode, Parser& parser) {
    if (opcode == 0) ready = true;
    else if (opcode == 1) { (void)parser.read_uint(); error = parser.read_string(); }
    else {
      Fd socket(parser.read_fd());
      if (peers.size() < 64) peers.emplace_back(socket.release());
    }
  });
  if (!broker.display().roundtrip() || !ready) throw std::runtime_error(error.empty() ? "daemon registration failed" : error);
  std::fputs("zterm: daemon registered with zwwm as 'zterm'\n", stderr);
  bool running = true;
  while (running) {
    if (!broker.display().flush()) break;
    for (auto& peer : peers) (void)peer.channel.flush();
    std::erase_if(peers, [](const auto& peer) {
      return !peer.channel.healthy() || (peer.done && !peer.channel.has_output()) || std::chrono::steady_clock::now() >= peer.deadline;
    });
    std::vector<pollfd> descriptors{
      {broker.display().fd(), static_cast<short>(POLLIN | (broker.display().wants_write() ? POLLOUT : 0)), 0},
      {signals.get(), POLLIN, 0}};
    for (const auto& peer : peers) descriptors.push_back({peer.channel.fd(), static_cast<short>(POLLIN | (peer.channel.has_output() ? POLLOUT : 0)), 0});
    const int result = poll(descriptors.data(), descriptors.size(), peers.empty() ? -1 : 1000);
    if (result < 0) { if (errno == EINTR) continue; throw std::runtime_error("daemon poll failed"); }
    if (descriptors[0].revents & (POLLHUP | POLLERR | POLLNVAL)) break;
    if (descriptors[0].revents & POLLIN) if (!broker.display().dispatch_pending()) break;
    if (descriptors[1].revents & POLLIN) {
      signalfd_siginfo event{};
      while (read(signals.get(), &event, sizeof(event)) == sizeof(event)) {
        if (event.ssi_signo == SIGCHLD) { while (waitpid(-1, nullptr, WNOHANG) > 0) {} }
        else running = false;
      }
    }
    for (auto& peer : peers) {
      const auto found = std::find_if(descriptors.begin() + 2, descriptors.end(), [&](const auto& descriptor) { return descriptor.fd == peer.channel.fd(); });
      if (found == descriptors.end() || !found->revents) continue;
      if (found->revents & (POLLIN | POLLHUP | POLLERR)) {
        (void)peer.channel.read_available();
        if (!peer.done) if (const auto message = peer.channel.next_message()) request(peer, broker, *message);
        if (peer.channel.peer_closed()) peer.done = true;
      }
      if (found->revents & POLLOUT) (void)peer.channel.flush();
    }
  }
  p::zwwm_client_daemon_v1_destroy(broker.display(), registration);
  (void)broker.display().flush();
  return broker.display().has_error() ? 1 : 0;
}

}  // namespace zterm
