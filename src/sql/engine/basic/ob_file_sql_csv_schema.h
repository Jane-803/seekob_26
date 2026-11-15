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
    std::vector<ObFileSqlType> &types)
{
  columns.clear();
  types.clear();

  ObFileSqlCsvReader reader;
  if (reader.open(path) != 0) {
    return -1;
  }

  if (reader.read_header(columns) != 0 || columns.empty()) {
    return -1;
  }

  deduplicate_csv_columns(columns);
  types.assign(columns.size(), ObFileSqlType::NULL_VALUE);

  std::vector<std::string> row;
  int status = 0;

  while ((status = reader.next_row(row)) == 0) {
    if (row.size() != columns.size()) {
      return -1;
    }

    for (size_t i = 0; i < row.size(); ++i) {
      types[i] = merge_csv_types(types[i], infer_csv_value(row[i]));
    }
  }

  if (status != 1) {
    return -1;
  }

  for (size_t i = 0; i < types.size(); ++i) {
    if (types[i] == ObFileSqlType::NULL_VALUE) {
      types[i] = ObFileSqlType::STRING;
    }
  }

  return 0;
}

} // namespace sql
} // namespace oceanbase

