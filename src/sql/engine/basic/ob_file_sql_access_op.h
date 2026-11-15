#pragma once

#include <string>
#include <vector>
#include "sql/engine/ob_operator.h"
#include "sql/engine/basic/ob_file_sql_csv_reader.h"
#include "sql/engine/basic/ob_file_sql_json_reader.h"
#include "sql/engine/basic/ob_file_sql_parquet_reader.h"
#include "sql/engine/basic/ob_file_sql_format.h"

namespace oceanbase
{
namespace sql
{

class ObFileSqlAccessSpec : public ObOpSpec
{
  OB_UNIS_VERSION_V(1);
public:
  ObFileSqlAccessSpec(common::ObIAllocator &alloc,
                      const ObPhyOperatorType type)
    : ObOpSpec(alloc, type),
      file_path_(),
      file_format_(0),
      has_header_(true),
      column_exprs_(alloc),
      column_names_(alloc)
  {
  }

  common::ObString file_path_;
  int64_t file_format_;
  bool has_header_;
  common::ObFixedArray<ObExpr *, common::ObIAllocator> column_exprs_;
  common::ObFixedArray<common::ObString, common::ObIAllocator> column_names_;

private:
  DISALLOW_COPY_AND_ASSIGN(ObFileSqlAccessSpec);
};

class ObFileSqlAccessOp : public ObOperator
{
public:
  ObFileSqlAccessOp(ObExecContext &exec_ctx,
                    const ObOpSpec &spec,
                    ObOpInput *input);

  virtual int inner_open() override;
  virtual int inner_rescan() override;
  virtual int inner_get_next_row() override;
  virtual int inner_close() override;
  virtual void destroy() override;

private:
  ObFileSqlCsvReader reader_;
  ObFileSqlJsonReader json_reader_;
  ObFileSqlParquetReader parquet_reader_;
  std::vector<std::string> row_;
  std::vector<std::string> column_names_;
  std::vector<bool> nulls_;
  std::vector<std::string> glob_paths_;
  size_t glob_index_ = 0;

  DISALLOW_COPY_AND_ASSIGN(ObFileSqlAccessOp);
};

} // namespace sql
} // namespace oceanbase

