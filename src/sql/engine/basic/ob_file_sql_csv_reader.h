#pragma once

#include <cstdio>
#include <cerrno>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace oceanbase
{
namespace sql
{

class ObFileSqlCsvReader
{
public:
  ObFileSqlCsvReader() : file_(nullptr), initial_stat_() {}

  ~ObFileSqlCsvReader()
  {
    close();
  }

  int open(const char *path)
  {
    close();
    if (path == nullptr || path[0] != '/') {
      return -1;
    }
    int dir_fd = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir_fd < 0) {
      return -1;
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
      } else {
        ::close(dir_fd);
        dir_fd = next_fd;
        begin = end + 1;
      }
    }
    ::close(dir_fd);
    if (file_fd < 0) {
      return -1;
    }
    if (::fstat(file_fd, &initial_stat_) != 0 || !S_ISREG(initial_stat_.st_mode) ||
        (file_ = ::fdopen(file_fd, "rb")) == nullptr) {
      ::close(file_fd);
      return -1;
    }
    return 0;
  }

  int read_header(std::vector<std::string> &columns)
  {
    if (rescan() != 0) {
      return -1;
    }
    return next_row(columns) == 0 ? 0 : -1;
  }

  int rescan()
  {
    return file_ == nullptr || !unchanged() ? -1 : std::fseek(file_, 0, SEEK_SET);
  }

  void close()
  {
    if (file_ != nullptr) {
      std::fclose(file_);
      file_ = nullptr;
    }
  }

  // 返回 0 表示读到一行，1 表示文件结束，-1 表示错误。
  int next_row(std::vector<std::string> &row)
  {
    row.clear();
    if (file_ == nullptr || !unchanged()) {
      return -1;
    }

    std::string field;
    bool in_quotes = false;
    bool after_quotes = false;
    bool has_data = false;

    while (true) {
      int ch = std::fgetc(file_);

      if (ch == EOF) {
        if (std::ferror(file_) || in_quotes || !unchanged()) {
          return -1;
        }
        if (!has_data) {
          return 1;
        }
        row.push_back(field);
        return unchanged() ? 0 : -1;
      }

      has_data = true;

      if (in_quotes) {
        if (ch == '"') {
          int next = std::fgetc(file_);
          if (next == '"') {
            field.push_back('"');
          } else {
            if (next != EOF) {
              std::ungetc(next, file_);
            }
            in_quotes = false;
            after_quotes = true;
          }
        } else {
          field.push_back(static_cast<char>(ch));
        }
      } else if (ch == ',') {
        row.push_back(field);
        field.clear();
        after_quotes = false;
      } else if (ch == '\n' || ch == '\r') {
        if (ch == '\r') {
          int next = std::fgetc(file_);
          if (next != '\n' && next != EOF) {
            std::ungetc(next, file_);
          }
        }
        row.push_back(field);
        return unchanged() ? 0 : -1;
      } else if (ch == '"' && field.empty() && !after_quotes) {
        in_quotes = true;
      } else if (after_quotes || ch == '"') {
        return -1;
      } else {
        field.push_back(static_cast<char>(ch));
      }
    }
  }

private:
  bool unchanged() const
  {
    struct stat now;
    return ::fstat(::fileno(file_), &now) == 0 &&
           now.st_dev == initial_stat_.st_dev &&
           now.st_ino == initial_stat_.st_ino &&
           now.st_size == initial_stat_.st_size &&
           now.st_mtim.tv_sec == initial_stat_.st_mtim.tv_sec &&
           now.st_mtim.tv_nsec == initial_stat_.st_mtim.tv_nsec &&
           now.st_ctim.tv_sec == initial_stat_.st_ctim.tv_sec &&
           now.st_ctim.tv_nsec == initial_stat_.st_ctim.tv_nsec;
  }

  std::FILE *file_;
  struct stat initial_stat_;

  ObFileSqlCsvReader(const ObFileSqlCsvReader &) = delete;
  ObFileSqlCsvReader &operator=(const ObFileSqlCsvReader &) = delete;
};

} // namespace sql
} // namespace oceanbase
