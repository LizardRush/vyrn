// files.h on POSIX: open(2), fstat, fsync, rename, link, mkdir.
#include "platform/files.h"

#include "platform/posix_util.h"
#include "vyrn/platform.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>
#include <utility>

namespace vyrn::platform {

namespace {

void fill(const struct stat& st, FileInfo& out) {
  out = FileInfo{};
  out.type = S_ISREG(st.st_mode)   ? FileInfo::Type::Regular
             : S_ISDIR(st.st_mode) ? FileInfo::Type::Directory
             : S_ISLNK(st.st_mode) ? FileInfo::Type::Symlink
                                   : FileInfo::Type::Other;
  out.size = st.st_size > 0 ? static_cast<uint64_t>(st.st_size) : 0;
  posix::stat_mtime(st, out.mtime_s, out.mtime_ns);
  out.device = static_cast<uint64_t>(st.st_dev);
  out.inode = static_cast<uint64_t>(st.st_ino);
}

int fd_of(intptr_t handle) { return static_cast<int>(handle); }

}  // namespace

std::string error_text(int err) { return std::strerror(err); }

int file_info(const std::string& path, bool follow, FileInfo& out) {
  struct stat st {};
  if ((follow ? ::stat(path.c_str(), &st) : ::lstat(path.c_str(), &st)) != 0) return errno;
  fill(st, out);
  return 0;
}

// ---- File ------------------------------------------------------------------------------------------------------

File::~File() { close(); }

File::File(File&& other) noexcept : handle_(std::exchange(other.handle_, -1)) {}

File& File::operator=(File&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = std::exchange(other.handle_, -1);
  }
  return *this;
}

File File::open_read(const std::string& path, bool no_follow, int& err) {
  // O_NONBLOCK: a FIFO in a file's place can't hang the open or the reads (regular files ignore it).
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | (no_follow ? O_NOFOLLOW : 0));
  err = fd < 0 ? errno : 0;
  return File(fd);
}

File File::create(const std::string& path, Create how, bool no_follow, int mode, int& err) {
  const int flags = O_WRONLY | O_CREAT | O_CLOEXEC | (how == Create::Exclusive ? O_EXCL : O_TRUNC) |
                    (no_follow ? O_NOFOLLOW : 0);
  const int fd = ::open(path.c_str(), flags, static_cast<mode_t>(mode));
  err = fd < 0 ? errno : 0;
  return File(fd);
}

File File::create_unique(const std::string& prefix, std::string& path, int& err) {
  std::string name = prefix + "XXXXXX";
  const int fd = ::mkstemp(name.data());   // mode 0600
  err = fd < 0 ? errno : 0;
  if (fd >= 0) {
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    path = name;
  }
  return File(fd);
}

int File::info(FileInfo& out) const {
  struct stat st {};
  if (::fstat(fd_of(handle_), &st) != 0) return errno;
  fill(st, out);
  return 0;
}

int File::read(void* buffer, size_t size, size_t& got) {
  got = 0;
  for (;;) {
    const ssize_t n = ::read(fd_of(handle_), buffer, size);
    if (n >= 0) {
      got = static_cast<size_t>(n);
      return 0;
    }
    if (errno != EINTR) return errno;
  }
}

int File::write_all(const void* data, size_t size) {
  const char* bytes = static_cast<const char*>(data);
  while (size > 0) {
    const ssize_t n = ::write(fd_of(handle_), bytes, size);
    if (n < 0) {
      if (errno == EINTR) continue;
      return errno;
    }
    bytes += n;
    size -= static_cast<size_t>(n);
  }
  return 0;
}

int File::sync() { return ::fsync(fd_of(handle_)) == 0 ? 0 : errno; }

int File::set_mode(int mode) { return ::fchmod(fd_of(handle_), static_cast<mode_t>(mode)) == 0 ? 0 : errno; }

int File::close() {
  if (handle_ == -1) return 0;
  const int rc = ::close(fd_of(std::exchange(handle_, -1)));
  return rc == 0 ? 0 : errno;
}

// ---- paths -----------------------------------------------------------------------------------------------------

int make_dir(const std::string& path, int mode) {
  return ::mkdir(path.c_str(), static_cast<mode_t>(mode)) == 0 ? 0 : errno;
}

int remove_file(const std::string& path) { return ::unlink(path.c_str()) == 0 ? 0 : errno; }

int remove_dir(const std::string& path) { return ::rmdir(path.c_str()) == 0 ? 0 : errno; }

int set_mode(const std::string& path, int mode) {
  return ::chmod(path.c_str(), static_cast<mode_t>(mode)) == 0 ? 0 : errno;
}

int check_readable(const std::string& path) { return ::access(path.c_str(), R_OK) == 0 ? 0 : errno; }

int rename_replace(const std::string& from, const std::string& to) {
  return ::rename(from.c_str(), to.c_str()) == 0 ? 0 : errno;
}

int rename_no_replace(const std::string& from, const std::string& to, bool directory) {
  int err = posix::rename_exclusive(from.c_str(), to.c_str());
  if (err != EINVAL && err != ENOSYS && err != ENOTSUP) return err;   // done, or a real answer (EEXIST, ...)
  if (!directory) {   // this file system can't refuse to replace: a hard link can
    if (::link(from.c_str(), to.c_str()) != 0) return errno;
    ::unlink(from.c_str());
    return 0;
  }
  // Claim the name with mkdir (fails if it exists), then move onto that empty folder (fails with ENOTEMPTY if
  // anything was put into it meanwhile).
  if (::mkdir(to.c_str(), 0755) != 0) return errno;
  if (::rename(from.c_str(), to.c_str()) == 0) return 0;
  err = errno;
  ::rmdir(to.c_str());   // only if it is still the empty folder made above
  return err;
}

int sync_path(const std::string& path, bool directory) {
  const int fd = ::open(path.c_str(), (directory ? O_RDONLY | O_DIRECTORY : O_RDONLY) | O_CLOEXEC);
  if (fd < 0) return errno;
  const int rc = ::fsync(fd) == 0 ? 0 : errno;
  ::close(fd);
  return rc;
}

// ---- FolderLock ------------------------------------------------------------------------------------------------

FolderLock::FolderLock(const std::string& folder, int wait_ms) {
  const int fd = ::open(folder.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) return;   // can't serialise here: the caller falls back to its own checks
  handle_ = fd;
  for (int attempt = 0; attempt < wait_ms; ++attempt) {
    if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
      held_ = true;
      return;
    }
    if (errno != EWOULDBLOCK && errno != EINTR) return;   // no flock on this file system
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  busy_ = true;
}

FolderLock::~FolderLock() {
  if (handle_ != -1) ::close(static_cast<int>(handle_));   // closing it releases the flock
}

// ---- vyrn/platform.h -------------------------------------------------------------------------------------------

int write_file_atomic(const std::string& path, const std::string& temp, const std::string& content, int mode) {
  ::unlink(temp.c_str());   // a leftover of an earlier attempt
  int err = 0;
  File file = File::create(temp, File::Create::Exclusive, /*no_follow=*/true, mode, err);
  if (!file) return err;
  err = file.write_all(content.data(), content.size());
  if (err == 0) err = file.sync();
  if (const int closed = file.close(); err == 0) err = closed;
  if (err == 0) err = rename_replace(temp, path);
  if (err != 0) ::unlink(temp.c_str());
  return err;
}

bool is_writable(const std::string& path) { return ::access(path.c_str(), W_OK) == 0; }

}  // namespace vyrn::platform
