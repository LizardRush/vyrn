// files.h on Windows: the wide CreateFileW family. Files are opened with FILE_SHARE_DELETE so that an atomic replace
// (MoveFileExW) works while someone reads them, as on POSIX. Errors come back as errno values (win_util.h).
#include "platform/files.h"

#include "platform/system.h"
#include "platform/win_util.h"
#include "vyrn/platform.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <utility>
#include <vector>

namespace vyrn::platform {

namespace {

using win::widen;

constexpr DWORD kShareAll = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

HANDLE handle_of(intptr_t h) { return reinterpret_cast<HANDLE>(h); }

int last_errno() { return win::errno_from(GetLastError()); }

// FILETIME (100 ns since 1601) -> seconds and nanoseconds since 1970.
int64_t unix_time(const FILETIME& ft, int32_t& nanoseconds) {
  ULARGE_INTEGER t;
  t.LowPart = ft.dwLowDateTime;
  t.HighPart = ft.dwHighDateTime;
  const int64_t ticks = static_cast<int64_t>(t.QuadPart) - 116444736000000000LL;
  int64_t seconds = ticks / 10000000;
  int64_t rest = ticks % 10000000;
  if (rest < 0) {
    rest += 10000000;
    --seconds;
  }
  nanoseconds = static_cast<int32_t>(rest * 100);
  return seconds;
}

// A symbolic link or a junction (other reparse points, such as cloud placeholders, are ordinary files here).
bool is_link(HANDLE h) {
  FILE_ATTRIBUTE_TAG_INFO tag{};
  if (!GetFileInformationByHandleEx(h, FileAttributeTagInfo, &tag, sizeof tag)) return false;
  return (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
         (tag.ReparseTag == IO_REPARSE_TAG_SYMLINK || tag.ReparseTag == IO_REPARSE_TAG_MOUNT_POINT);
}

int describe(HANDLE h, bool links_too, FileInfo& out) {
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(h, &info)) return last_errno();
  out = FileInfo{};
  const DWORD kind = GetFileType(h);
  if (links_too && is_link(h)) {
    out.type = FileInfo::Type::Symlink;
  } else if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
    out.type = FileInfo::Type::Directory;
  } else if (kind == FILE_TYPE_DISK) {
    out.type = FileInfo::Type::Regular;
  } else {
    out.type = FileInfo::Type::Other;   // a device or a pipe
  }
  out.size = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
  out.mtime_s = unix_time(info.ftLastWriteTime, out.mtime_ns);
  out.device = info.dwVolumeSerialNumber;
  out.inode = (static_cast<uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
  return 0;
}

// Opens `path`; no_follow refuses a symbolic link or junction with ELOOP (like O_NOFOLLOW).
HANDLE open_path(const std::string& path, DWORD access, DWORD disposition, DWORD flags, bool no_follow, int& err) {
  const std::wstring w = widen(path);
  HANDLE h = CreateFileW(w.c_str(), access, kShareAll, nullptr, disposition,
                         flags | (no_follow ? FILE_FLAG_OPEN_REPARSE_POINT : 0), nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    err = last_errno();
    return h;
  }
  if (no_follow) {
    if (is_link(h)) {
      CloseHandle(h);
      err = ELOOP;
      return INVALID_HANDLE_VALUE;
    }
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (GetFileInformationByHandleEx(h, FileAttributeTagInfo, &tag, sizeof tag) &&
        (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {   // not a link (e.g. a cloud file): open its content
      CloseHandle(h);
      h = CreateFileW(w.c_str(), access, kShareAll, nullptr, disposition, flags, nullptr);
      if (h == INVALID_HANDLE_VALUE) {
        err = last_errno();
        return h;
      }
    }
  }
  err = 0;
  return h;
}

}  // namespace

std::string error_text(int err) {
  switch (err) {   // the MSVC runtime has no words for the POSIX additions
    case ELOOP: return "Too many levels of symbolic links";
    case ENOTSUP: return "Operation not supported";
    case EWOULDBLOCK: return "Operation would block";
    case EADDRINUSE: return "Address already in use";
    case EADDRNOTAVAIL: return "Address not available";
    case ECONNRESET: return "Connection reset by peer";
    case ECONNABORTED: return "Connection aborted";
    case ECONNREFUSED: return "Connection refused";
    case ETIMEDOUT: return "Connection timed out";
    case ENOTCONN: return "Not connected";
    case ENOBUFS: return "No buffer space available";
    default: return std::strerror(err);
  }
}

int file_info(const std::string& path, bool follow, FileInfo& out) {
  win::Handle h(CreateFileW(widen(path).c_str(), FILE_READ_ATTRIBUTES, kShareAll, nullptr, OPEN_EXISTING,
                            FILE_FLAG_BACKUP_SEMANTICS | (follow ? 0 : FILE_FLAG_OPEN_REPARSE_POINT), nullptr));
  if (!h) return last_errno();
  return describe(h.get(), !follow, out);
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
  // FILE_FLAG_BACKUP_SEMANTICS: a folder opens too (info() then says what it is, as fstat would).
  HANDLE h = open_path(path, GENERIC_READ, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS,
                       no_follow, err);
  return File(h == INVALID_HANDLE_VALUE ? -1 : reinterpret_cast<intptr_t>(h));
}

File File::create(const std::string& path, Create how, bool no_follow, int /*mode*/, int& err) {
  if (how == Create::Truncate && no_follow) {
    FileInfo info;
    if (file_info(path, false, info) == 0 && info.is_symlink()) {   // never write through a link
      err = ELOOP;
      return File();
    }
  }
  // CREATE_NEW fails for anything that has the name, a dangling link included (like O_EXCL).
  const DWORD disposition = how == Create::Exclusive ? CREATE_NEW : CREATE_ALWAYS;
  HANDLE h = CreateFileW(widen(path).c_str(), GENERIC_WRITE, kShareAll, nullptr, disposition, FILE_ATTRIBUTE_NORMAL,
                         nullptr);
  err = h == INVALID_HANDLE_VALUE ? last_errno() : 0;
  return File(h == INVALID_HANDLE_VALUE ? -1 : reinterpret_cast<intptr_t>(h));
}

File File::create_unique(const std::string& prefix, std::string& path, int& err) {
  static const char kChars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  for (int attempt = 0; attempt < 100; ++attempt) {
    unsigned char bytes[6];
    if (!random_bytes(bytes, sizeof bytes)) {
      err = EIO;
      return File();
    }
    std::string name = prefix;
    for (const unsigned char b : bytes) name += kChars[b % 62];
    File file = create(name, Create::Exclusive, true, 0600, err);
    if (file) {
      path = name;
      return file;
    }
    if (err != EEXIST) return File();
  }
  err = EEXIST;
  return File();
}

int File::info(FileInfo& out) const { return describe(handle_of(handle_), false, out); }

int File::read(void* buffer, size_t size, size_t& got) {
  got = 0;
  DWORD n = 0;
  const DWORD want = static_cast<DWORD>(size < (1u << 30) ? size : (1u << 30));
  if (!ReadFile(handle_of(handle_), buffer, want, &n, nullptr)) {
    const DWORD error = GetLastError();
    if (error == ERROR_BROKEN_PIPE || error == ERROR_HANDLE_EOF) return 0;   // the end
    return win::errno_from(error);
  }
  got = n;
  return 0;
}

int File::write_all(const void* data, size_t size) {
  const char* bytes = static_cast<const char*>(data);
  while (size > 0) {
    DWORD n = 0;
    const DWORD chunk = static_cast<DWORD>(size < (1u << 30) ? size : (1u << 30));
    if (!WriteFile(handle_of(handle_), bytes, chunk, &n, nullptr)) return last_errno();
    if (n == 0) return EIO;
    bytes += n;
    size -= n;
  }
  return 0;
}

int File::sync() { return FlushFileBuffers(handle_of(handle_)) ? 0 : last_errno(); }

int File::set_mode(int /*mode*/) { return 0; }   // permissions come from the folder's ACL

int File::close() {
  if (handle_ == -1) return 0;
  return CloseHandle(handle_of(std::exchange(handle_, -1))) ? 0 : last_errno();
}

// ---- paths -----------------------------------------------------------------------------------------------------

int make_dir(const std::string& path, int /*mode*/) {
  return CreateDirectoryW(widen(path).c_str(), nullptr) ? 0 : last_errno();
}

int remove_file(const std::string& path) {
  const std::wstring w = widen(path);
  if (DeleteFileW(w.c_str())) return 0;
  const DWORD error = GetLastError();
  const DWORD attributes = GetFileAttributesW(w.c_str());
  // A read-only file can't be deleted on Windows; POSIX doesn't care.
  if (error == ERROR_ACCESS_DENIED && attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY)) {
    SetFileAttributesW(w.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
    if (DeleteFileW(w.c_str())) return 0;
  }
  return win::errno_from(error);
}

int remove_dir(const std::string& path) { return RemoveDirectoryW(widen(path).c_str()) ? 0 : last_errno(); }

int set_mode(const std::string& /*path*/, int /*mode*/) { return 0; }

int check_readable(const std::string& path) {
  return GetFileAttributesW(widen(path).c_str()) != INVALID_FILE_ATTRIBUTES ? 0 : last_errno();
}

int rename_replace(const std::string& from, const std::string& to) {
  return MoveFileExW(widen(from).c_str(), widen(to).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
             ? 0
             : last_errno();
}

int rename_no_replace(const std::string& from, const std::string& to, bool directory) {
  // Without MOVEFILE_REPLACE_EXISTING, MoveFileExW fails when the name is taken (ERROR_ALREADY_EXISTS -> EEXIST).
  return MoveFileExW(widen(from).c_str(), widen(to).c_str(), directory ? 0 : MOVEFILE_WRITE_THROUGH) ? 0
                                                                                                    : last_errno();
}

int sync_path(const std::string& path, bool directory) {
  if (directory) return 0;   // folders can't be flushed on Windows; MOVEFILE_WRITE_THROUGH covers the renames
  win::Handle h(CreateFileW(widen(path).c_str(), GENERIC_WRITE, kShareAll, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!h) return last_errno();
  return FlushFileBuffers(h.get()) ? 0 : last_errno();
}

void file_clock_now(int64_t& seconds, int32_t& nanoseconds) {
  FILETIME now{};
  GetSystemTimeAsFileTime(&now);   // the clock that stamps files
  seconds = unix_time(now, nanoseconds);
}

// ---- FolderLock: a named mutex per folder ----------------------------------------------------------------------

FolderLock::FolderLock(const std::string& folder, int wait_ms) {
  // The name: a hash of the absolute, lower-case path (FNV-1a), in this logon session.
  std::wstring path = widen(folder);
  std::vector<wchar_t> full(32768);
  const DWORD n = GetFullPathNameW(path.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr);
  if (n > 0 && n < full.size()) path.assign(full.data(), n);
  while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
  uint64_t hash = 14695981039346656037ull;
  for (wchar_t c : path) {
    c = c == L'/' ? L'\\' : static_cast<wchar_t>(std::towlower(c));
    hash = (hash ^ static_cast<uint64_t>(c)) * 1099511628211ull;
  }
  wchar_t name[64];
  std::swprintf(name, 64, L"Local\\vyrn-folder-%016llx", static_cast<unsigned long long>(hash));
  HANDLE mutex = CreateMutexW(nullptr, FALSE, name);
  if (mutex == nullptr) return;   // can't serialise here: the caller falls back to its own checks
  handle_ = reinterpret_cast<intptr_t>(mutex);
  const DWORD waited = WaitForSingleObject(mutex, static_cast<DWORD>(wait_ms < 0 ? 0 : wait_ms));
  if (waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED) {   // abandoned: its holder died, it is ours now
    held_ = true;
  } else if (waited == WAIT_TIMEOUT) {
    busy_ = true;
  }
}

FolderLock::~FolderLock() {
  if (handle_ == -1) return;
  if (held_) ReleaseMutex(handle_of(handle_));
  CloseHandle(handle_of(handle_));
}

// ---- vyrn/platform.h -------------------------------------------------------------------------------------------

int write_file_atomic(const std::string& path, const std::string& temp, const std::string& content, int mode) {
  remove_file(temp);   // a leftover of an earlier attempt
  int err = 0;
  File file = File::create(temp, File::Create::Exclusive, /*no_follow=*/true, mode, err);
  if (!file) return err;
  err = file.write_all(content.data(), content.size());
  if (err == 0) err = file.sync();
  if (const int closed = file.close(); err == 0) err = closed;
  if (err == 0) err = rename_replace(temp, path);
  if (err != 0) remove_file(temp);
  return err;
}

bool is_writable(const std::string& path) {
  const DWORD attributes = GetFileAttributesW(widen(path).c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_READONLY);
}

}  // namespace vyrn::platform
