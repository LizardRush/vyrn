// POSIX primitives whose Linux and macOS versions differ: system_linux.cpp and system_mac.cpp implement them.
// Only the *_posix.cpp files use this header.
#pragma once

#include <spawn.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <cstddef>
#include <cstdint>

namespace vyrn::platform::posix {

int pipe_cloexec(int fds[2]);         // 0 or the errno; both ends close-on-exec (Linux: pipe2)
int socketpair_cloexec(int fds[2]);   // AF_UNIX stream pair, close-on-exec, sends never raise SIGPIPE (macOS:
                                      // SO_NOSIGPIPE on both ends)
ssize_t send_nosignal(int fd, const void* data, size_t size);   // Linux: MSG_NOSIGNAL
void no_sigpipe(int fd);   // a socket made elsewhere (accept, socket): macOS sets SO_NOSIGPIPE; Linux needs nothing
// Makes the child close every descriptor from `first` up that the file actions didn't set up (Linux:
// posix_spawn_file_actions_addclosefrom_np; macOS: POSIX_SPAWN_CLOEXEC_DEFAULT, added to *flags). 0 or the errno.
int close_fds_from(posix_spawn_file_actions_t* actions, short* flags, int first);
void stat_mtime(const struct stat& st, int64_t& seconds, int32_t& nanoseconds);   // st_mtim / st_mtimespec
// rename that fails with EEXIST instead of replacing (Linux renameat2 RENAME_NOREPLACE, macOS renamex_np
// RENAME_EXCL). 0 or the errno; EINVAL, ENOSYS or ENOTSUP: this file system can't.
int rename_exclusive(const char* from, const char* to);

}  // namespace vyrn::platform::posix
