#pragma once

#include <algorithm>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"
#include "sql/engine/basic/ob_file_sql_io.h"
#include "sql/engine/basic/ob_file_sql_type_infer.h"

namespace oceanbase
{
namespace sql
{

class ObFileSqlJsonReader
{
public:
  ObFileSqlJsonReader() : file_(nullptr), initial_stat_(), line_number_(0) {}
  ~ObFileSqlJsonReader() { close(); }

  int open(const char *path)
  {
    close();
    file_ = open_file_sql_regular(path, initial_stat_);
    return file_ == nullptr ? -1 : 0;
  }

  void close()
  {
    if (file_ != nullptr) {
      std::fclose(file_);
      file_ = nullptr;
    }
    line_number_ = 0;
  }

  int rescan()
  {
    if (file_ == nullptr || !file_sql_stat_unchanged(file_, initial_stat_) ||
        std::fseek(file_, 0, SEEK_SET) != 0) {
      return -1;
    }
    line_number_ = 0;
    return 0;
  }

  int next_document(rapidjson::Document &doc)
  {
    if (file_ == nullptr || !file_sql_stat_unchanged(file_, initial_stat_)) {
      return -1;
    }
    std::string line;
    int ch = 0;
    while ((ch = std::fgetc(file_)) != EOF && ch != '\n') {
      if (line.size() >= MAX_LINE_LENGTH) {
        return -1;
      }
      line.push_back(static_cast<char>(ch));
    }
    if (ch == EOF && std::ferror(file_)) {
      return -1;
    }
    if (line.empty() && ch == EOF) {
      return file_sql_stat_unchanged(file_, initial_stat_) ? 1 : -1;
    }
    ++line_number_;
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty() || !file_sql_stat_unchanged(file_, initial_stat_)) {
      return -1;
    }
    doc.Parse(line.data(), line.size());
    return doc.HasParseError() || !doc.IsObject() ? -1 : 0;
  }

  int next_row(const std::vector<std::string> &columns,
               std::vector<std::string> &row,
               std::vector<bool> &nulls)
  {
    rapidjson::Document doc;
    int status = next_document(doc);
    if (status != 0) {
      return status;
    }
    row.assign(columns.size(), std::string());
    nulls.assign(columns.size(), true);
    std::vector<bool> seen(columns.size(), false);
    for (auto it = doc.MemberBegin(); it != doc.MemberEnd(); ++it) {
      if (!it->name.IsString() || !is_scalar(it->value)) {
        return -1;
      }
      const std::string name(it->name.GetString(), it->name.GetStringLength());
      const auto column = std::find(columns.begin(), columns.end(), name);
      if (column == columns.end()) {
        return -1;
      }
      const size_t index = static_cast<size_t>(column - columns.begin());
      if (seen[index]) {
        return -1;
      }
      seen[index] = true;
      if (!it->value.IsNull()) {
        nulls[index] = false;
        if (it->value.IsString()) {
          row[index].assign(it->value.GetString(), it->value.GetStringLength());
        } else {
          rapidjson::StringBuffer buffer;
          rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
          it->value.Accept(writer);
          row[index].assign(buffer.GetString(), buffer.GetSize());
        }
      }
    }
    return 0;
  }

  int64_t line_number() const { return line_number_; }

  static bool is_scalar(const rapidjson::Value &value)
  {
    return value.IsNull() || value.IsBool() || value.IsNumber() || value.IsString();
  }

  static ObFileSqlType infer_type(const rapidjson::Value &value)
  {
    if (value.IsNull()) return ObFileSqlType::NULL_VALUE;
    if (value.IsBool()) return ObFileSqlType::BOOLEAN;
    if (value.IsInt64()) return ObFileSqlType::INTEGER;
    if (value.IsUint64()) {
      return value.GetUint64() <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
          ? ObFileSqlType::INTEGER : ObFileSqlType::STRING;
    }
    if (value.IsNumber()) return ObFileSqlType::DOUBLE;
    const std::string text(value.GetString(), value.GetStringLength());
    if (is_iso_datetime(text)) return ObFileSqlType::DATETIME;
    if (is_iso_date(text)) return ObFileSqlType::DATE;
    return ObFileSqlType::STRING;
  }

private:
  static const size_t MAX_LINE_LENGTH = 16 * 1024 * 1024;
  std::FILE *file_;
  struct stat initial_stat_;
  int64_t line_number_;

  ObFileSqlJsonReader(const ObFileSqlJsonReader &) = delete;
  ObFileSqlJsonReader &operator=(const ObFileSqlJsonReader &) = delete;
};

inline int infer_file_sql_json_schema(const char *path,
    std::vector<std::string> &columns, std::vector<ObFileSqlType> &types,
    std::vector<bool> *nullable = nullptr)
{
  columns.clear();
  types.clear();
  if (nullable != nullptr) nullable->clear();
  ObFileSqlJsonReader reader;
  if (reader.open(path) != 0) {
    return -1;
  }
  rapidjson::Document doc;
  int status = 0;
  int64_t document_count = 0;
  while ((status = reader.next_document(doc)) == 0) {
    std::vector<std::string> row_names;
    for (auto it = doc.MemberBegin(); it != doc.MemberEnd(); ++it) {
      if (!it->name.IsString() || !ObFileSqlJsonReader::is_scalar(it->value)) {
        return -1;
      }
      const std::string name(it->name.GetString(), it->name.GetStringLength());
      if (name.empty() || name.find('\0') != std::string::npos) {
        return -1;
      }
      if (std::find(row_names.begin(), row_names.end(), name) != row_names.end()) {
        return -1;
      }
      row_names.push_back(name);
      auto found = std::find(columns.begin(), columns.end(), name);
      if (found == columns.end()) {
        if (columns.size() >= 4096) {
          return -1;
        }
        columns.push_back(name);
        types.push_back(ObFileSqlType::NULL_VALUE);
        if (nullable != nullptr) nullable->push_back(document_count > 0);
        found = columns.end() - 1;
      }
      const size_t index = static_cast<size_t>(found - columns.begin());
      types[index] = merge_csv_types(types[index], ObFileSqlJsonReader::infer_type(it->value));
      if (nullable != nullptr && it->value.IsNull()) (*nullable)[index] = true;
    }
    if (nullable != nullptr) {
      for (size_t i = 0; i < columns.size(); ++i) {
        if (std::find(row_names.begin(), row_names.end(), columns[i]) == row_names.end()) {
          (*nullable)[i] = true;
        }
      }
    }
    ++document_count;
  }
  if (status != 1 || columns.empty()) {
    return -1;
  }
  for (size_t i = 0; i < types.size(); ++i) {
    if (types[i] == ObFileSqlType::NULL_VALUE) {
      types[i] = ObFileSqlType::STRING;
      if (nullable != nullptr) (*nullable)[i] = true;
    }
  }
  return 0;
}

} // namespace sql
} // namespace oceanbase
