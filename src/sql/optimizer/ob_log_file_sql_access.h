#pragma once

#include "sql/optimizer/ob_logical_operator.h"

namespace oceanbase
{
namespace sql
{

class ObLogFileSqlAccess : public ObLogicalOperator
{
public:
  explicit ObLogFileSqlAccess(ObLogPlan &plan)
    : ObLogicalOperator(plan),
      table_id_(common::OB_INVALID_ID),
      file_path_(),
      file_format_(0),
      has_header_(true)
  {
  }

  virtual ~ObLogFileSqlAccess() {}

  void set_table_id(uint64_t id) { table_id_ = id; }
  uint64_t get_table_id() const { return table_id_; }

  void set_file_path(const common::ObString &path)
  {
    file_path_ = path;
  }

  const common::ObString &get_file_path() const
  {
    return file_path_;
  }

  void set_file_format(int64_t format) { file_format_ = format; }
  int64_t get_file_format() const { return file_format_; }
  void set_has_header(bool value) { has_header_ = value; }
  bool has_header() const { return has_header_; }
  void set_csv_format(char delimiter, char quote, char escape)
  {
    delimiter_ = delimiter;
    quote_ = quote;
    escape_ = escape;
  }
  char delimiter() const { return delimiter_; }
  char quote() const { return quote_; }
  char escape() const { return escape_; }
  void set_nullstr(const common::ObString &value) { nullstr_ = value; }
  const common::ObString &nullstr() const { return nullstr_; }
  void set_file_stat(int64_t device, int64_t inode, int64_t size,
                     int64_t mtime_ns, int64_t ctime_ns)
  {
    file_device_ = device;
    file_inode_ = inode;
    file_size_ = size;
    file_mtime_ns_ = mtime_ns;
    file_ctime_ns_ = ctime_ns;
  }
  int64_t file_device() const { return file_device_; }
  int64_t file_inode() const { return file_inode_; }
  int64_t file_size() const { return file_size_; }
  int64_t file_mtime_ns() const { return file_mtime_ns_; }
  int64_t file_ctime_ns() const { return file_ctime_ns_; }

  common::ObIArray<ObColumnRefRawExpr *> &get_column_exprs()
  {
    return column_exprs_;
  }

  const common::ObIArray<ObColumnRefRawExpr *> &get_column_exprs() const
  {
    return column_exprs_;
  }

  virtual int est_cost() override
  {
    set_card(1000.0);
    set_op_cost(1000.0);
    set_cost(1000.0);
    return OB_SUCCESS;
  }

  virtual int compute_equal_set() override
  {
    set_output_equal_sets(&empty_expr_sets_);
    return OB_SUCCESS;
  }

  virtual int compute_table_set() override
  {
    set_table_set(&empty_table_set_);
    return OB_SUCCESS;
  }

  virtual int is_my_fixed_expr(const ObRawExpr *expr, bool &is_fixed) override
  {
    is_fixed = false;
    for (int64_t i = 0; !is_fixed && i < column_exprs_.count(); ++i) {
      is_fixed = (column_exprs_.at(i) == expr);
    }
    return OB_SUCCESS;
  }

  virtual int get_op_exprs(
      common::ObIArray<ObRawExpr *> &all_exprs) override
  {
    int ret = ObLogicalOperator::get_op_exprs(all_exprs);
    if (OB_SUCC(ret)) {
      ret = append(all_exprs, column_exprs_);
    }
    return ret;
  }

  virtual int allocate_expr_post(ObAllocExprContext &ctx) override
  {
    int ret = OB_SUCCESS;
    for (int64_t i = 0; OB_SUCC(ret) &&
                        i < column_exprs_.count(); ++i) {
      ObColumnRefRawExpr *expr = column_exprs_.at(i);
      if (OB_ISNULL(expr)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(
                     mark_expr_produced(expr, branch_id_, id_, ctx))) {
      } else if (!is_plan_root() &&
                 OB_FAIL(output_exprs_.push_back(expr))) {
      }
    }

    if (OB_SUCC(ret)) {
      ret = ObLogicalOperator::allocate_expr_post(ctx);
    }
    return ret;
  }

private:
  uint64_t table_id_;
  common::ObString file_path_;
  int64_t file_format_;
  bool has_header_;
  char delimiter_ = ',';
  char quote_ = '"';
  char escape_ = '"';
  common::ObString nullstr_;
  int64_t file_device_ = 0;
  int64_t file_inode_ = 0;
  int64_t file_size_ = 0;
  int64_t file_mtime_ns_ = 0;
  int64_t file_ctime_ns_ = 0;
  common::ObSEArray<ObColumnRefRawExpr *, 4,
                    common::ModulePageAllocator, true> column_exprs_;

  DISALLOW_COPY_AND_ASSIGN(ObLogFileSqlAccess);
};

} // namespace sql
} // namespace oceanbase

