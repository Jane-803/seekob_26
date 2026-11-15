#pragma once

#include <cstdint>

namespace oceanbase
{
namespace sql
{

enum FileSqlFormat : int64_t
{
  FILE_SQL_CSV = 0,
  FILE_SQL_JSONL = 1,
  FILE_SQL_PARQUET = 2,
  FILE_SQL_GLOB = 3
};

} // namespace sql
} // namespace oceanbase
