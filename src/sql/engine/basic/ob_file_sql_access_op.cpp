#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/basic/ob_file_sql_access_op.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/expr/ob_obj_cast_runtime.h"
#include "sql/resolver/ob_resolver_utils.h"
#include <glob.h>
#include <limits.h>
#include <sys/stat.h>

namespace oceanbase
{
using namespace common;

namespace sql
{

OB_SERIALIZE_MEMBER_INHERIT(ObFileSqlAccessSpec,
                            ObOpSpec,
                            file_path_,
                            file_format_,
                            column_exprs_,
                            column_names_);

ObFileSqlAccessOp::ObFileSqlAccessOp(
    ObExecContext &exec_ctx,
    const ObOpSpec &spec,
    ObOpInput *input)
  : ObOperator(exec_ctx, spec, input),
    reader_(),
    json_reader_(),
    row_()
{
}

int ObFileSqlAccessOp::inner_open()
{
  int ret = OB_SUCCESS;

  if (MY_SPEC.file_path_.empty() ||
      MY_SPEC.column_exprs_.empty() ||
      MY_SPEC.column_names_.count() != MY_SPEC.column_exprs_.count()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    const std::string path(
        MY_SPEC.file_path_.ptr(),
        MY_SPEC.file_path_.length());

    if (MY_SPEC.file_format_ == FILE_SQL_GLOB) {
      glob_paths_.clear();
      glob_index_ = 0;
      glob_t matches = {};
      const int glob_ret = ::glob(path.c_str(), 0, nullptr, &matches);
      if (glob_ret != 0 && glob_ret != GLOB_NOMATCH) {
        ret = OB_IO_ERROR;
      } else if (matches.gl_pathc > 100000) {
        ret = OB_SIZE_OVERFLOW;
      } else {
        ObString secure_file_priv;
        if (OB_FAIL(GET_MY_SESSION(ctx_)->get_secure_file_priv(secure_file_priv))) {
        } else {
          for (size_t i = 0; OB_SUCC(ret) && i < matches.gl_pathc; ++i) {
            char canonical[PATH_MAX];
            struct stat info;
            if (::realpath(matches.gl_pathv[i], canonical) == nullptr) {
              ret = OB_FILE_NOT_EXIST;
            } else if (OB_FAIL(ObResolverUtils::check_secure_path(
                           secure_file_priv, ObString(canonical)))) {
            } else if (::stat(canonical, &info) != 0) {
              ret = OB_IO_ERROR;
            } else if (S_ISREG(info.st_mode)) {
              glob_paths_.emplace_back(canonical);
            }
          }
        }
      }
      ::globfree(&matches);
    } else if (MY_SPEC.file_format_ == FILE_SQL_JSONL) {
      column_names_.clear();
      for (int64_t i = 0; i < MY_SPEC.column_names_.count(); ++i) {
        const ObString &name = MY_SPEC.column_names_.at(i);
        column_names_.emplace_back(name.ptr(), name.length());
      }
      if (json_reader_.open(path.c_str()) != 0) {
        ret = OB_IO_ERROR;
        LOG_ERROR("file SQL JSONL open failed", K(ret), "path", path.c_str());
      }
    } else if (MY_SPEC.file_format_ != FILE_SQL_CSV) {
      ret = OB_NOT_SUPPORTED;
    } else if (reader_.open(path.c_str()) != 0) {
      ret = OB_IO_ERROR;
      LOG_ERROR("file SQL CSV open failed", K(ret), "path", path.c_str());
    } else if (reader_.read_header(row_) != 0 ||
               row_.size() != static_cast<size_t>(MY_SPEC.column_exprs_.count())) {
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
  } else if (MY_SPEC.file_format_ == FILE_SQL_JSONL) {
    if (json_reader_.rescan() != 0) {
      ret = OB_IO_ERROR;
      LOG_ERROR("file SQL JSONL rescan failed", K(ret), K(json_reader_.line_number()));
    }
  } else if (MY_SPEC.file_format_ == FILE_SQL_GLOB) {
    glob_index_ = 0;
  } else if (reader_.read_header(row_) != 0) {
    ret = OB_ERR_UNEXPECTED;
    LOG_ERROR("file SQL CSV rescan failed", K(ret));
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

    int read_ret = 0;
    if (MY_SPEC.file_format_ == FILE_SQL_GLOB) {
      if (glob_index_ >= glob_paths_.size()) {
        read_ret = 1;
      } else {
        row_.assign(1, glob_paths_[glob_index_++]);
      }
    } else {
      read_ret = MY_SPEC.file_format_ == FILE_SQL_JSONL
          ? json_reader_.next_row(column_names_, row_, nulls_)
          : reader_.next_row(row_);
    }

    if (read_ret == 1) {
      ret = OB_ITER_END;
    } else if (read_ret != 0) {
      ret = OB_IO_ERROR;
      LOG_ERROR("file SQL read failed", K(ret), K(read_ret), K(MY_SPEC.file_format_),
                K(json_reader_.line_number()));
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

            if (MY_SPEC.file_format_ == FILE_SQL_JSONL ? nulls_[i] : value.empty()) {
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
  json_reader_.close();
  row_.clear();
  column_names_.clear();
  nulls_.clear();
  glob_paths_.clear();
  glob_index_ = 0;
  return OB_SUCCESS;
}

void ObFileSqlAccessOp::destroy()
{
  reader_.close();
  json_reader_.close();
  glob_paths_.clear();
  ObOperator::destroy();
}

} // namespace sql
} // namespace oceanbase
