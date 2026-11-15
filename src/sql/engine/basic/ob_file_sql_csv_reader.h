#pragma once

#include <cstdio>
#include "sql/engine/basic/ob_file_sql_io.h"
#include <string>
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
    file_ = open_file_sql_regular(path, initial_stat_);
    return file_ == nullptr ? -1 : 0;
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
  int next_row(std::vector<std::string> &row, std::vector<bool> *nulls = nullptr)
  {
    row.clear();
    if (nulls != nullptr) {
      nulls->clear();
    }
    if (file_ == nullptr || !unchanged()) {
      return -1;
    }

    std::string field;
    bool in_quotes = false;
    bool after_quotes = false;
    bool has_data = false;
    bool quoted = false;
    size_t row_bytes = 0;

    const auto finish_field = [&]() {
      if (nulls != nullptr) {
        nulls->push_back(field.empty() && !quoted);
      }
      row.push_back(field);
      field.clear();
      quoted = false;
      after_quotes = false;
    };

    while (true) {
      int ch = std::fgetc(file_);

      if (ch == EOF) {
        if (std::ferror(file_) || in_quotes || !unchanged()) {
          return -1;
        }
        if (!has_data) {
          return 1;
        }
        finish_field();
        return unchanged() ? 0 : -1;
      }

      has_data = true;
      if (++row_bytes > 16 * 1024 * 1024) {
        return -1;
      }

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
        finish_field();
        if (row.size() >= 4096) {
          return -1;
        }
      } else if (ch == '\n' || ch == '\r') {
        if (ch == '\r') {
          int next = std::fgetc(file_);
          if (next != '\n' && next != EOF) {
            std::ungetc(next, file_);
          }
        }
        finish_field();
        return unchanged() ? 0 : -1;
      } else if (ch == '"' && field.empty() && !after_quotes) {
        in_quotes = true;
        quoted = true;
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
    return file_sql_stat_unchanged(file_, initial_stat_);
  }

  std::FILE *file_;
  struct stat initial_stat_;

  ObFileSqlCsvReader(const ObFileSqlCsvReader &) = delete;
  ObFileSqlCsvReader &operator=(const ObFileSqlCsvReader &) = delete;
};

} // namespace sql
} // namespace oceanbase
