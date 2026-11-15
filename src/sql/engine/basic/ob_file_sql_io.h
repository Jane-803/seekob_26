#pragma once

#include <cstdio>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace oceanbase
{
namespace sql
{

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
