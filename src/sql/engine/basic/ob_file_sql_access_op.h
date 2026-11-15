#pragma once

#include <string>
#include <vector>
#include "sql/engine/ob_operator.h"
#include "sql/engine/basic/ob_file_sql_csv_reader.h"

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
      column_exprs_(alloc)
  {
  }

  common::ObString file_path_;
  common::ObFixedArray<ObExpr *, common::ObIAllocator> column_exprs_;

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
  std::vector<std::string> row_;

  DISALLOW_COPY_AND_ASSIGN(ObFileSqlAccessOp);
};

} // namespace sql
} // namespace oceanbase

