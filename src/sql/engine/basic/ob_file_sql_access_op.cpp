#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/basic/ob_file_sql_access_op.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/expr/ob_obj_cast_runtime.h"

namespace oceanbase
{
using namespace common;

namespace sql
{

OB_SERIALIZE_MEMBER_INHERIT(ObFileSqlAccessSpec,
                            ObOpSpec,
                            file_path_,
                            column_exprs_);

ObFileSqlAccessOp::ObFileSqlAccessOp(
    ObExecContext &exec_ctx,
    const ObOpSpec &spec,
    ObOpInput *input)
  : ObOperator(exec_ctx, spec, input),
    reader_(),
    row_()
{
}

int ObFileSqlAccessOp::inner_open()
{
  int ret = OB_SUCCESS;

  if (MY_SPEC.file_path_.empty() ||
      MY_SPEC.column_exprs_.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    const std::string path(
        MY_SPEC.file_path_.ptr(),
        MY_SPEC.file_path_.length());

    if (reader_.open(path.c_str()) != 0) {
      ret = OB_IO_ERROR;
    } else if (reader_.read_header(row_) != 0) {
      ret = OB_ERR_UNEXPECTED;
      reader_.close();
    } else if (row_.size() !=
               static_cast<size_t>(MY_SPEC.column_exprs_.count())) {
      ret = OB_ERR_UNEXPECTED;
      reader_.close();
    }
  }

  return ret;
}

int ObFileSqlAccessOp::inner_rescan()
{
  int ret = OB_SUCCESS;

  if (OB_FAIL(ObOperator::inner_rescan())) {
  } else if (reader_.read_header(row_) != 0) {
    ret = OB_ERR_UNEXPECTED;
  } else if (row_.size() !=
             static_cast<size_t>(MY_SPEC.column_exprs_.count())) {
    ret = OB_ERR_UNEXPECTED;
  }

  return ret;
}

int ObFileSqlAccessOp::inner_get_next_row()
{
  int ret = OB_SUCCESS;

  if (OB_FAIL(try_check_status())) {
  } else {
    clear_evaluated_flag();

    const int read_ret = reader_.next_row(row_);

    if (read_ret == 1) {
      ret = OB_ITER_END;
    } else if (read_ret != 0) {
      ret = OB_IO_ERROR;
    } else if (row_.size() !=
               static_cast<size_t>(MY_SPEC.column_exprs_.count())) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      ObCastMode cast_mode = CM_NONE;

      if (OB_FAIL(ObSQLUtils::get_default_cast_mode(
              false, 0, GET_MY_SESSION(ctx_), cast_mode))) {
      } else {
        cast_mode = cast_mode | CM_COLUMN_CONVERT;

        const ObDataTypeCastParams dtc_params =
            ObBasicSessionInfo::create_dtc_params(
                GET_MY_SESSION(ctx_));

        ObSqlObjCastRuntime cast_runtime(&eval_ctx_.exec_ctx_);

        for (int64_t i = 0;
             OB_SUCC(ret) && i < MY_SPEC.column_exprs_.count();
             ++i) {
          ObExpr *expr = MY_SPEC.column_exprs_.at(i);

          if (OB_ISNULL(expr)) {
            ret = OB_ERR_UNEXPECTED;
          } else {
            const std::string &value = row_[i];
            ObObj src_obj;
            ObObj dst_obj;

            if (value.empty()) {
              src_obj.set_null();
            } else if (expr->obj_meta_.get_type() ==
                       ObTinyIntType &&
                       (value == "true" || value == "TRUE" ||
                        value == "false" || value == "FALSE")) {
              src_obj.set_tinyint(
                  value == "true" || value == "TRUE" ? 1 : 0);
            } else {
              const ObString text(
                  static_cast<int32_t>(value.size()),
                  value.data());
              src_obj.set_varchar(text);
              src_obj.set_collation_type(
                  CS_TYPE_UTF8MB4_GENERAL_CI);
            }

            const ObObjType dst_type =
                expr->obj_meta_.get_type();

            ObAccuracy accuracy =
                ObAccuracy::DDL_DEFAULT_ACCURACY2[0][dst_type];

            ObCastCtx cast_ctx(
                &eval_ctx_.exec_ctx_.get_allocator(),
                &dtc_params,
                cast_mode,
                expr->obj_meta_.get_collation_type(),
                &accuracy);

            cast_ctx.runtime_ = &cast_runtime;

            if (src_obj.is_null()) {
              dst_obj.set_null();
            } else if (OB_FAIL(ObObjCaster::to_type(
                           dst_type,
                           expr->obj_meta_.get_collation_type(),
                           cast_ctx,
                           src_obj,
                           dst_obj))) {
            }

            if (OB_SUCC(ret)) {
              ObDatum &datum =
                  expr->locate_datum_for_write(eval_ctx_);

              if (OB_FAIL(datum.from_obj(dst_obj))) {
              } else {
                expr->set_evaluated_projected(eval_ctx_);
              }
            }
          }
        }
      }
    }
  }

  return ret;
}

int ObFileSqlAccessOp::inner_close()
{
  reader_.close();
  row_.clear();
  return OB_SUCCESS;
}

void ObFileSqlAccessOp::destroy()
{
  reader_.close();
  ObOperator::destroy();
}

} // namespace sql
} // namespace oceanbase
