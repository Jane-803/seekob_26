#pragma once

#include <algorithm>
#include <string>
#include <vector>
#include <unistd.h>

#include "file_sql_parquet.h"
#include "sql/engine/basic/ob_file_sql_io.h"
#include "sql/engine/basic/ob_file_sql_type_infer.h"

namespace oceanbase
{
namespace sql
{

class ObFileSqlParquetReader
{
public:
  ObFileSqlParquetReader() : handle_(nullptr), check_fd_(-1), initial_stat_() {}
  ~ObFileSqlParquetReader() { close(); }

  void set_projection(const std::vector<int64_t> &projection) { projection_ = projection; }

  int open(const char *path)
  {
    close();
    struct stat opened_stat;
    std::FILE *file = open_file_sql_regular(path, opened_stat);
    if (file == nullptr) {
      return -1;
    }
    const int read_fd = ::dup(::fileno(file));
    check_fd_ = ::dup(::fileno(file));
    std::fclose(file);
    if (read_fd < 0 || check_fd_ < 0) {
      if (read_fd >= 0) ::close(read_fd);
      close();
      return -1;
    }
    handle_ = fs_parquet_open_fd(read_fd, projection_.data(), projection_.size());
    if (handle_ == nullptr) {
      close();
      return -1;
    }
    initial_stat_ = opened_stat;
    path_.assign(path);
    const int64_t count = fs_parquet_column_count(handle_);
    if (count <= 0 || count > 4096) {
      close();
      return -1;
    }
    columns_.clear();
    types_.clear();
    nullable_.clear();
    for (int64_t i = 0; i < count; ++i) {
      const int64_t length = fs_parquet_column_name_len(handle_, i);
      const int kind = fs_parquet_column_type(handle_, i);
      const int nullable = fs_parquet_column_nullable(handle_, i);
      if (length <= 0 || length > 65535 || kind < 0 || kind > 6 ||
          nullable < 0 || nullable > 1) {
        close();
        return -1;
      }
      std::string name(static_cast<size_t>(length), '\0');
      if (fs_parquet_column_name_copy(handle_, i, name.data(), name.size()) != 0 ||
          name.find('\0') != std::string::npos ||
          std::find(columns_.begin(), columns_.end(), name) != columns_.end()) {
        close();
        return -1;
      }
      columns_.push_back(name);
      types_.push_back(static_cast<ObFileSqlType>(kind));
      nullable_.push_back(nullable == 1);
    }
    return 0;
  }

  void close()
  {
    if (handle_ != nullptr) {
      fs_parquet_close(handle_);
      handle_ = nullptr;
    }
    if (check_fd_ >= 0) {
      ::close(check_fd_);
      check_fd_ = -1;
    }
    path_.clear();
    columns_.clear();
    types_.clear();
    nullable_.clear();
  }

  int rescan()
  {
    if (handle_ == nullptr || !unchanged()) {
      return -1;
    }
    const std::string path = path_;
    const struct stat original = initial_stat_;
    if (open(path.c_str()) != 0) {
      return -1;
    }
    if (original.st_dev != initial_stat_.st_dev ||
        original.st_ino != initial_stat_.st_ino ||
        original.st_size != initial_stat_.st_size ||
        original.st_mtim.tv_sec != initial_stat_.st_mtim.tv_sec ||
        original.st_mtim.tv_nsec != initial_stat_.st_mtim.tv_nsec ||
        original.st_ctim.tv_sec != initial_stat_.st_ctim.tv_sec ||
        original.st_ctim.tv_nsec != initial_stat_.st_ctim.tv_nsec) {
      close();
      return -1;
    }
    return 0;
  }

  int next_row(std::vector<std::string> &row, std::vector<bool> &nulls)
  {
    if (handle_ == nullptr || !unchanged()) {
      return -1;
    }
    const int status = fs_parquet_next(handle_);
    if (status != 0) {
      return status == 1 && unchanged() ? 1 : -1;
    }
    row.assign(columns_.size(), std::string());
    nulls.assign(columns_.size(), true);
    const size_t value_count = projection_.empty() ? columns_.size() : projection_.size();
    for (size_t i = 0; i < value_count; ++i) {
      const size_t column_index = projection_.empty() ? i : static_cast<size_t>(projection_[i]);
      const int64_t length = fs_parquet_value_len(handle_, static_cast<int64_t>(i));
      if (length == -1) {
      } else if (length < 0 || length > 16 * 1024 * 1024) {
        return -1;
      } else {
        nulls[column_index] = false;
        row[column_index].resize(static_cast<size_t>(length));
        if (length > 0 && fs_parquet_value_copy(handle_, static_cast<int64_t>(i),
                                                 row[column_index].data(), row[column_index].size()) != 0) {
          return -1;
        }
      }
    }
    return unchanged() ? 0 : -1;
  }

  const std::vector<std::string> &columns() const { return columns_; }
  const std::vector<ObFileSqlType> &types() const { return types_; }
  const std::vector<bool> &nullable() const { return nullable_; }

private:
  bool unchanged() const
  {
    struct stat now;
    return check_fd_ >= 0 && ::fstat(check_fd_, &now) == 0 &&
           now.st_dev == initial_stat_.st_dev &&
           now.st_ino == initial_stat_.st_ino &&
           now.st_size == initial_stat_.st_size &&
           now.st_mtim.tv_sec == initial_stat_.st_mtim.tv_sec &&
           now.st_mtim.tv_nsec == initial_stat_.st_mtim.tv_nsec &&
           now.st_ctim.tv_sec == initial_stat_.st_ctim.tv_sec &&
           now.st_ctim.tv_nsec == initial_stat_.st_ctim.tv_nsec;
  }

  void *handle_;
  int check_fd_;
  struct stat initial_stat_;
  std::string path_;
  std::vector<std::string> columns_;
  std::vector<ObFileSqlType> types_;
  std::vector<bool> nullable_;
  std::vector<int64_t> projection_;

  ObFileSqlParquetReader(const ObFileSqlParquetReader &) = delete;
  ObFileSqlParquetReader &operator=(const ObFileSqlParquetReader &) = delete;
};

inline int infer_file_sql_parquet_schema(const char *path,
    std::vector<std::string> &columns, std::vector<ObFileSqlType> &types,
    std::vector<bool> *nullable = nullptr)
{
  ObFileSqlParquetReader reader;
  if (reader.open(path) != 0) {
    return -1;
  }
  columns = reader.columns();
  types = reader.types();
  if (nullable != nullptr) *nullable = reader.nullable();
  return 0;
}

} // namespace sql
} // namespace oceanbase
