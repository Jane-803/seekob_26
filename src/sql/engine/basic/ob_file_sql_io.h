#pragma once

#include <cstdio>
#include <fcntl.h>
#include <limits.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace oceanbase
{
namespace sql
{

inline bool file_sql_secure_path_allowed(const char *path,
                                         const char *secure_dir,
                                         size_t secure_dir_len)
{
  if (path == nullptr || secure_dir == nullptr || secure_dir_len == 0 ||
      std::string(secure_dir, secure_dir_len).find('\0') != std::string::npos) {
    return false;
  }
  const std::string allowed_dir(secure_dir, secure_dir_len);
  char canonical_dir[PATH_MAX];
  char canonical_path[PATH_MAX];
  struct stat info;
  if (::realpath(allowed_dir.c_str(), canonical_dir) == nullptr ||
      ::stat(canonical_dir, &info) != 0 || !S_ISDIR(info.st_mode) ||
      ::realpath(path, canonical_path) == nullptr) {
    return false;
  }
  const std::string prefix(canonical_dir);
  const std::string candidate(canonical_path);
  return prefix == "/" ? candidate[0] == '/'
      : candidate.size() > prefix.size() &&
        candidate.compare(0, prefix.size(), prefix) == 0 &&
        candidate[prefix.size()] == '/';
}

inline std::FILE *open_file_sql_regular(const char *path, struct stat &info)
{
  if (path == nullptr || path[0] != '/') {
    return nullptr;
  }
  int dir_fd = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dir_fd < 0) {
    return nullptr;
  }
  const std::string full_path(path);
  size_t begin = 1;
  int file_fd = -1;
  while (begin < full_path.size()) {
    const size_t end = full_path.find('/', begin);
    const bool last = end == std::string::npos;
    const std::string component = full_path.substr(begin, last ? end : end - begin);
    if (component.empty() || component == "." || component == "..") {
      break;
    }
    const int next_fd = ::openat(dir_fd, component.c_str(),
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW | (last ? O_NONBLOCK : O_DIRECTORY));
    if (next_fd < 0) {
      break;
    }
    if (last) {
      file_fd = next_fd;
      break;
    } else {
      ::close(dir_fd);
      dir_fd = next_fd;
      begin = end + 1;
    }
  }
  ::close(dir_fd);
  if (file_fd < 0) {
    return nullptr;
  }
  if (::fstat(file_fd, &info) != 0 || !S_ISREG(info.st_mode)) {
    ::close(file_fd);
    return nullptr;
  }
  std::FILE *file = ::fdopen(file_fd, "rb");
  if (file == nullptr) {
    ::close(file_fd);
  }
  return file;
}

inline bool file_sql_stat_unchanged(std::FILE *file, const struct stat &initial)
{
  struct stat now;
  return file != nullptr && ::fstat(::fileno(file), &now) == 0 &&
         now.st_dev == initial.st_dev && now.st_ino == initial.st_ino &&
         now.st_size == initial.st_size &&
         now.st_mtim.tv_sec == initial.st_mtim.tv_sec &&
         now.st_mtim.tv_nsec == initial.st_mtim.tv_nsec &&
         now.st_ctim.tv_sec == initial.st_ctim.tv_sec &&
         now.st_ctim.tv_nsec == initial.st_ctim.tv_nsec;
}

} // namespace sql
} // namespace oceanbase
