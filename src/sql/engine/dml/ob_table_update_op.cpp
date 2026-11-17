/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SQL_ENG

#include "config_bridge.h"
#include "ob_table_update_op.h"
#include "sql/engine/dml/ob_dml_service.h"
#include "sql/resolver/ob_resolver_utils.h"
#include "share/inner_table/ob_inner_table_schema.h"

namespace oceanbase
{
using namespace common;
using namespace share;
using namespace storage;
namespace sql
{

static int ai_rowkey_predicate(const ObUpdCtDef &ctdef,
    const ObTableSchema &table, ObEvalCtx &eval_ctx, bool old_row, ObSqlString &predicate)
{
  int ret = OB_SUCCESS;
  const ExprFixedArray &row = old_row ? ctdef.old_row_ : ctdef.new_row_;
  for (int64_t i = 0; OB_SUCC(ret) && i < ctdef.dupd_ctdef_.rowkey_cnt_; ++i) {
    const ObColumnSchemaV2 *key_col = table.get_column_schema(ctdef.column_ids_.at(i));
    ObExpr *key_expr = row.at(i);
    ObDatum *datum = NULL;
    ObObj value;
    char literal[4096];
    int64_t pos = 0;
    if (OB_ISNULL(key_col) || OB_ISNULL(key_expr)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(key_expr->eval(eval_ctx, datum))) {
    } else if (OB_ISNULL(datum) || datum->is_null()) {
      ret = OB_INVALID_ARGUMENT;
    } else if (OB_FAIL(datum->to_obj(value, key_expr->obj_meta_,
                                   key_expr->obj_datum_map_))) {
    } else if (OB_FAIL(value.print_sql_literal(literal, sizeof(literal), pos))) {
    } else if (i > 0 && OB_FAIL(predicate.append(" AND "))) {
    } else if (OB_FAIL(ObResolverUtils::append_escaped_identifier(
                           predicate, key_col->get_column_name_str()))) {
    } else if (OB_FAIL(predicate.append(" = "))) {
    } else if (OB_FAIL(predicate.append(literal, pos))) {
    }
  }
  return ret;
}

static bool ai_dependency_changed(const ObColumnSchemaV2 &ai_col,
    const ObUpdCtDef &ctdef, ObEvalCtx &eval_ctx)
{
  bool changed = false;
  const ObString ids = ai_col.get_ai_dependency_ids();
  for (int64_t pos = 0; pos < ids.length() && !changed;) {
    uint64_t dependency_id = 0;
    while (pos < ids.length() && ids.ptr()[pos] >= '0' && ids.ptr()[pos] <= '9') {
      dependency_id = dependency_id * 10 + ids.ptr()[pos++] - '0';
    }
    if (pos < ids.length()) {
      ++pos;
    }
    for (int64_t i = 0; i < ctdef.assign_columns_.count() && !changed; ++i) {
      const int64_t index = ctdef.assign_columns_.at(i).projector_index_;
      if (index >= 0 && index < ctdef.column_ids_.count() &&
          ctdef.column_ids_.at(index) == dependency_id) {
        ObDatum *before = NULL;
        ObDatum *after = NULL;
        if (ctdef.old_row_.at(index)->eval(eval_ctx, before) != OB_SUCCESS ||
            ctdef.new_row_.at(index)->eval(eval_ctx, after) != OB_SUCCESS ||
            before == NULL || after == NULL || !ObDatum::binary_equal(*before, *after)) {
          changed = true;
        }
      }
    }
  }
  return changed;
}


OB_SERIALIZE_MEMBER((ObTableUpdateOpInput, ObTableModifyOpInput));

ObTableUpdateSpec::ObTableUpdateSpec(ObIAllocator &alloc, const ObPhyOperatorType type)
    : ObTableModifySpec(alloc, type),
      alloc_(alloc)
{
}

ObTableUpdateSpec::~ObTableUpdateSpec()
{
}

OB_DEF_SERIALIZE(ObTableUpdateSpec)
{
  int ret = OB_SUCCESS;
  int64_t tbl_cnt = upd_ctdefs_.count();
  BASE_SER((ObTableUpdateSpec, ObTableModifySpec));
  OB_UNIS_ENCODE(tbl_cnt);
  for (int64_t i = 0; OB_SUCC(ret) && i < tbl_cnt; ++i) {
    int64_t idx_cnt = upd_ctdefs_.at(i).count();
    OB_UNIS_ENCODE(idx_cnt);
    for (int64_t j = 0; OB_SUCC(ret) && j < idx_cnt; ++j) {
      ObUpdCtDef *upd_ctdef = upd_ctdefs_.at(i).at(j);
      if (OB_ISNULL(upd_ctdef)) {
        ret = OB_ERR_UNEXPECTED;
      }
      OB_UNIS_ENCODE(*upd_ctdef);
    }
  }
  return ret;
}

OB_DEF_DESERIALIZE(ObTableUpdateSpec)
{
  int ret = OB_SUCCESS;
  int64_t tbl_cnt = 0;
  BASE_DESER((ObTableUpdateSpec, ObTableModifySpec));
  OB_UNIS_DECODE(tbl_cnt);
  if (OB_SUCC(ret) && tbl_cnt > 0) {
    OZ(upd_ctdefs_.allocate_array(alloc_, tbl_cnt));
  }
  ObDMLCtDefAllocator<ObUpdCtDef> ctdef_allocator(alloc_);
  for (int64_t i = 0; OB_SUCC(ret) && i < tbl_cnt; ++i) {
    int64_t index_cnt = 0;
    OB_UNIS_DECODE(index_cnt);
    OZ(upd_ctdefs_.at(i).allocate_array(alloc_, index_cnt));
    for (int64_t j = 0; OB_SUCC(ret) && j < index_cnt; ++j) {
      ObUpdCtDef *upd_ctdef = ctdef_allocator.alloc();
      if (OB_ISNULL(upd_ctdef)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      }
      OB_UNIS_DECODE(*upd_ctdef);
      upd_ctdefs_.at(i).at(j) = upd_ctdef;
    }
  }
  return ret;
}

OB_DEF_SERIALIZE_SIZE(ObTableUpdateSpec)
{
  int64_t len = 0;
  int64_t tbl_cnt = upd_ctdefs_.count();
  BASE_ADD_LEN((ObTableUpdateSpec, ObTableModifySpec));
  OB_UNIS_ADD_LEN(tbl_cnt);
  for (int64_t i = 0; i < tbl_cnt; ++i) {
    int64_t index_cnt = upd_ctdefs_.at(i).count();
    OB_UNIS_ADD_LEN(index_cnt);
    for (int64_t j = 0; j < index_cnt; ++j) {
      ObUpdCtDef *upd_ctdef = upd_ctdefs_.at(i).at(j);
      if (upd_ctdef != nullptr) {
        OB_UNIS_ADD_LEN(*upd_ctdef);
      }
    }
  }
  return len;
}

ObTableUpdateOp::ObTableUpdateOp(ObExecContext &exec_ctx, const ObOpSpec &spec,
                                 ObOpInput *input)
  : ObTableModifyOp(exec_ctx, spec, input)
{
}

int ObTableUpdateOp::check_need_exec_single_row()
{
  int ret = OB_SUCCESS;
  ret = ObTableModifyOp::check_need_exec_single_row();
  if (OB_SUCC(ret) && !execute_single_row_) {
    for (int64_t i = 0; OB_SUCC(ret) && i < MY_SPEC.upd_ctdefs_.count() && !execute_single_row_; ++i) {
      const ObTableUpdateSpec::UpdCtDefArray &ctdefs = MY_SPEC.upd_ctdefs_.at(i);
      const ObUpdCtDef &upd_ctdef = *ctdefs.at(0);
      for (int64_t j = 0;
          OB_SUCC(ret) && !execute_single_row_ && j < upd_ctdef.trig_ctdef_.tg_args_.count();
          ++j) {
        const ObTriggerArg &tri_arg = upd_ctdef.trig_ctdef_.tg_args_.at(j);
        execute_single_row_ = tri_arg.is_execute_single_row();
      }
    }
  }
  return ret;
}

int ObTableUpdateOp::inner_open()
{
  int ret = OB_SUCCESS;
  NG_TRACE(update_open);
  //execute update with das
  if (OB_FAIL(ObTableModifyOp::inner_open())) {
  } else if (OB_UNLIKELY(MY_SPEC.upd_ctdefs_.empty())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_UNLIKELY(iter_end_)) {
    //do nothing
  } else if (OB_FAIL(inner_open_with_das())) {
  }
  NG_TRACE(update_end);
  return ret;
}

int ObTableUpdateOp::inner_close()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(close_table_for_each())) {
  }
  int  close_ret = ObTableModifyOp::inner_close();
  return (OB_SUCCESS == ret) ? close_ret : ret;
}

int ObTableUpdateOp::inner_rescan()
{
  int ret = OB_SUCCESS;
  if (!MY_SPEC.gi_above_) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "table update rescan");
  } else if (OB_FAIL(ObTableModifyOp::inner_rescan())) {
  } else if (OB_UNLIKELY(iter_end_)) {
    //do nothing
  } else if (OB_FAIL(close_table_for_each())) {
  } else if (OB_FAIL(open_table_for_each())) {
  }
  return ret;
}

int ObTableUpdateOp::inner_switch_iterator()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObTableModifyOp::inner_switch_iterator())) {
  }
  return ret;
}

int ObTableUpdateOp::write_row_to_das_buffer()
{
  int ret = OB_SUCCESS;
  ret = update_row_to_das();
  return ret;
}

OB_INLINE int ObTableUpdateOp::inner_open_with_das()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(open_table_for_each())) {
  } else {
    dml_rtctx_.set_pick_del_task_first();
  }
  return ret;
}

OB_INLINE int ObTableUpdateOp::open_table_for_each()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(upd_rtdefs_.allocate_array(ctx_.get_allocator(), MY_SPEC.upd_ctdefs_.count()))) {
  }
  trigger_clear_exprs_.reset();
  fk_checkers_.reset();
  for (int64_t i = 0; OB_SUCC(ret) && i < upd_rtdefs_.count(); ++i) {
    UpdRtDefArray &rtdefs = upd_rtdefs_.at(i);
    const ObTableUpdateSpec::UpdCtDefArray &ctdefs = MY_SPEC.upd_ctdefs_.at(i);
    if (OB_FAIL(rtdefs.allocate_array(ctx_.get_allocator(), ctdefs.count()))) {
    }
    for (int64_t j = 0; OB_SUCC(ret) && j < rtdefs.count(); ++j) {
      const ObUpdCtDef &upd_ctdef = *ctdefs.at(j);
      ObUpdRtDef &upd_rtdef = rtdefs.at(j);
      upd_rtdef.primary_rtdef_ = &rtdefs.at(0);
      if (OB_FAIL(ObDMLService::init_upd_rtdef(dml_rtctx_,
                                               upd_rtdef,
                                               upd_ctdef,
                                               trigger_clear_exprs_,
                                               fk_checkers_))) {
      }
    }
    if (OB_SUCC(ret) && !rtdefs.empty()) {
      ObUpdRtDef &primary_upd_rtdef = rtdefs.at(0);
      if (OB_SUCC(ret)) {
        //this table is being accessed by dml operator, mark its table location as writing
        primary_upd_rtdef.dupd_rtdef_.table_loc_->is_writing_ = true;
      }
    }
  }
  return ret;
}

OB_INLINE int ObTableUpdateOp::close_table_for_each()
{
  int ret = OB_SUCCESS;
  if (OB_SUCCESS == ctx_.get_errcode()) {
    for (int64_t i = 0; OB_SUCC(ret) && i < upd_rtdefs_.count(); ++i) {
      if (!upd_rtdefs_.at(i).empty()) {
        const ObUpdCtDef &primary_upd_ctdef = *MY_SPEC.upd_ctdefs_.at(i).at(0);
        ObUpdRtDef &primary_upd_rtdef = upd_rtdefs_.at(i).at(0);
        if (OB_NOT_NULL(primary_upd_rtdef.dupd_rtdef_.table_loc_)) {
          primary_upd_rtdef.dupd_rtdef_.table_loc_->is_writing_ = false;
        }
        if (!primary_upd_rtdef.has_table_cycle_ && primary_upd_ctdef.need_check_table_cycle_ && OB_FAIL(ObDMLService::delete_table_id_from_parent_table_set(dml_rtctx_, primary_upd_ctdef))) {
        }
      }
    }
  }
  //whether it is successful or not, needs to release rtdef
  for (int64_t i = 0; i < upd_rtdefs_.count(); ++i) {
    upd_rtdefs_.at(i).release_array();
  }
  return ret;
}

OB_INLINE int ObTableUpdateOp::calc_multi_tablet_id(const ObUpdCtDef &upd_ctdef,
                                                    ObExpr &part_id_expr,
                                                    ObTabletID &tablet_id,
                                                    bool check_exist)
{
  int ret = OB_SUCCESS;
  ObObjectID partition_id = OB_INVALID_ID;
  if (OB_FAIL(ObExprCalcPartitionBase::calc_part_and_tablet_id(&part_id_expr,
                                                               eval_ctx_,
                                                               partition_id,
                                                               tablet_id))) {
  } else if (check_exist && (!upd_ctdef.multi_ctdef_->hint_part_ids_.empty()
                             && !has_exist_in_array(
                               upd_ctdef.multi_ctdef_->hint_part_ids_, partition_id))) {
    ret = OB_PARTITION_NOT_MATCH;
  }
  return ret;
}

OB_INLINE int ObTableUpdateOp::calc_tablet_loc(const ObUpdCtDef &upd_ctdef,
                                               ObUpdRtDef &upd_rtdef,
                                               ObDASTabletLoc *&old_tablet_loc,
                                               ObDASTabletLoc *&new_tablet_loc)
{
  int ret = OB_SUCCESS;
  if (MY_SPEC.use_dist_das_) {
    ObTabletID old_tablet_id;
    ObTabletID new_tablet_id;
    if (upd_ctdef.multi_ctdef_ != nullptr) {
      ObExpr *calc_part_id_old = upd_ctdef.multi_ctdef_->calc_part_id_old_;
      ObExpr *calc_part_id_new = upd_ctdef.multi_ctdef_->calc_part_id_new_;
      if (calc_part_id_old != nullptr) {
        if (OB_FAIL(calc_multi_tablet_id(upd_ctdef, *calc_part_id_old, old_tablet_id))) {
        } else if (OB_FAIL(calc_multi_tablet_id(upd_ctdef, *calc_part_id_new, new_tablet_id, true))) {
        }
      }
    }
    if (OB_SUCC(ret)) {
      ObDASTableLoc &table_loc = *upd_rtdef.dupd_rtdef_.table_loc_;
      if (old_tablet_id == new_tablet_id) {
        if (OB_FAIL(DAS_CTX(ctx_).extended_tablet_loc(table_loc, old_tablet_id, old_tablet_loc))) {
        } else {
          new_tablet_loc = old_tablet_loc;
        }
      } else if (OB_FAIL(DAS_CTX(ctx_).extended_tablet_loc(table_loc, old_tablet_id, old_tablet_loc))) {
      } else if (OB_FAIL(DAS_CTX(ctx_).extended_tablet_loc(table_loc, new_tablet_id, new_tablet_loc))) {
      }
    }
  } else {
    //direct write delete row to storage
    old_tablet_loc = MY_INPUT.get_tablet_loc();
    new_tablet_loc = old_tablet_loc;
  }
  return ret;
}

OB_INLINE int ObTableUpdateOp::update_row_to_das()
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < MY_SPEC.upd_ctdefs_.count(); ++i) {
    const ObTableUpdateSpec::UpdCtDefArray &ctdefs = MY_SPEC.upd_ctdefs_.at(i);
    UpdRtDefArray &rtdefs = upd_rtdefs_.at(i);
    for (int64_t j = 0; OB_SUCC(ret) && j < ctdefs.count(); ++j) {
      //update each table with fetched row
      const ObUpdCtDef &upd_ctdef = *ctdefs.at(j);
      ObUpdRtDef &upd_rtdef = rtdefs.at(j);
      ObDASTabletLoc *old_tablet_loc = nullptr;
      ObDASTabletLoc *new_tablet_loc = nullptr;
      ObDMLModifyRowNode modify_row(this, &upd_ctdef, &upd_rtdef, ObDmlEventType::DE_UPDATING);
      bool is_skipped = false;
      ++upd_rtdef.cur_row_num_;
      if (OB_FAIL(ObDMLService::process_update_row(upd_ctdef, upd_rtdef, is_skipped, *this))) {
      } else if (OB_UNLIKELY(is_skipped)) {
        //this row has been skipped, so can not write to DAS buffer(include its global index)
        //so need to break this loop
        break;
      } else if (OB_FAIL(calc_tablet_loc(upd_ctdef, upd_rtdef, old_tablet_loc, new_tablet_loc))) {
      } else if (OB_FAIL(ObDMLService::update_row(upd_ctdef, upd_rtdef, old_tablet_loc, new_tablet_loc, dml_rtctx_,
                                                 modify_row.old_row_, modify_row.new_row_, modify_row.full_row_))) {
      } else if (need_after_row_process(upd_ctdef) && OB_FAIL(dml_modify_rows_.push_back(modify_row))) {
      } else {
        ++upd_rtdef.found_rows_;
      }
      if (OB_SUCC(ret) && j == 0 && upd_ctdef.is_primary_index_ &&
          upd_rtdef.is_row_changed_ && GET_MY_SESSION(ctx_)->is_user_session() &&
          !is_sys_table(upd_ctdef.dupd_ctdef_.index_tid_)) {
        ObSchemaGetterGuard *guard = ctx_.get_sql_ctx()->schema_guard_;
        const ObTableSchema *table = NULL;
        const ObDatabaseSchema *database = NULL;
        if (OB_ISNULL(guard)) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(guard->get_table_schema(
                            upd_ctdef.dupd_ctdef_.index_tid_, table))) {
        } else if (OB_ISNULL(table)) {
          ret = OB_SCHEMA_ERROR;
        } else {
          bool has_ai = false;
          for (int64_t k = 0; k < table->get_column_count() && !has_ai; ++k) {
            const ObColumnSchemaV2 *col = table->get_column_schema_by_idx(k);
            has_ai = col != NULL && col->is_ai_column();
          }
          if (has_ai) {
            ObSqlString old_key;
            ObSqlString new_key;
            ObSEArray<const ObColumnSchemaV2 *, 4> affected;
            if (OB_FAIL(ai_rowkey_predicate(upd_ctdef, *table, eval_ctx_, true, old_key))) {
            } else if (OB_FAIL(ai_rowkey_predicate(
                                   upd_ctdef, *table, eval_ctx_, false, new_key))) {
            } else {
              const bool key_changed = old_key.string() != new_key.string();
              for (int64_t k = 0; OB_SUCC(ret) && k < table->get_column_count(); ++k) {
                const ObColumnSchemaV2 *col = table->get_column_schema_by_idx(k);
                if (col != NULL && col->is_ai_column() &&
                    (key_changed || ai_dependency_changed(*col, upd_ctdef, eval_ctx_))) {
                  ret = affected.push_back(col);
                }
              }
            }
            if (OB_SUCC(ret) && !affected.empty()) {
              if (OB_FAIL(guard->get_database_schema(table->get_database_id(), database))) {
              } else if (OB_ISNULL(database)) {
                ret = OB_SCHEMA_ERROR;
              } else if (OB_FAIL(submit_all_dml_task())) {
              } else if (OB_FAIL(open_inner_conn())) {
              } else {
                ctx_.get_das_ctx().is_ai_column_task_ = true;
                if (OB_FAIL(begin_nested_session(false))) {
                } else {
                  GET_MY_SESSION(ctx_)->set_inner_session();
                  static uint64_t sequence = 0;
                  const uint64_t seq = ATOMIC_AAF(&sequence, 1);
                  const int64_t now = ObTimeUtility::current_time();
                  ObSqlString request;
                  ObSqlString old_literal;
                  ObSqlString new_literal;
                  ObSqlString update_sql;
                  ObSqlString delete_sql;
                  if (OB_FAIL(request.append_fmt("'%ld-%lu'", now, seq))) {
                  } else if (OB_FAIL(sql_append_hex_escape_str(old_key.string(), old_literal))) {
                  } else if (OB_FAIL(sql_append_hex_escape_str(new_key.string(), new_literal))) {
                  } else if (OB_FAIL(update_sql.append("UPDATE "))) {
                  } else if (OB_FAIL(ObResolverUtils::append_escaped_identifier(
                                           update_sql, database->get_database_name_str()))) {
                  } else if (OB_FAIL(update_sql.append("."))) {
                  } else if (OB_FAIL(ObResolverUtils::append_escaped_identifier(
                                           update_sql, table->get_table_name_str()))) {
                  } else if (OB_FAIL(update_sql.append(" SET "))) {
                  }
                  for (int64_t k = 0; OB_SUCC(ret) && k < affected.count(); ++k) {
                    const ObColumnSchemaV2 *col = affected.at(k);
                    if (k > 0 && OB_FAIL(update_sql.append(", "))) {
                    } else if (OB_FAIL(ObResolverUtils::append_escaped_identifier(
                                            update_sql, col->get_column_name_str()))) {
                    } else if (OB_FAIL(update_sql.append_fmt(
                        "=NULL, `__ai_state_%lu`=0, `__ai_request_%lu`=%s",
                        col->get_column_id(), col->get_column_id(), request.ptr()))) {
                    }
                  }
                  if (OB_SUCC(ret) && OB_FAIL(update_sql.append_fmt(" WHERE %s", new_key.ptr()))) {
                  } else if (OB_SUCC(ret) && OB_FAIL(delete_sql.append_fmt(
                      "DELETE FROM oceanbase.%s WHERE table_id=%lu AND rowkey_predicate=%s",
                      OB_ALL_AI_COLUMN_TASK_TNAME,
                      ObSchemaUtils::get_extract_schema_id(table->get_table_id()),
                      old_literal.ptr()))) {
                  } else if (OB_SUCC(ret) && OB_FAIL(execute_write(delete_sql.ptr()))) {
                  } else if (OB_SUCC(ret) && OB_FAIL(execute_write(update_sql.ptr()))) {
                  }
                  for (int64_t k = 0; OB_SUCC(ret) && k < affected.count(); ++k) {
                    const ObColumnSchemaV2 *col = affected.at(k);
                    ObSqlString sql;
                    if (OB_FAIL(sql.append_fmt(
                        "INSERT INTO oceanbase.%s (task_id, table_id, column_id, "
                        "rowkey_predicate, request_id, definition_version, state, "
                        "retry_count, max_retries, eligible_ts) VALUES "
                        "(UUID_SHORT(), %lu, %lu, %s, %s, %ld, 0, 0, %ld, NOW(6))",
                        OB_ALL_AI_COLUMN_TASK_TNAME,
                        ObSchemaUtils::get_extract_schema_id(table->get_table_id()),
                        col->get_column_id(), new_literal.ptr(), request.ptr(),
                        col->get_schema_version() > 0 ? col->get_schema_version()
                                                      : table->get_schema_version(),
                        col->get_ai_max_retries()))) {
                    } else if (OB_FAIL(execute_write(sql.ptr()))) {
                    }
                  }
                  GET_MY_SESSION(ctx_)->set_user_session();
                  int end_ret = end_nested_session();
                  if (OB_SUCC(ret)) {
                    ret = end_ret;
                  }
                }
                ctx_.get_das_ctx().is_ai_column_task_ = false;
              }
            }
          }
        }
      }
    } //end for global index ctdef loop
    if (OB_SUCC(ret)) {
      int64_t update_rows = rtdefs.at(0).is_row_changed_ ? 1 : 0;
      ObSQLSessionInfo *session = GET_MY_SESSION(ctx_);
      bool client_found_rows = session->get_capability().cap_flags_.OB_CLIENT_FOUND_ROWS;
      int64_t affected_rows = client_found_rows ? 1 /*found_rows*/ : update_rows;
      if (OB_FAIL(merge_implict_cursor(affected_rows, 1 /*found_rows*/, 1 /*match_rows*/, update_rows /*duplicated_rows*/))) {
      }
    }
  } // end for table ctdef loop

  return ret;
}

int ObTableUpdateOp::check_update_affected_row()
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < upd_rtdefs_.count(); ++i) {
    UpdRtDefArray &upd_rtdef_array = upd_rtdefs_.at(i);
    const ObUpdCtDef *primary_upd_ctdef = MY_SPEC.upd_ctdefs_.at(i).at(0);
    ObUpdRtDef &primary_upd_rtdef = upd_rtdef_array.at(0);
    int64_t primary_write_rows = 0;
    primary_write_rows += primary_upd_rtdef.dupd_rtdef_.affected_rows_;
    if (primary_upd_rtdef.ddel_rtdef_ != nullptr) {
      //update rows across partitions, need to add das delete op's affected rows
      primary_write_rows += primary_upd_rtdef.ddel_rtdef_->affected_rows_;
    }
    if (primary_upd_rtdef.dlock_rtdef_ != nullptr) {
      primary_write_rows += primary_upd_rtdef.dlock_rtdef_->affected_rows_;
    }
    for (int64_t j = 1; OB_SUCC(ret) && j < upd_rtdef_array.count(); ++j) {
      int64_t index_write_rows = 0;
      const ObUpdCtDef *index_upd_ctdef = MY_SPEC.upd_ctdefs_.at(i).at(j);
      ObUpdRtDef &index_upd_rtdef = upd_rtdef_array.at(j);
      index_write_rows += index_upd_rtdef.dupd_rtdef_.affected_rows_;
      if (index_upd_rtdef.ddel_rtdef_ != nullptr) {
        //update rows across partitions, need to add das delete op's affected rows
        index_write_rows += index_upd_rtdef.ddel_rtdef_->affected_rows_;
      }
      if (index_upd_rtdef.dlock_rtdef_ != nullptr) {
        index_write_rows += index_upd_rtdef.dlock_rtdef_->affected_rows_;
      }
      if (primary_write_rows != index_write_rows) {
        ret = OB_ERR_DEFENSIVE_CHECK;
        ObString func_name = ObString::make_string("check_update_affected_row");
        LOG_USER_ERROR(OB_ERR_DEFENSIVE_CHECK, func_name.length(), func_name.ptr());
        LOG_ERROR_RET(OB_ERR_DEFENSIVE_CHECK, "Fatal Error!!! data table update affected row is not match with index table",
                  K(ret), K(primary_write_rows), K(index_write_rows),
                  KPC(primary_upd_ctdef), K(primary_upd_rtdef),
                  KPC(index_upd_ctdef), K(index_upd_rtdef));
        LOG_DBA_ERROR_V2(OB_SQL_UPDATE_AFFECTED_ROW_FAIL, ret, "Attention!!!", "data table update affected row is not match with index table"
                  ", primary_write_rows is: ", primary_write_rows,
                  ", index_write_rows: ", index_write_rows);
      }
    }
    if (OB_SUCC(ret) && !primary_upd_ctdef->dupd_ctdef_.is_ignore_) {
      if (primary_upd_rtdef.found_rows_ != primary_write_rows) {
        ret = OB_ERR_DEFENSIVE_CHECK;
        ObString func_name = ObString::make_string("check_update_affected_row");
        LOG_USER_ERROR(OB_ERR_DEFENSIVE_CHECK, func_name.length(), func_name.ptr());
        LOG_ERROR_RET(OB_ERR_DEFENSIVE_CHECK, "Fatal Error!!! data table update affected row is not match with found rows",
                  K(ret), K(primary_write_rows), K(primary_upd_rtdef.found_rows_),
                  KPC(primary_upd_ctdef), K(primary_upd_rtdef));
        LOG_DBA_ERROR_V2(OB_SQL_UPDATE_AFFECTED_ROW_FAIL, ret, "Attention!!!", "data table update affected row is not match with found rows",
                  ", primary_write_rows is: ", primary_write_rows,
                  ", primary_found_rows is: ", primary_upd_rtdef.found_rows_);
      }
    }
  }
  return ret;
}

int ObTableUpdateOp::write_rows_post_proc(int last_errno)
{
  int ret = last_errno;
  if (iter_end_) {
    ObPhysicalPlanCtx *plan_ctx = GET_PHY_PLAN_CTX(ctx_);
    ObSQLSessionInfo *session = GET_MY_SESSION(ctx_);
    int64_t found_rows = 0;
    int64_t changed_rows = 0;
    for (int64_t i = 0; OB_SUCC(ret) && i < upd_rtdefs_.count(); ++i) {
      ObUpdRtDef &upd_rtdef = upd_rtdefs_.at(i).at(0);
      found_rows += upd_rtdef.found_rows_;
      changed_rows += upd_rtdef.dupd_rtdef_.affected_rows_;
      if (upd_rtdef.ddel_rtdef_ != nullptr) {
        //update rows across partitions, need to add das delete op's affected rows
        changed_rows += upd_rtdef.ddel_rtdef_->affected_rows_;
        //insert new row to das after old row has been deleted in storage
        //reference to: 
      }
    }
    if (OB_SUCC(ret)) {
      plan_ctx->add_row_matched_count(found_rows);
      plan_ctx->add_row_duplicated_count(changed_rows);
      plan_ctx->add_affected_rows(session->get_capability().cap_flags_.OB_CLIENT_FOUND_ROWS ?
                                  found_rows : changed_rows);
    }
    if (OB_SUCC(ret) && (config::_enable_defensive_check() > 0)) {
      if (OB_FAIL(check_update_affected_row())) {
      }
    }
  }

  return ret;
}

} // end namespace sql
} // end namespace oceanbase
