// Files, the portable way (docs/dev/platforms.md section 3): open/read/write/fsync, exclusive create, atomic
// replace, no-replace moves, file identity. POSIX: open(2) and friends; Windows: the wide (...W) API.
// Every call that fails reports an errno value (Windows errors are mapped: ENOENT, EACCES, EEXIST, ELOOP, ...),
// so callers keep their messages (error_text()). Paths are UTF-8.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace vyrn::platform {

std::string error_text(int err);   // strerror(err)

struct FileInfo {
  enum class Type { Regular, Directory, Symlink, Other };
  Type type = Type::Other;
  uint64_t size = 0;
  int64_t mtime_s = 0;    // last modification: seconds since the epoch ...
  int32_t mtime_ns = 0;   // ... and nanoseconds
  uint64_t device = 0;    // device + inode name the file (Windows: volume serial number + file index)
  uint64_t inode = 0;
  bool is_regular() const { return type == Type::Regular; }
  bool is_directory() const { return type == Type::Directory; }
  bool is_symlink() const { return type == Type::Symlink; }
};

// stat (follow = true) or lstat (follow = false: a symlink describes itself). 0 or the errno.
int file_info(const std::string& path, bool follow, FileInfo& out);

class File {
 public:
  File() = default;
  ~File();
  File(File&& other) noexcept;
  File& operator=(File&& other) noexcept;
  File(const File&) = delete;
  File& operator=(const File&) = delete;

  // For reading. no_follow: refuse a symlink (ELOOP). A FIFO never blocks the open or the reads (O_NONBLOCK).
  // `err` gets the errno when it fails.
  static File open_read(const std::string& path, bool no_follow, int& err);
  enum class Create {
    Truncate,    // O_CREAT | O_TRUNC
    Exclusive,   // O_CREAT | O_EXCL: EEXIST when anything (even a dangling symlink) has the name
  };
  // For writing. no_follow: never write through a symlink. `mode` is the POSIX permission (ignored on Windows).
  static File create(const std::string& path, Create how, bool no_follow, int mode, int& err);
  // Creates "<prefix>XXXXXX" (6 random characters) exclusively, mode 0600 (like mkstemp); path = its name.
  static File create_unique(const std::string& prefix, std::string& path, int& err);

  bool is_open() const { return handle_ != -1; }
  explicit operator bool() const { return is_open(); }
  // The calls below return 0 or the errno (EINTR is retried inside).
  int info(FileInfo& out) const;                                  // fstat
  int read(void* buffer, size_t size, size_t& got);               // got = 0 at the end
  int write_all(const void* data, size_t size);
  int sync();                                                     // fsync / FlushFileBuffers
  int set_mode(int mode);                                         // fchmod (Windows: nothing)
  int close();                                                    // closing errors count (NFS, quotas)

 private:
  explicit File(intptr_t handle) : handle_(handle) {}
  intptr_t handle_ = -1;   // a file descriptor (POSIX) or a HANDLE (Windows)
};

// Each returns 0 or the errno.
int make_dir(const std::string& path, int mode);   // EEXIST when the name is taken
int remove_file(const std::string& path);          // unlink: a symlink is removed itself
int remove_dir(const std::string& path);           // an empty folder only
int set_mode(const std::string& path, int mode);   // chmod (Windows: nothing)
int check_readable(const std::string& path);       // access(R_OK)
// Atomic replace: readers see the old or the new file (POSIX rename; Windows MoveFileExW with
// MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH).
int rename_replace(const std::string& from, const std::string& to);
// Moves a file or a folder to a name that must be free: EEXIST (or ENOTEMPTY) when it is taken, and then nothing
// changed. Linux renameat2(RENAME_NOREPLACE), macOS renamex_np(RENAME_EXCL); where the file system can't do that,
// a file is hard-linked and a folder claimed with mkdir. Windows: MoveFileExW without replacing.
int rename_no_replace(const std::string& from, const std::string& to, bool directory);
// fsync a file or a folder (POSIX). Folders can't be synced on Windows: 0 there.
int sync_path(const std::string& path, bool directory);

// An exclusive lock between processes, named by a folder, held while this object lives (POSIX: flock() on the
// folder; Windows: a named mutex). Waits at most about wait_ms. When the system can't lock that folder, it is
// neither held nor busy (the caller carries on without it).
class FolderLock {
 public:
  FolderLock(const std::string& folder, int wait_ms);
  ~FolderLock();
  FolderLock(const FolderLock&) = delete;
  FolderLock& operator=(const FolderLock&) = delete;
  bool busy() const { return busy_; }   // another holder kept it for longer than wait_ms

 private:
  intptr_t handle_ = -1;
  bool held_ = false;
  bool busy_ = false;
};

// "Now", from the clock that stamps file modification times (Linux: CLOCK_REALTIME_COARSE), so that a file
// written after this call never looks older than it.
void file_clock_now(int64_t& seconds, int32_t& nanoseconds);

}  // namespace vyrn::platform
