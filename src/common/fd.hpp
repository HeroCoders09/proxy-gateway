#pragma once
// RAII owner of a file descriptor. Every socket / fd in the project lives in one of these;
// there are no raw close() calls elsewhere.
#include <unistd.h>

#include <utility>

namespace pg {

class Fd {
 public:
  Fd() = default;
  explicit Fd(int fd) : fd_(fd) {}
  ~Fd() { reset(); }

  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  Fd(Fd&& other) noexcept : fd_(other.release()) {}
  Fd& operator=(Fd&& other) noexcept {
    if (this != &other) reset(other.release());
    return *this;
  }

  int get() const { return fd_; }
  bool valid() const { return fd_ >= 0; }
  explicit operator bool() const { return valid(); }

  // Give up ownership without closing.
  int release() {
    int f = fd_;
    fd_ = -1;
    return f;
  }

  // Close the current fd (if any) and take ownership of `fd`.
  void reset(int fd = -1) {
    if (fd_ >= 0) ::close(fd_);  // On Linux the fd is released even if close() reports EINTR.
    fd_ = fd;
  }

 private:
  int fd_ = -1;
};

}  // namespace pg
