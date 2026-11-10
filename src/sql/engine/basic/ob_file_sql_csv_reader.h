#ifndef OCEANBASE_SQL_OB_FILE_SQL_CSV_READER_H_
#define OCEANBASE_SQL_OB_FILE_SQL_CSV_READER_H_

#include <cstdio>
#include <string>
#include <vector>

namespace oceanbase
{
namespace sql
{

class ObFileSqlCsvReader
{
public:
  ObFileSqlCsvReader() : file_(nullptr) {}

  ~ObFileSqlCsvReader()
  {
    close();
  }

  int open(const char *path)
  {
    close();
    file_ = std::fopen(path, "rb");
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
    return file_ == nullptr ? -1 : std::fseek(file_, 0, SEEK_SET);
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
    if (file_ == nullptr) {
      return -1;
    }

    std::string field;
    bool in_quotes = false;
    bool after_quotes = false;
    bool has_data = false;

    while (true) {
      int ch = std::fgetc(file_);

      if (ch == EOF) {
        if (std::ferror(file_) || in_quotes) {
          return -1;
        }
        if (!has_data) {
          return 1;
        }
        row.push_back(field);
        return 0;
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
        return 0;
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
  std::FILE *file_;

  ObFileSqlCsvReader(const ObFileSqlCsvReader &) = delete;
  ObFileSqlCsvReader &operator=(const ObFileSqlCsvReader &) = delete;
};

} // namespace sql
} // namespace oceanbase

#endif
