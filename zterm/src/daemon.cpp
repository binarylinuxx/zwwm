#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "connection.hpp"
#include "session.hpp"
#include <zwwm-client-connection-v1-zwayland-client.h>
#include <zwayland/wire/connection.hpp>
#include <zwayland/wire/message.hpp>

#include <poll.h>
#include <signal.h>
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
#include <filesystem>

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

Options options(std::vector<std::string>& arguments) {
  std::vector<char*> argv;
  for (auto& argument : arguments) argv.push_back(argument.data());
  return load_options(argv.size(), argv.data());
}

Options defaults() { std::vector<std::string> arguments{"zterm"}; return options(arguments); }

struct Runtime {
  struct Window { std::uint32_t id; std::unique_ptr<Session> session; };
  std::list<Window> windows;
  std::unique_ptr<Session> spare;
  std::uint32_t next_id = 1;

  void warm() { spare = prepare_session(defaults()); }
  void recycle() {
    for (auto it = windows.begin(); it != windows.end();) {
      if (it->session->running()) { ++it; continue; }
      it->session->retire();
      if (it->session->healthy()) spare = std::move(it->session);
      it = windows.erase(it);
    }
  }
};

std::uint32_t launch(Runtime& runtime, Parser& parser) {
  // Open request: wire object 1/opcode 0, cwd string, NUL-separated argv array,
  // and an optional NUL-separated environment array. Reply: window id, error string.
  const std::string directory(parser.read_string());
  auto arguments = strings(parser.read_array(), 256);
  auto environment = parser.at_end() ? std::vector<std::string>{} : strings(parser.read_array(), 1024);
  if (!parser.at_end() || directory.empty() || directory.front() != '/' || directory.find('\0') != std::string::npos)
    throw std::runtime_error("daemon launch requires an absolute working directory");
  if (!arguments.empty() && arguments.front() == "daemon") throw std::runtime_error("nested daemon launch is not supported");
  for (size_t i = 0; i < arguments.size(); ++i) {
    if (arguments[i] == "-e" || arguments[i] == "--") break;
    if (arguments[i] == "--config" && i + 1 < arguments.size()) {
      auto& path = arguments[++i];
      if (std::filesystem::path(path).is_relative()) path = (std::filesystem::path(directory) / path).string();
    } else if (arguments[i].starts_with("--config=")) {
      const auto path = std::filesystem::path(arguments[i].substr(9));
      if (path.is_relative()) arguments[i] = "--config=" + (std::filesystem::path(directory) / path).string();
    }
  }
  arguments.insert(arguments.begin(), "zterm");
  auto argv = std::make_shared<std::vector<std::string>>(std::move(arguments));
  auto settings = options(*argv);
  if (settings.action != Options::Action::Run) throw std::runtime_error("daemon activation requires terminal window options");
  auto window = runtime.spare && runtime.spare->healthy() ? std::move(runtime.spare) : prepare_session(settings);
  window->open(std::move(settings), [argv] { return options(*argv); }, directory, std::move(environment));
  window->prepare_frame();
  const auto id = runtime.next_id++;
  runtime.windows.push_back({id,std::move(window)});
  return id;
}

void request(Peer& peer, Runtime& runtime, const zwayland::wire::Message& message) {
  std::uint32_t window = 0; std::string error;
  try {
    if (message.object_id != 1 || message.opcode != 0 || !message.fds.empty()) throw std::runtime_error("invalid zterm daemon request");
    Parser parser(message.body, message.fds); window = launch(runtime, parser);
  } catch (const std::exception& exception) { error = exception.what(); }
  for (const auto fd : message.fds) close(fd);
  peer.channel.consume(message.size, message.fds.size());
  zwayland::wire::MessageBuilder reply(1); reply.set_opcode(0);
  reply.append_uint(window); reply.append_string(error);
  peer.channel.queue(reply.finish(), reply.take_fds()); peer.done = true; (void)peer.channel.flush();
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
  Runtime runtime;
  runtime.warm();
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
  std::fputs("zterm: terminal engine, GPU and fonts ready; daemon registered as 'zterm'\n", stderr);
  bool running = true;
  bool prewarm_failed = false;
  while (running) {
    for (auto& window : runtime.windows) {
      try { window.session->prepare_frame(); }
      catch (const std::exception& error) { std::fprintf(stderr,"zterm: window failed: %s\n",error.what()); window.session->retire(); }
    }
    if (runtime.spare) {
      try { runtime.spare->prepare_frame(); }
      catch (const std::exception& error) { std::fprintf(stderr,"zterm: idle renderer failed: %s\n",error.what()); runtime.spare.reset(); }
      if (runtime.spare && !runtime.spare->healthy()) runtime.spare.reset();
    }
    runtime.recycle();
    if (runtime.spare) prewarm_failed = false;
    if (!runtime.spare && !prewarm_failed && std::any_of(runtime.windows.begin(),runtime.windows.end(),[](const auto& window) { return window.session->presented(); })) {
      try { runtime.warm(); }
      catch (const std::exception& error) { std::fprintf(stderr,"zterm: renderer prewarm failed: %s\n",error.what()); prewarm_failed = true; }
    }
    if (!broker.display().flush()) break;
    for (auto& peer : peers) (void)peer.channel.flush();
    std::erase_if(peers, [](const auto& peer) {
      return !peer.channel.healthy() || (peer.done && !peer.channel.has_output()) || std::chrono::steady_clock::now() >= peer.deadline;
    });
    std::vector<pollfd> descriptors{
      {broker.display().fd(), static_cast<short>(POLLIN | (broker.display().wants_write() ? POLLOUT : 0)), 0},
      {signals.get(), POLLIN, 0}};
    for (const auto& peer : peers) descriptors.push_back({peer.channel.fd(), static_cast<short>(POLLIN | (peer.channel.has_output() ? POLLOUT : 0)), 0});
    const auto peer_end = descriptors.size();
    struct Range { Session* session; size_t start, count; };
    std::vector<Range> ranges;
    int timeout = peers.empty() ? -1 : 1000;
    const auto append = [&](Session& session) {
      const auto start = descriptors.size(); session.descriptors(descriptors);
      ranges.push_back({&session,start,descriptors.size()-start});
      const int next = session.timeout();
      if (next >= 0) timeout = timeout < 0 ? next : std::min(timeout,next);
    };
    for (auto& window : runtime.windows) append(*window.session);
    if (runtime.spare) append(*runtime.spare);
    const int result = poll(descriptors.data(), descriptors.size(), timeout);
    if (result < 0) { if (errno == EINTR) continue; throw std::runtime_error("daemon poll failed"); }
    if (descriptors[0].revents & (POLLHUP | POLLERR | POLLNVAL)) break;
    if (descriptors[0].revents & POLLIN) if (!broker.display().dispatch_pending()) break;
    if (descriptors[1].revents & POLLIN) {
      signalfd_siginfo event{};
      while (read(signals.get(), &event, sizeof(event)) == sizeof(event)) {
        if (event.ssi_signo == SIGCHLD) {
          for (pid_t child; (child = waitpid(-1,nullptr,WNOHANG)) > 0;) {
            for (auto& window : runtime.windows) window.session->reap(child);
            if (runtime.spare) runtime.spare->reap(child);
          }
        }
        else running = false;
      }
    }
    for (const auto& range : ranges) {
      try { range.session->dispatch(std::span<const pollfd>(descriptors).subspan(range.start,range.count)); }
      catch (const std::exception& error) { std::fprintf(stderr,"zterm: session input failed: %s\n",error.what()); range.session->retire(); }
    }
    for (auto& peer : peers) {
      const auto end = descriptors.begin() + peer_end;
      const auto found = std::find_if(descriptors.begin() + 2, end, [&](const auto& descriptor) { return descriptor.fd == peer.channel.fd(); });
      if (found == end || !found->revents) continue;
      if (found->revents & (POLLIN | POLLHUP | POLLERR)) {
        (void)peer.channel.read_available();
        if (!peer.done) if (const auto message = peer.channel.next_message()) request(peer, runtime, *message);
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
