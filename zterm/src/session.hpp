#pragma once

#include "config.hpp"
#include <poll.h>
#include <functional>
#include <memory>
#include <span>
#include <sys/types.h>

namespace zterm {

class Session {
 public:
  virtual ~Session() = default;
  virtual void open(Options options, std::function<Options()> reload,
                    std::string directory, std::vector<std::string> environment) = 0;
  virtual void prepare_frame() = 0;
  virtual void descriptors(std::vector<pollfd>& output) const = 0;
  virtual void dispatch(std::span<const pollfd> events) = 0;
  virtual int timeout() const = 0;
  virtual bool running() const = 0;
  virtual bool healthy() const = 0;
  virtual bool presented() const = 0;
  virtual pid_t child() const = 0;
  virtual void reap(pid_t child) = 0;
  virtual void retire() = 0;
};

std::unique_ptr<Session> prepare_session(Options options);

}  // namespace zterm
