#pragma once

#include "sql/engine/basic/ob_file_sql_csv_reader.h"
#include "sql/engine/basic/ob_file_sql_type_infer.h"

namespace oceanbase
{
namespace sql
{

inline int infer_file_sql_csv_schema(
    const char *path,
    std::vector<std::string> &columns,
    std::vector<ObFileSqlType> &types,
    bool has_header = true,
    std::vector<bool> *nullable = nullptr,
    char delimiter = ',', char quote = '"', char escape = '"',
    const std::string &nullstr = "")
{
  columns.clear();
  types.clear();

  ObFileSqlCsvReader reader;
  reader.set_format(delimiter, quote, escape);
  reader.set_nullstr(nullstr);
  if (reader.open(path) != 0) {
    return -1;
  }

  if (has_header) {
    if (reader.read_header(columns) != 0 || columns.empty()) {
      return -1;
    }
  } else {
    std::vector<std::string> first_row;
    if (reader.next_row(first_row) != 0 || first_row.empty() ||
        reader.rescan() != 0) {
      return -1;
    }
    for (size_t i = 0; i < first_row.size(); ++i) {
      columns.push_back("column" + std::to_string(i + 1));
    }
  }

  deduplicate_csv_columns(columns);
  types.assign(columns.size(), ObFileSqlType::NULL_VALUE);
  if (nullable != nullptr) {
    nullable->assign(columns.size(), false);
  }

  std::vector<std::string> row;
  std::vector<bool> nulls;
  int status = 0;

  while ((status = reader.next_row(row, &nulls)) == 0) {
    if (row.size() != columns.size()) {
      return -1;
    }

    for (size_t i = 0; i < row.size(); ++i) {
      types[i] = merge_csv_types(types[i], nulls[i] ? ObFileSqlType::NULL_VALUE
          : row[i].empty() ? ObFileSqlType::STRING : infer_csv_value(row[i]));
      if (nullable != nullptr && nulls[i]) {
        (*nullable)[i] = true;
      }
    }
  }

  if (status != 1) {
    return -1;
  }

  for (size_t i = 0; i < types.size(); ++i) {
    if (types[i] == ObFileSqlType::NULL_VALUE) {
      types[i] = ObFileSqlType::STRING;
      if (nullable != nullptr) {
        (*nullable)[i] = true;
      }
    }
  }

  return 0;
}

} // namespace sql
} // namespace oceanbase

